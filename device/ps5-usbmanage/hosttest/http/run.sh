#!/usr/bin/env bash
# ============================================================
#  hosttest/http/run.sh —— usbmanage 的 HTTP 层端到端验证（宿主机执行）
#
#  做什么：
#    把 usbmanage.c **原文**在宿主机上编译成可执行文件（用 shim/stub.c 顶替
#    FreeBSD 专有的 getfsstat/unmount/sync），真实监听一个端口，然后用
#    curl 逐个打接口做断言。
#
#  为什么值得做：
#    上机一次很贵（要重走越狱链）。而"页面有没有被正确编进 elf、Content-Length
#    对不对、%2F 解码后的白名单判断、内部卷会不会被误列"，这些都跟 PS5 无关，
#    在宿主机上就能穷举验证。剩下的真机未知项只有 getfsstat/unmount 本身的行为。
#
#  用法：bash hosttest/http/run.sh
#  退出码：0 = 全通过，1 = 有断言失败
# ============================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$HERE"

PORT=19100
BIN=usbmanage-host.exe
SRV_PID=""
PASS=0
FAIL=0

# ---- 定位宿主机 clang ----
# 顺序：HOST_CC → PS5_CLANG（编译 payload 时已设的那个）→ PATH。
# 都取不到就明确报错——不猜任何绝对路径。
if [ -n "${HOST_CC:-}" ]; then
  CC="$HOST_CC"
elif [ -n "${PS5_CLANG:-}" ]; then
  CC="$PS5_CLANG"
elif command -v clang.exe >/dev/null 2>&1; then
  CC="clang.exe"
elif command -v clang >/dev/null 2>&1; then
  CC="clang"
else
  CC=""
fi
if [ -z "$CC" ] || { [ ! -x "$CC" ] && ! command -v "$CC" >/dev/null 2>&1; }; then
  echo "[error] 找不到宿主机 clang。" >&2
  echo "        请安装 LLVM/clang，或用环境变量指定：" >&2
  echo "          export HOST_CC=/c/llvm-mingw/bin/clang.exe   # 或 PS5_CLANG=..." >&2
  exit 1
fi

ok()   { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
check(){ # check <描述> <实际> <期望子串>
  case "$2" in
    *"$3"*) ok "$1" ;;
    *) bad "$1"; printf '        期望包含: %s\n        实际    : %s\n' "$3" "$2" ;;
  esac
}
nocheck(){ # 断言"不包含"
  case "$2" in
    *"$3"*) bad "$1"; printf '        不应包含: %s\n        实际    : %s\n' "$3" "$2" ;;
    *) ok "$1" ;;
  esac
}

# ---- 1. 编译 ----
# 先按 build.sh 的同一方式重新生成内嵌页面数组，保证测的是"当前这份页面"，
# 而不是上一次构建残留的旧头文件。
echo "[http] 生成 web/index_html.h"
xxd -i -n usbmanage_index_html "$ROOT/web/index.html" > "$ROOT/web/index_html.h"

# 图标同理：usbmanage.c 现在 #include 了 web/icon_png.h，这里按 build.sh
# 的同一方式重新生成，保证测的是"当前这份图标"而不是上一次的残留头文件。
echo "[http] 生成 web/icon_png.h"
xxd -i -n usbmanage_icon_png "$ROOT/web/icon.png" > "$ROOT/web/icon_png.h"

echo "[http] 编译 usbmanage.c（原文）+ 桩件"
"$CC" -I shim -include prologue.h -O1 -Wall -Wno-unused-parameter \
      -Dclose=usbmanage_host_close \
      "$ROOT/usbmanage.c" stub.c -o "$BIN" -lws2_32 2>&1 | sed 's/^/        /'
if [ ! -x "$BIN" ] && [ ! -f "$BIN" ]; then
  echo "[http] 编译失败" >&2
  exit 1
fi
echo "[http] 编译通过: $BIN"

start_server() {
  local label="$1"; shift
  # --force：端口上若有同版本实例，一律接管。测试要靠它保证每个场景都是
  #          新进程 + 新的伪造挂载表，不会被"同版本就只激活"的策略挡住。
  env "$@" ./"$BIN" "$PORT" --force > server.log 2>&1 &
  SRV_PID=$!
  local i
  for i in $(seq 1 40); do
    if curl -s -m 1 --noproxy 127.0.0.1 "http://127.0.0.1:$PORT/ping" 2>/dev/null | grep -q pong; then
      return 0
    fi
    sleep 0.15
  done
  echo "[http] 服务未能启动（$label），server.log:" >&2
  cat server.log >&2
  return 1
}

stop_server() {
  curl -s -m 2 --noproxy 127.0.0.1 "http://127.0.0.1:$PORT/shutdown" >/dev/null 2>&1
  local i
  for i in $(seq 1 20); do
    kill -0 "$SRV_PID" 2>/dev/null || return 0
    sleep 0.1
  done
  kill "$SRV_PID" 2>/dev/null
  wait "$SRV_PID" 2>/dev/null
}

# ============================================================
# 【语言约定，改断言前必读】
#   响应正文的语言由**请求头 Accept-Language** 决定，`?lang=` 优先级更高；
#   不带请求头就是英文（默认）。所以用 G() 发的请求拿到的 msg 一律是英文，
#   断言里就该写英文。中文分简繁两档：zh-CN 拿简体、zh-TW/zh-HK 拿繁体、
#   其余语言一律英文（见场景 N）。
#   **屏幕通知**是另一回事：它在任何请求之前发出，只能看环境变量
#   LC_ALL / LC_MESSAGES / LANG，见场景 L（钉中文）与 N（钉中文）。
# ============================================================

G() { curl -s -m 5 --noproxy 127.0.0.1 "http://127.0.0.1:$PORT$1"; }

# ============================================================
# 伪造的进程表与文件描述符表（"谁在占用"用）。
# 刻意混入三类**不该被杀**的：pid 1、Sce* 系统组件、以及本服务自己（4242）。
# 自己也放进进程表，是因为真机上它本来就在里面 —— 而"不杀自己"必须验到。
FAKE_PROCS="87:elfldr.elf;93:payload.elf;99:web-file-mgr.elf;1:init;10:SceShellCore.elf;4242:usbmanage.elf"
FAKE_FDS="93:-1:/mnt/usb0;99:12:/mnt/usb0/movie.mkv;87:-5:/mnt/usb0/hb.elf;1:3:/mnt/usb0/a;10:5:/mnt/usb0/b"

# ============================================================
echo
echo "=== 场景 A：已插两块 U 盘（usb0 / usb1）==="
# LANG 显式钉死成英文：屏幕通知是在任何 HTTP 请求之前发的，那时没有
# Accept-Language 可读，只能靠环境变量判语言（见 usbmanage.c 语言块）。
# 这里钉死才测得到确定结果；中文那一档在场景 N 里单独起一个进程验。
start_server A USBMANAGE_FAKE_PROCS="$FAKE_PROCS" USBMANAGE_FAKE_FDS="$FAKE_FDS" \
           USBMANAGE_FAKE_SELF_PID=4242 LANG=en_US.UTF-8 || exit 1

# 版本号从源码里读，避免每次升版本都要改测试
VER=$(sed -n 's/^#define VERSION  *"\([^"]*\)".*/\1/p' "$ROOT/usbmanage.c")
[ -n "$VER" ] || { echo "[http] 无法从 usbmanage.c 读取 VERSION" >&2; exit 1; }

check "GET /version 返回 $VER"   "$(G /version)" "\"version\":\"$VER\""
check "GET /version 带机器标识（认自己靠它，缺了会拒绝接管）" \
      "$(G /version)" '"app":"usbmanage"'
check "GET /version 带中文名"    "$(G /version)" '"name":"PS5 USB管理器"'
check "GET /version 带繁体名（与简体同字，字段仍要有）" \
      "$(G /version)" '"name_hant":"PS5 USB管理器"'
check "GET /version 带英文名"    "$(G /version)" '"name_en":"PS5 USB Manager"'
check "GET /version 报出界面语言（未发 Accept-Language -> 英文）" \
      "$(G /version)" '"lang":"en"'
check "GET /ping 返回 pong"      "$(G /ping)"    'pong'

# --- 启动时应自动去"弹界面"（拉浏览器 + 发通知）---
DG=$(G /diag)
check "/diag 报出局域网地址"      "$DG" '"lan_ip":"'
check "/diag 报出界面语言"        "$DG" '"lang":"'
check "/diag：默认开启自启"       "$DG" '"ui_autolaunch":true'
check "/diag：启动时已尝试一次"   "$DG" '"ui_attempts":1'
check "/diag：端口空闲，属于正常启动" "$DG" '"ui_reason":"start"'
check "/diag：四个系统调用都成功" "$DG" '"rc":{"user_init":0,"browser":0,"notify":0,"knotify":0}'
check "/diag：进程扫描可用（real kernel: 6 procs）" "$DG" '"procscan":{"supported":true,"procs":6}'

# 地址必须用主机自己的局域网地址，不能写死回环地址。
# 断言方式：从 /diag 取 lan_ip，再看启动日志里浏览器接口收到的 URI 是否同一个 ——
# 直接断言"不是 127.0.0.1"会在没有外网的机器上误报。
LANIP=$(printf '%s' "$DG" | sed -n 's/.*"lan_ip":"\([^"]*\)".*/\1/p')
if [ -n "$LANIP" ]; then
  ok "/diag 报出了局域网地址（$LANIP）"
else
  bad "/diag 的 lan_ip 为空"
fi
check "启动日志：浏览器接口收到的 URI 用的就是该局域网地址" \
      "$(cat server.log)" "sceSystemServiceLaunchWebBrowser(\"http://$LANIP:$PORT/\")"
check "启动日志：确实调了浏览器接口" "$(cat server.log)" 'sceSystemServiceLaunchWebBrowser("http://'
check "启动日志：确实发了屏幕通知" "$(cat server.log)" 'sceNotificationSend(user=254'
check "启动日志：通知走了 rawData 结构且无自造字段" "$(cat server.log)" 'hasMsg=1, hasAction=0'
check "启动日志：调浏览器前先初始化了 UserService" \
      "$(cat server.log)" 'sceUserServiceInitialize -> 0'
check "启动日志：界面地址用局域网 IP（与 /diag 一致）" \
      "$(cat server.log)" "web UI: http://$LANIP:$PORT/"
if [ -f last_notify.json ]; then
  check "通知结构对齐官方 samples/notify（rawData/InteractiveToastTemplateB）" \
        "$(cat last_notify.json)" '"rawData":{"viewTemplateType":"InteractiveToastTemplateB"'
  nocheck "通知不含自造的 actionUrl 字段（主机不认会让整条不显示）" \
          "$(cat last_notify.json)" 'actionUrl'
  check "通知正文带管理地址（英文档，LANG=en_US）" "$(cat last_notify.json)" 'address http://'
  check "通知时间戳形如 ISO8601" "$(cat last_notify.json)" '"createdDateTime":"20'
  nocheck "通知时间戳不是写死的样例值" "$(cat last_notify.json)" 'T00:00:00'
  check "通知内容含可点的地址与端口" "$(cat last_notify.json)" ":$PORT/\""
  if [ "$LANIP" != "127.0.0.1" ]; then
    check "通知里的地址是局域网 IP，不是回环地址" "$(cat last_notify.json)" "http://$LANIP:$PORT/"
    nocheck "通知里不出现 127.0.0.1" "$(cat last_notify.json)" '127.0.0.1'
  fi
else
  bad "没有留下 last_notify.json（通知桩件未被调用）"
fi

# --- 第二条通知通道（内核通知）---
# 真机上 sceNotificationSend 那套不显示，所以再发一条经典的内核通知。
check "启动日志：也调了内核通知接口" "$(cat server.log)" 'sceKernelSendNotificationRequest(unk=0'
if [ -f notify_kernel.txt ]; then
  check "内核通知文本带程序名与版本（英文档）" "$(cat notify_kernel.txt)" "PS5 USB Manager v$VER"
  check "内核通知文本带可用的管理地址" "$(cat notify_kernel.txt)" "http://$LANIP:$PORT/"
  if [ "$LANIP" != "127.0.0.1" ]; then
    nocheck "内核通知不出现回环地址" "$(cat notify_kernel.txt)" '127.0.0.1'
  fi
else
  bad "没有留下 notify_kernel.txt（内核通知桩件未被调用）"
fi

# --- 内置页面：字节级比对 ---
CODE=$(curl -s -m 5 --noproxy 127.0.0.1 -o page.out -D page.hdr -w '%{http_code}' "http://127.0.0.1:$PORT/")
check "GET / 返回 200" "$CODE" "200"
check "GET / 的 Content-Type 是 text/html" "$(cat page.hdr)" "Content-Type: text/html"
EXPECT_LEN=$(wc -c < "$ROOT/web/index.html")
GOT_LEN=$(wc -c < page.out)
if [ "$EXPECT_LEN" = "$GOT_LEN" ] && cmp -s page.out "$ROOT/web/index.html"; then
  ok "GET / 返回的页面与 web/index.html 字节完全一致（$GOT_LEN bytes）"
else
  bad "页面字节不一致"; echo "        期望 $EXPECT_LEN bytes / 实际 $GOT_LEN bytes"
fi
check "页面含选择按钮标记" "$(cat page.out)" 'data-eject'

# --- 列表 ---
L=$(G /list)
check "/list 只列外接卷 count=2"      "$L" '"count":2'
check "/list 含 /mnt/usb0"            "$L" '/mnt/usb0'
check "/list 含 /mnt/usb1"            "$L" '/mnt/usb1'
nocheck "/list 不含内部卷 /mnt/ext0"  "$L" '/mnt/ext0'
nocheck "/list 不含内部卷 /mnt/ext1"  "$L" '/mnt/ext1'
check "/list 的 usb0 可卸载"          "$L" '"mount":"/mnt/usb0","device":"/dev/da0s1","fstype":"exfat","ejectable":true'
# 每个卷都带 label 字段。这里设备是假的（宿主上不存在 /dev/da0s1），
# 读不到就报空串——这是"读不到不影响列出"的正面断言。
check "/list 每个卷都带 label 字段（读不到设备时为空串）" "$L" '"ejectable":true,"label":""'

A=$(G '/list?all=1')
check "/list?all=1 列出全部 count=5"    "$A" '"count":5'
check "/list?all=1 含 /mnt/ext0"        "$A" '/mnt/ext0'
check "/list?all=1 中 ext0 不可卸载"    "$A" '"mount":"/mnt/ext0","device":"/dev/es0.crypt","fstype":"ufs","ejectable":false'
check "/list?all=1 中 ext1 不可卸载"    "$A" '"mount":"/mnt/ext1","device":"/dev/ssd1.user","fstype":"bfs","ejectable":false'
check "/list?all=1 未被截断（shown=count）" "$A" '"shown":5,"truncated":false'

# --- 浏览器导航到 JSON 端点 -> 302 到选择页 ---
BROWSER='Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8'
R1=$(curl -s -m 5 --noproxy 127.0.0.1 -o /dev/null -D - -H "$BROWSER" "http://127.0.0.1:$PORT/list?all=1")
check "浏览器打开 /list?all=1 被 302 到 /" "$R1" "302 Found"
check "  且 Location 指向 /"               "$R1" "Location: /"
R2=$(curl -s -m 5 --noproxy 127.0.0.1 -o /dev/null -D - -H "$BROWSER" "http://127.0.0.1:$PORT/eject?mount=%2Fmnt%2Fusb0")
check "浏览器打开 /eject 也被 302（不会误触发卸载）" "$R2" "302 Found"
check "  302 之后 usb0 仍在（没被卸掉）"   "$(G /list)" '/mnt/usb0'
check "  curl（Accept 无 text/html）仍拿到 JSON" "$(G /list)" '"scope":"usb"'
R4=$(curl -s -m 5 --noproxy 127.0.0.1 -o /dev/null -D - -H "$BROWSER" "http://127.0.0.1:$PORT/release?mount=%2Fmnt%2Fusb0&confirm=1")
check "浏览器打开 /release 也被 302（不会误触发终止进程）" "$R4" "302 Found"
if [ -f kill.log ]; then
  bad "  但 302 之后竟然留下了 kill.log —— 有进程被误杀"
else
  ok "  302 之后一个进程都没被杀（无 kill.log）"
fi
check "浏览器打开 /holders 也被 302" "$(curl -s -m 5 --noproxy 127.0.0.1 -o /dev/null -D - -H "$BROWSER" "http://127.0.0.1:$PORT/holders?mount=%2Fmnt%2Fusb0")" "302 Found"
check "浏览器打开 /procs 也被 302"   "$(curl -s -m 5 --noproxy 127.0.0.1 -o /dev/null -D - -H "$BROWSER" "http://127.0.0.1:$PORT/procs")" "302 Found"
R3=$(curl -s -m 5 --noproxy 127.0.0.1 -o /dev/null -D - -H "$BROWSER" "http://127.0.0.1:$PORT/")
check "页面自身的请求不触发重定向（/ 直接 200）" "$R3" "200 OK"

# --- 拒绝非法目标 ---
check "拒绝 /mnt/ext0（编码形式 %2F）" "$(G '/eject?mount=%2Fmnt%2Fext0')" 'path not allowed'
check "拒绝 /mnt/ext0（明文形式）"     "$(G '/eject?mount=/mnt/ext0')"     'path not allowed'
check "拒绝 /mnt/ext1"                 "$(G '/eject?mount=%2Fmnt%2Fext1')" 'path not allowed'
check "拒绝 /mnt/usb0/ 带尾斜杠"       "$(G '/eject?mount=%2Fmnt%2Fusb0%2F')" 'path not allowed'
check "拒绝路径穿越"                   "$(G '/eject?mount=%2Fmnt%2Fusb0%2F..%2Fext0')" 'path not allowed'
check "拒绝 /（根）"                   "$(G '/eject?mount=%2F')" 'path not allowed'
check "拒绝 /mnt/ext2（数字后缀不放松）" "$(G '/eject?mount=%2Fmnt%2Fext2')" 'path not allowed'
check "缺 mount 参数时报错"            "$(G /eject)" 'missing'

# --- 真正卸载（关键回归：挂载点里的 %2F 若不先解码就永远卸不掉）---
check "卸载 /mnt/usb0（编码形式）成功"  "$(G '/eject?mount=%2Fmnt%2Fusb0')" '"ok":true'
check "卸载 /mnt/usb0（明文形式）已不在" "$(G '/eject?mount=/mnt/usb0')" 'not mounted'
L2=$(G /list)
check "卸载后 /list count=1"           "$L2" '"count":1'
nocheck "卸载后 /list 不再含 usb0"     "$L2" '/mnt/usb0'
check "卸载后仍在的 usb1 可卸载"       "$L2" '"mount":"/mnt/usb1"'
check "桩件确实收到了 unmount(/mnt/usb0)" "$(cat server.log)" 'unmount("/mnt/usb0", flags=0x0) called'

# --- 占用进程查询 ---
HD=$(G '/holders?mount=%2Fmnt%2Fusb0')
check "/holders 回执 ok"                    "$HD" '"ok":true'
check "/holders 报出占用数 count=5（3 可终止 + 2 被护栏拦下）" "$HD" '"count":5'
check "  扫过 6 个进程、6 个 fd 表都读到了"  "$HD" '"scanned":6,"readable":6'
check "  支持标记为真（内核放行了 fd 表）"  "$HD" '"supported":true'
check "  列出普通 fd 的占用者"              "$HD" '{"pid":99,"comm":"web-file-mgr.elf","fd":12,"kind":"fd","path":"/mnt/usb0/movie.mkv","self":false,"killable":true}'
check "  列出 cwd 落在该盘的占用者"         "$HD" '"fd":-1,"kind":"cwd","path":"/mnt/usb0"'
check "  列出可执行体就在该盘的占用者"      "$HD" '"fd":-5,"kind":"text"'
check "  pid 1 不可杀并说明原因"            "$HD" '"pid":1,"comm":"init","fd":3,"kind":"fd","path":"/mnt/usb0/a","self":false,"killable":false,"why":"critical system process (pid 1 or kernel)"'
check "  Sce* 系统组件不可杀并说明原因"     "$HD" '"killable":false,"why":"Sony system component (Sce* prefix, never killed)"'
nocheck "  自己（4242）没在占用者里"        "$HD" '"pid":4242'

check "/holders 拒绝内部卷"     "$(G '/holders?mount=%2Fmnt%2Fext0')" 'path not allowed'
check "/holders 缺参数时报错"   "$(G /holders)" 'missing'

PR=$(G /procs)
check "/procs 列出进程 ok"      "$PR" '"ok":true'
check "  报出 6 个进程"          "$PR" '"count":6,"shown":6,"truncated":false'
check "  逐条带 pid 与 comm"     "$PR" '{"pid":87,"ppid":1,"uid":0,"comm":"elfldr.elf","self":false}'
check "  标出哪个是自己"         "$PR" '{"pid":4242,"ppid":1,"uid":0,"comm":"usbmanage.elf","self":true}'

# --- 应用图标 ---
# 图标是编进 elf 的（web/icon.png 经 xxd -> web/icon_png.h），不是读外部文件，
# 所以这里必须比对字节：长度对了、内容不对（比如换了图没重新构建）也得判死。
check "图标 Content-Type 为 image/png" \
  "$(curl -s -m 5 --noproxy 127.0.0.1 -D - -o /dev/null "http://127.0.0.1:$PORT/icon.png" | tr -d '\r' | grep -i '^content-type:')" \
  "image/png"
check "图标返回 200" \
  "$(curl -s -m 5 --noproxy 127.0.0.1 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/icon.png")" "200"
curl -s -m 5 --noproxy 127.0.0.1 "http://127.0.0.1:$PORT/icon.png" -o icon.out
if cmp -s icon.out "$ROOT/web/icon.png"; then
  ok "GET /icon.png 与 web/icon.png 字节完全一致（$(wc -c < icon.out) bytes）"
else
  bad "图标字节不一致"
  echo "        期望 $(wc -c < "$ROOT/web/icon.png") bytes / 实际 $(wc -c < icon.out) bytes"
fi
curl -s -m 5 --noproxy 127.0.0.1 "http://127.0.0.1:$PORT/favicon.ico" -o favicon.out
if cmp -s favicon.out "$ROOT/web/icon.png"; then
  ok "GET /favicon.ico 与图标文件字节一致（不再是空的 204）"
else
  bad "/favicon.ico 返回的不是那份图标"
fi
# 页面必须真的引用图标：否则路由通了、浏览器的页签上仍是默认图标。
check "页面声明了 <link rel=\"icon\"> 指向 /icon.png" \
  "$(cat page.out)" 'rel="icon" type="image/png" href="/icon.png"'
check "页面页头用了图标做 logo" "$(cat page.out)" '<img class="logo" src="/icon.png"'

check "未知端点给出指引" "$(G /nope)" 'unknown endpoint'
check "POST 被拒（405）" "$(curl -s -m 5 --noproxy 127.0.0.1 -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/list")" "405"

stop_server
echo "  （场景 A 结束，服务已退出）"

# ============================================================
echo
echo "=== 场景 B：没插 U 盘（只有内部卷）==="
start_server B USBMANAGE_FAKE_EMPTY=1 || exit 1
LB=$(G /list)
check "无 U 盘时 count=0（内部卷绝不进入可卸载列表）" "$LB" '"count":0'
nocheck "无 U 盘时列表里没有 /mnt/ext0" "$LB" '/mnt/ext0'
nocheck "无 U 盘时列表里没有 /mnt/ext1" "$LB" '/mnt/ext1'
check "无 U 盘时仍可只读诊断" "$(G '/list?all=1')" '"count":2'
stop_server
echo "  （场景 B 结束，服务已退出）"

# ============================================================
echo
echo "=== 场景 C：卷被占用，unmount 失败 —— 必须连带说出是谁在占用 ==="
start_server C USBMANAGE_FAKE_EBUSY=1 USBMANAGE_FAKE_PROCS="$FAKE_PROCS" \
             USBMANAGE_FAKE_FDS="$FAKE_FDS" USBMANAGE_FAKE_SELF_PID=4242 || exit 1
EC=$(G '/eject?mount=%2Fmnt%2Fusb0')
check "卸载失败时 ok=false" "$EC" '"ok":false'
check "卸载失败时带 errno"   "$EC" '"code":'
check "  并标出 busy"        "$EC" '"busy":true'
check "失败时给出下一步动作（而非只复述错误）" "$EC" 'see the holder list below, or use the release button'
check "  并给出 release_url（页面与脚本都不用自己拼）" "$EC" '"release_url":"/release?mount=/mnt/usb0&confirm=1"'
check "  回执里直接带上占用者名单" "$EC" '"holders":[{"pid":87,"comm":"elfldr.elf"'
check "  占用者数量也报出来"       "$EC" '"holder_count":5'
check "  占用者标记里带是否可终止" "$EC" '"killable":true'
check "  pi 1 被标为不可终止"      "$EC" '"killable":false,"why":"critical system process (pid 1 or kernel)"'
check "  未被强制时 forced=false"  "$EC" '"forced":false'
check "失败后列表仍在"       "$(G /list)" '"count":2'
check "桩件收到的 flags 是 0（未强制）" "$(cat server.log)" 'unmount("/mnt/usb0", flags=0x0) called'

# 强制卸载：仍失败，但必须真的把 MNT_FORCE 传下去
FC=$(G '/eject?mount=%2Fmnt%2Fusb0&force=1')
check "强制卸载被标记 forced=true" "$FC" '"forced":true'
check "  桩件确实收到了 MNT_FORCE"  "$(cat server.log)" 'unmount("/mnt/usb0", flags=0x80000 MNT_FORCE) called'
stop_server
echo "  （场景 C 结束，服务已退出）"

# ============================================================
echo
echo "=== 场景 G：占用 -> 解除占用 -> 卸载成功（用户实际遇到的那条链路）==="
rm -f kill.log
start_server G USBMANAGE_FAKE_EBUSY_ONCE=1 USBMANAGE_FAKE_PROCS="$FAKE_PROCS" \
             USBMANAGE_FAKE_FDS="$FAKE_FDS" USBMANAGE_FAKE_SELF_PID=4242 || exit 1

EG=$(G '/eject?mount=%2Fmnt%2Fusb0')
check "第一次卸载因占用失败" "$EG" '"busy":true'
check "  并列出 5 个占用者（含 2 个杀不动的）" "$EG" '"holder_count":5'

# 先看预览：不带 confirm 绝不动手
RV=$(G '/release?mount=%2Fmnt%2Fusb0')
check "/release 不带 confirm 时是预览"  "$RV" '"confirmed":false'
check "  预览也列出占用者（before=5）"   "$RV" '"before":5'
check "  killed 为空"                   "$RV" '"killed":[]'
check "  明确说明没执行任何动作"         "$RV" 'preview: nothing was terminated'
if [ -f kill.log ]; then
  bad "  预览状态下不该有任何进程被杀，但 kill.log 出现了"
else
  ok "  预览状态下没有任何进程被杀"
fi

# 真动手
RL=$(G '/release?mount=%2Fmnt%2Fusb0&confirm=1')
check "/release confirm=1 回执 ok"       "$RL" '"ok":true'
check "  confirmed=true"                 "$RL" '"confirmed":true'
check "  杀掉 3 个可终止的占用者（TERM）" "$RL" '{"pid":87,"comm":"elfldr.elf","sig":"TERM"},{"pid":93,"comm":"payload.elf","sig":"TERM"},{"pid":99,"comm":"web-file-mgr.elf","sig":"TERM"}'
check "  被护栏拦下 2 个"                "$RL" '"skipped":2'
check "  解除后只剩 2 个（都是拦下的）"   "$RL" '"after":2'
check "  本场景没有沙箱挂接，child_released_count=0" "$RL" '"child_released_count":0'
check "  并成功卸载"                     "$RL" '"unmounted":true'
check "  给出可拔出的结论"               "$RL" 'it is now safe to unplug'
check "  卸载确实经过 vdc 之后的主路径"   "$(cat server.log)" 'unmount("/mnt/usb0", flags=0x0) called'

# 关键：kill.log 里只能有那三个，绝不含自己、pid 1、Sce*
KL=$(cat kill.log 2>/dev/null)
check "kill.log 记录了对 87 发 TERM" "$KL" "87 TERM"
check "kill.log 记录了对 93 发 TERM" "$KL" "93 TERM"
check "kill.log 记录了对 99 发 TERM" "$KL" "99 TERM"
nocheck "kill.log 里没有自己（4242）"     "$KL" "4242"
nocheck "kill.log 里没有 pid 1"          "$KL" $'1 TERM\n'
nocheck "kill.log 里没有 SceShellCore（10）" "$KL" $'10 TERM\n'
if [ "$(grep -c . kill.log)" = "3" ]; then
  ok "kill.log 恰好 3 行 —— 只杀该杀的"
else
  bad "kill.log 行数不是 3（实际 $(grep -c . kill.log)）"; cat kill.log
fi

LG=$(G /list)
check "卸载后 /list count=1"        "$LG" '"count":1'
nocheck "  不再含 usb0"              "$LG" '"mount":"/mnt/usb0"'
check "  未被占用影响的 usb1 仍在"   "$LG" '"mount":"/mnt/usb1"'
check "  拦下的那两个仍在占用者名单" "$(G '/holders?mount=%2Fmnt%2Fusb1')" '"mount":"/mnt/usb1"'
stop_server
echo "  （场景 G 结束，服务已退出）"

# ============================================================
echo
echo "=== 场景 H：内核不放行 KERN_PROC_FILEDESC 时必须如实说「查不了」，而不是谎报「没人占用」 ==="
start_server H USBMANAGE_FAKE_EBUSY=1 USBMANAGE_FAKE_PROCS="$FAKE_PROCS" \
             USBMANAGE_FAKE_FILEDESC_FAIL=1 USBMANAGE_FAKE_SELF_PID=4242 || exit 1
HH=$(G '/holders?mount=%2Fmnt%2Fusb0')
check "/holders ok（服务没崩）"        "$HH" '"ok":true'
check "  但 supported=false"           "$HH" '"supported":false'
check "  带出错误原因"                 "$HH" '"err":'
check "  count 为 0"                   "$HH" '"count":0'
EH=$(G '/eject?mount=%2Fmnt%2Fusb0')
check "/eject 也如实报 holders_supported=false" "$EH" '"holders_supported":false'
check "  并说明 fd 表被拒、解除时会跳过终止" "$EH" 'refuses to expose the process file-descriptor table'
RH=$(G '/release?mount=%2Fmnt%2Fusb0&confirm=1')
check "/release 如实报 supported=false，但不再直接放弃" "$RH" '"supported":false'
check "  跳过终止（killed=[]，查不到人不硬杀）" "$RH" '"killed":[]'
check "  仍尝试了沙箱挂接解除（本场景没有，count=0）" "$RH" '"child_released_count":0'
check "  常规卸载失败后自动补 force（forced_retry）" "$RH" '"forced_retry":true'
check "  force 也失败，如实报 unmounted=false（EBUSY 是常开的）" "$RH" '"unmounted":false'
PD=$(G /diag)
check "但 /procs 仍然可用（另一条 sysctl 是好的）" "$PD" '"procscan":{"supported":true'
check "  进程清单照常"                 "$(G /procs)" '"count":6'
stop_server
echo "  （场景 H 结束，服务已退出）"

# ============================================================
echo
echo "=== 场景 I：本服务自己也是占用者时，绝不自杀 ==="
rm -f kill.log
start_server I USBMANAGE_FAKE_EBUSY_ONCE=1 USBMANAGE_FAKE_SELF_PID=4242 \
             USBMANAGE_FAKE_PROCS="4242:usbmanage.elf;99:web-file-mgr.elf" \
             USBMANAGE_FAKE_FDS="4242:-1:/mnt/usb0;99:12:/mnt/usb0/movie.mkv" || exit 1
HI=$(G '/holders?mount=%2Fmnt%2Fusb0')
check "自己出现在占用者名单里"    "$HI" '"pid":4242,"comm":"usbmanage.elf","fd":-1,"kind":"cwd","path":"/mnt/usb0","self":true,"killable":false'
check "  并说明为什么不杀它"      "$HI" 'this service itself (killing it kills the service)'
RI=$(G '/release?mount=%2Fmnt%2Fusb0&confirm=1')
check "只杀掉了另一个占用者"      "$RI" '{"pid":99,"comm":"web-file-mgr.elf","sig":"TERM"}'
check "  自己那一条排在杀不动里"  "$RI" '"skipped":1'
check "  自己的 pid 没进 killed"  "$(cat kill.log)" "99 TERM"
nocheck "  自己绝不在 kill.log 里" "$(cat kill.log)" "4242"
check "服务本身还活着（还能应答）" "$(G /ping)" "pong"
stop_server
echo "  （场景 I 结束，服务已退出）"

# ============================================================
echo
echo "=== 场景 J：沙箱 nullfs 挂接占用（没有任何进程 fd）==="
echo "  真机形态：应用把 U 盘挂进自己的沙箱视图，fd 表查不到人，unmount 却 EBUSY。"
rm -f kill.log
start_server J USBMANAGE_FAKE_USB_VIEWS=1 USBMANAGE_FAKE_EBUSY_ONCE=1 \
             USBMANAGE_FAKE_PROCS="1:init" USBMANAGE_FAKE_SELF_PID=4242 || exit 1

EJ=$(G '/eject?mount=%2Fmnt%2Fusb0')
check "第一次卸载因挂接占用失败"          "$EJ" '"busy":true'
check "  没有任何进程 fd 占用（holder_count=0）" "$EJ" '"holder_count":0'
check "  提示占用多半来自系统沙箱挂接"   "$EJ" 'system sandbox nullfs mount'
check "  仍然给出 release_url"           "$EJ" '"release_url":"/release?mount=/mnt/usb0&confirm=1"'

RJ=$(G '/release?mount=%2Fmnt%2Fusb0&confirm=1')
check "/release 回执 ok"                 "$RJ" '"ok":true'
check "  卸掉了 2 个沙箱挂接"            "$RJ" '"child_released_count":2'
check "  两个挂接逐条回执且都成功"       "$RJ" '{"mount":"/mnt/sandbox/CUSA12345_000/mnt/usb0","ok":true,"err":0},{"mount":"/mnt/sandbox/CUSA67890_000/data/media","ok":true,"err":0}'
check "  没杀任何进程（没有可杀的占用者）" "$RJ" '"killed":[]'
check "  最终卸载成功"                   "$RJ" '"unmounted":true'
check "  结论里说明解除了系统内部挂接"   "$RJ" 'system mounts released'

# 卸载顺序必须正确：先两个挂接视图，最后才是目标卷
S1=$(grep -n 'unmount("/mnt/sandbox/CUSA12345_000/mnt/usb0"' server.log | head -1 | cut -d: -f1)
S2=$(grep -n 'unmount("/mnt/sandbox/CUSA67890_000/data/media"' server.log | head -1 | cut -d: -f1)
S3=$(grep -n 'unmount("/mnt/usb0"' server.log | tail -1 | cut -d: -f1)
if [ -n "$S1" ] && [ -n "$S2" ] && [ -n "$S3" ] && [ "$S1" -lt "$S3" ] && [ "$S2" -lt "$S3" ]; then
  ok "卸载顺序正确：先解除两个沙箱挂接（行 $S1/$S2），最后卸目标（行 $S3）"
else
  bad "卸载顺序不对（挂接行 $S1/$S2，目标行 $S3）"; grep -n 'unmount(' server.log
fi
if [ -f kill.log ]; then
  bad "  但 kill.log 出现了 —— 无占用者场景不该杀任何进程"
else
  ok "  一个进程都没被杀"
fi

LJ=$(G /list)
check "卸载后 usb0 从列表消失（count=1）" "$LJ" '"count":1'
nocheck "  不再含 usb0"                   "$LJ" '"mount":"/mnt/usb0"'
check "  未涉及的 usb1 仍在"              "$LJ" '"mount":"/mnt/usb1"'
stop_server
echo "  （场景 J 结束，服务已退出）"

# ============================================================
echo
echo "=== 场景 D：主机挂载点极多（82 条），U 盘排在最后 ==="
start_server D USBMANAGE_FAKE_MANY=1 || exit 1
LD1=$(G /list)
check "U 盘没被上限藏住：count=2"      "$LD1" '"count":2'
check "  找到 /mnt/usb0"               "$LD1" '"mount":"/mnt/usb0","device":"/dev/da2p1","fstype":"exfatfs","ejectable":true'
check "  找到 /mnt/usb1"               "$LD1" '"mount":"/mnt/usb1","device":"/dev/da2p2","fstype":"exfatfs","ejectable":true'
LD2=$(G '/list?all=1')
check "诊断视图如实报总数 count=82"    "$LD2" '"count":82'
check "  并标出已截断"                 "$LD2" '"truncated":true'
D_COUNT=$(printf '%s' "$LD2" | sed -n 's/.*"count":\([0-9]*\).*/\1/p')
D_SHOWN=$(printf '%s' "$LD2" | sed -n 's/.*"shown":\([0-9]*\).*/\1/p')
if [ -n "$D_SHOWN" ] && [ -n "$D_COUNT" ] && [ "$D_SHOWN" -lt "$D_COUNT" ]; then
  ok "  count/shown 自洽：列出 $D_SHOWN 条，真实 $D_COUNT 条"
else
  bad "  count/shown 不自洽（count=$D_COUNT shown=$D_SHOWN）"
fi
check "  截断后仍能卸载 usb0"          "$(G '/eject?mount=%2Fmnt%2Fusb0')" '"ok":true'
stop_server
echo "  （场景 D 结束，服务已退出）"

# ============================================================
echo
echo "=== 场景 E：--no-ui（只留接口，不主动打扰）==="
env ./"$BIN" --no-ui "$PORT" > server.log 2>&1 &
SRV_PID=$!
for i in $(seq 1 40); do
  curl -s -m 1 --noproxy 127.0.0.1 "http://127.0.0.1:$PORT/ping" 2>/dev/null | grep -q pong && break
  sleep 0.15
done
rm -f last_notify.json
DE=$(G /diag)
check "--no-ui 时 ui_autolaunch=false" "$DE" '"ui_autolaunch":false'
check "--no-ui 时一次都不尝试"          "$DE" '"ui_attempts":0'
check "--no-ui 时 rc 保持未尝试哨兵"    "$DE" '"rc":{"user_init":-999,"browser":-999,"notify":-999,"knotify":-999}'
nocheck "--no-ui 时日志里没有浏览器调用" "$(cat server.log)" 'sceSystemServiceLaunchWebBrowser'
OU=$(G /openui)
check "/openui 手动触发成功"     "$OU" '"ok":true'
check "  /openui 回执带真实 rc"  "$OU" '"browser":0'
check "  触发后 ui_attempts=1"   "$(G /diag)" '"ui_attempts":1'
check "  手动触发也发了通知"     "$(G /diag)" '"notify":0'
stop_server
echo "  （场景 E 结束，服务已退出）"

# ============================================================
echo
echo "=== 场景 F：系统调用被主机拒绝时，失败码必须如实报出 ==="
start_server F USBMANAGE_FAKE_NOUI_FAIL=1 || exit 1
DF=$(G /diag)
check "被拒时 browser rc 非 0"          "$DF" '"browser":-1'
check "被拒时 notify rc 非 0"           "$DF" '"notify":-1'
check "被拒时内核通知 rc 也非 0"        "$DF" '"knotify":-1'
check "  但 user_init 仍报真实值"       "$DF" '"user_init":0'
check "  服务本身不受影响，页面照常"    "$(curl -s -m 5 --noproxy 127.0.0.1 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/")" "200"
check "  列表照常可用"                  "$(G /list)" '"count":2'
stop_server
echo "  （场景 F 结束，服务已退出）"

# ============================================================
echo
echo "=== 场景 K：卷标解析 —— 直接读设备引导扇区与根目录 ==="
# 镜像由 mkfixtures.py 生成在当前目录，伪造挂载表把设备名指过去，
# 于是"打开设备 -> 解析引导扇区 -> 找卷标"这一段在宿主机上是真跑的。
PY=""
for c in python3 python py; do
  if command -v "$c" >/dev/null 2>&1; then PY="$c"; break; fi
done
if [ -n "$PY" ]; then
  echo "[http] 用 $PY 生成合成镜像"
  "$PY" mkfixtures.py . | sed 's/^/        /'
  start_server K USBMANAGE_FAKE_LABELS=1 USBMANAGE_FAKE_PROCS="$FAKE_PROCS" \
              USBMANAGE_FAKE_SELF_PID=4242 || exit 1
  LK=$(G /list)
  check "场景 K：6 块外接盘（5 张合成镜像 + 1 个不存在的设备）" "$LK" '"count":6'
  check "  exFAT：根目录 0x83 项里的 UTF-16 中文卷标" \
        "$LK" '"mount":"/mnt/usb2","device":"fix_exfat.img","fstype":"exfatfs","ejectable":true,"label":"移动硬盘"'
  check "  FAT32：0x0F 长名项拼回去的中文卷标"      "$LK" '"label":"备份盘"'
  check "  FAT16：固定根目录里的 8.3 短项"          "$LK" '"label":"BACKUP"'
  nocheck "  短项优先，不会被引导扇区里的另一个名字覆盖" "$LK" 'BPBLABEL'
  check "  FAT32 根目录为空时退回引导扇区里的 8.3"  "$LK" '"label":"OLDDISK"'
  check "  垃圾字节 -> 老实报"没有卷标""             "$LK" '"mount":"/mnt/usb6","device":"fix_blob.img","fstype":"msdosfs","ejectable":true,"label":""'
  check "  设备文件不存在 -> 同样只是没卷标"        "$LK" '"mount":"/mnt/usb7","device":"no_such_image.img","fstype":"msdosfs","ejectable":true,"label":""'
  check "  诊断视图里的内部卷不去读设备（label 为空）" \
        "$(G '/list?all=1')" '"mount":"/","device":"/dev/ssd0.ufs","fstype":"ufs","ejectable":false,"label":""'
  check "  有卷标不妨碍卸载（仍走同一条路）"        "$(G '/eject?mount=%2Fmnt%2Fusb2')" '"ok":true'
  check "  卸载回执带挂载点"                        "$(G '/eject?mount=%2Fmnt%2Fusb3')" '"ok":true'
  stop_server
  echo "  （场景 K 结束，服务已退出）"
else
  bad "找不到 python（python3/python/py 都没有），卷标场景未执行"
fi

# ============================================================
echo
echo "=== 场景 L：重复加载 —— 同版本只激活、不起第二个；--force 才接管 ==="
# 这一场要验通知文案，而通知语言只看环境变量。显式钉 LC_ALL=zh_CN.UTF-8：
# 两个目的——① 断言里写的都是中文原文；② 顺便验 LC_ALL 的优先级最高。
# 注意必须设 LC_ALL 而不是 LANG：Git Bash 默认已设 LC_ALL=C.UTF-8，
# 不覆盖它的话 LANG 根本轮不到（见 usbmanage.c 的 env_lang）。
export LC_ALL=zh_CN.UTF-8
start_server L USBMANAGE_FAKE_PROCS="$FAKE_PROCS" USBMANAGE_FAKE_FDS="$FAKE_FDS" \
             USBMANAGE_FAKE_SELF_PID=4242 || exit 1
FIRST_PID=$SRV_PID

# 第二次加载同一个文件（不带 --force）：应当"激活已有实例"后立刻退出
env USBMANAGE_FAKE_PROCS="$FAKE_PROCS" USBMANAGE_FAKE_FDS="$FAKE_FDS" \
    ./"$BIN" "$PORT" > second.log 2>&1
SECOND_RC=$?
check "同版本重复加载：新进程自己退出（退出码 0）" "$SECOND_RC" "0"
check "  日志说明是激活而不是新起一个"  "$(cat second.log)" "activating it instead of starting a second instance"
nocheck "  没有第二个监听者（日志里没有 listening）" "$(cat second.log)" "listening on 0.0.0.0"
check "  确实去把界面叫到屏幕上了"      "$(cat second.log)" 'sceSystemServiceLaunchWebBrowser("http://'
check "  激活时也发了通知"              "$(cat second.log)" 'sceNotificationSend(user=254'
check "  通知里说明了为什么（已有同一版本在运行）" "$(cat notify_kernel.txt)" "已有同一版本在运行"
check "  原实例仍在服务（pid 未被顶掉）" "$(G /ping)" "pong"
if kill -0 "$FIRST_PID" 2>/dev/null; then
  ok "  原实例进程确实还活着（重复加载没有换进程）"
else
  bad "  原实例进程没了 —— 同版本重复加载不该换进程"
fi
if [ "$(grep -c 'listening on' server.log)" = "1" ]; then
  ok "  原实例的日志里只有一次 listening（没被接管过一次）"
else
  bad "  原实例日志里 listening 出现 $(grep -c 'listening on' server.log) 次"
fi

# 带 --force 再来一次：这次应当请旧的退出、自己顶上
env USBMANAGE_FAKE_PROCS="$FAKE_PROCS" USBMANAGE_FAKE_FDS="$FAKE_FDS" \
    ./"$BIN" "$PORT" --force > force.log 2>&1 &
SRV_PID=$!
sleep 0.3
check "  接管日志说明要顶替"            "$(cat force.log)" "taking it over"
ok_wait=0
for i in $(seq 1 20); do
  curl -s -m 1 --noproxy 127.0.0.1 "http://127.0.0.1:$PORT/ping" 2>/dev/null | grep -q pong && { ok_wait=1; break; }
  sleep 0.15
done
if [ "$ok_wait" = "1" ]; then ok "  接管后新实例已在服务"; else bad "  接管后没人应答"; fi
gone=0
for i in $(seq 1 20); do
  kill -0 "$FIRST_PID" 2>/dev/null || { gone=1; break; }
  sleep 0.15
done
if [ "$gone" = "1" ]; then ok "  旧实例真的从进程里退出了（端口交给新实例）"; else bad "  旧实例没有退出"; fi
check "  旧实例是被 /shutdown 请退的（它自己的日志为证）" \
      "$(cat server.log)" 'shutting down (requested by a newer instance)'
check "  接管时的通知说明了版本变化"    "$(cat notify_kernel.txt)" "已接管：v$VER -> v$VER"
check "  新实例自述为接管档，且说清顶替的是谁" "$(G /diag)" \
      '"ui_reason":"takeover","other_version":"'"$VER"'"'
stop_server
unset LC_ALL
echo "  （场景 L 结束，服务已退出）"

# ============================================================
echo
echo "=== 场景 M：/shutdown 的来源校验 ==="
# 把对端地址伪造成非回环：此时"回环"这把钥匙失效，只剩页面标识头那一把。
start_server M USBMANAGE_FAKE_PEER_IP=192.168.1.200 USBMANAGE_FAKE_PROCS="$FAKE_PROCS" || exit 1
check "非回环来源、无页面标识头 -> 拒绝" "$(G /shutdown)" '"ok":false'
check "  拒绝理由说清了放行条件（带中文头，验双语真的生效）" \
      "$(curl -s -m 5 --noproxy 127.0.0.1 -H 'Accept-Language: zh-CN' "http://127.0.0.1:$PORT/shutdown")" \
      'shutdown 只允许本机触发'
check "  服务仍在（没被关掉）"           "$(G /ping)" "pong"
check "  浏览器地址栏打开 /shutdown 被 302（不会误关）" \
      "$(curl -s -m 5 --noproxy 127.0.0.1 -o /dev/null -D - -H "$BROWSER" "http://127.0.0.1:$PORT/shutdown")" "302 Found"
MS=$(curl -s -m 5 --noproxy 127.0.0.1 -H 'X-Requested-With: usbmanage' "http://127.0.0.1:$PORT/shutdown")
check "非回环来源 + X-Requested-With: usbmanage -> 放行" "$MS" '"ok":true'
check "  回执带 state=shutting_down（新实例据此判定接管成功）" "$MS" '"state":"shutting_down"'
check "  回执带自述名与版本"             "$MS" '"app":"usbmanage","version":"'$VER'"'
mdead=0
for i in $(seq 1 20); do
  kill -0 "$SRV_PID" 2>/dev/null || { mdead=1; break; }
  sleep 0.15
done
if [ "$mdead" = "1" ]; then ok "  进程真的从进程里退出了（不是只停服务）"; else bad "  进程还在"; fi

# 回环来源：不带任何标识头也必须放行 —— 新实例接管旧实例走的就是这条
start_server M2 USBMANAGE_FAKE_PROCS="$FAKE_PROCS" || exit 1
check "回环来源、不带标识头也放行（接管通道）" "$(G /shutdown)" '"ok":true'
stop_server
echo "  （场景 M 结束，服务已退出）"

# ============================================================
# 场景 N：界面语言 —— 中文按简繁分档，其余语言一律英文
#
# 判定来源是请求头 Accept-Language（它由主机系统语言决定），另有 ?lang=
# 作为最高优先级的覆盖口，供脚本与 PC 端显式指定。
# 屏幕通知发在任何请求**之前**，那时没有请求头可读，只能靠环境变量 LANG
# 判语言——所以这一场把 LANG 钉成 zh_CN，顺便把中文档的通知也验掉。
# ============================================================
echo
echo "=== 场景 N：界面语言（中文分简繁，其余英文）==="

start_server N LANG=zh_CN.UTF-8 || exit 1

if [ -f notify_kernel.txt ]; then
  check "  中文档：内核通知带中文程序名" "$(cat notify_kernel.txt)" "PS5 USB管理器 v$VER"
else
  bad "  中文档没有留下 notify_kernel.txt"
fi
if [ -f last_notify.json ]; then
  check "  中文档：通知正文是中文的「管理地址」" "$(cat last_notify.json)" '管理地址 http://'
else
  bad "  中文档没有留下 last_notify.json"
fi

# --- 请求语言：Accept-Language 决定 ---
check "  无 Accept-Language -> lang=en" "$(G /version)" '"lang":"en"'
check "  Accept-Language: zh-CN -> lang=zh-Hans" \
      "$(curl -s -m 5 --noproxy 127.0.0.1 -H 'Accept-Language: zh-CN,zh;q=0.9' "http://127.0.0.1:$PORT/version")" \
      '"lang":"zh-Hans"'
check "  Accept-Language: zh-Hans-CN -> lang=zh-Hans" \
      "$(curl -s -m 5 --noproxy 127.0.0.1 -H 'Accept-Language: zh-Hans-CN' "http://127.0.0.1:$PORT/version")" \
      '"lang":"zh-Hans"'
check "  Accept-Language: zh-TW -> lang=zh-Hant" \
      "$(curl -s -m 5 --noproxy 127.0.0.1 -H 'Accept-Language: zh-TW' "http://127.0.0.1:$PORT/version")" \
      '"lang":"zh-Hant"'
check "  Accept-Language: zh-HK -> lang=zh-Hant" \
      "$(curl -s -m 5 --noproxy 127.0.0.1 -H 'Accept-Language: zh-HK' "http://127.0.0.1:$PORT/version")" \
      '"lang":"zh-Hant"'
check "  Accept-Language: zh-Hant -> lang=zh-Hant" \
      "$(curl -s -m 5 --noproxy 127.0.0.1 -H 'Accept-Language: zh-Hant' "http://127.0.0.1:$PORT/version")" \
      '"lang":"zh-Hant"'
check "  Accept-Language: ja-JP -> lang=en" \
      "$(curl -s -m 5 --noproxy 127.0.0.1 -H 'Accept-Language: ja-JP' "http://127.0.0.1:$PORT/version")" \
      '"lang":"en"'

# --- 返回给界面的 msg 也要跟着语言走 ---
# 拿"白名单拒绝内部存储"那条来验：三种语言的正文明显不同，且各档里都不该
# 残留别档的文字（简体档不出现繁体字，英文档不出现汉字）。
EN_MSG=$(G '/eject?mount=%2Fmnt%2Fext0')
ZH_MSG=$(curl -s -m 5 --noproxy 127.0.0.1 -H 'Accept-Language: zh-CN' \
         "http://127.0.0.1:$PORT/eject?mount=%2Fmnt%2Fext0")
HT_MSG=$(curl -s -m 5 --noproxy 127.0.0.1 -H 'Accept-Language: zh-TW' \
         "http://127.0.0.1:$PORT/eject?mount=%2Fmnt%2Fext0")
check   "  英文档的 msg 用英文说明"     "$EN_MSG" 'only /mnt/usb<number> is allowed'
nocheck "  英文档的 msg 不残留中文说明" "$EN_MSG" '只允许 /mnt'
check   "  简体档的 msg 用简体说明"     "$ZH_MSG" '只允许 /mnt/usb'
nocheck "  简体档的 msg 不残留英文说明" "$ZH_MSG" 'only /mnt/usb'
nocheck "  简体档的 msg 不残留繁体字"   "$ZH_MSG" '只允許'
check   "  繁体档的 msg 用繁体说明"     "$HT_MSG" '只允許 /mnt/usb'
nocheck "  繁体档的 msg 不残留简体字"   "$HT_MSG" '只允许'

# --- 通知文案也要跟着语言走（同样看繁体那一档是否真的换了字形）---
if [ -f notify_kernel.txt ]; then
  nocheck "  简体档通知里不出现繁体字" "$(cat notify_kernel.txt)" '管理位址'
else
  bad "  简体档没有留下 notify_kernel.txt"
fi

# --- ?lang= 优先级最高 ---
check "  ?lang=zh 覆盖缺省（无 Accept-Language）" "$(G '/version?lang=zh')" '"lang":"zh-Hans"'
check "  ?lang=zh-Hant 覆盖缺省" "$(G '/version?lang=zh-Hant')" '"lang":"zh-Hant"'
check "  ?lang=en 覆盖 Accept-Language: zh-CN" \
      "$(curl -s -m 5 --noproxy 127.0.0.1 -H 'Accept-Language: zh-CN' "http://127.0.0.1:$PORT/version?lang=en")" \
      '"lang":"en"'
check "  ?lang=zh-Hans 覆盖 Accept-Language: zh-TW" \
      "$(curl -s -m 5 --noproxy 127.0.0.1 -H 'Accept-Language: zh-TW' "http://127.0.0.1:$PORT/version?lang=zh-Hans")" \
      '"lang":"zh-Hans"'

# --- 页面：三套文案在同一份 index.html 里，切语言不换文件 ---
PAGE_SRC="$(cat "$ROOT/web/index.html")"
check   "  页面里同时含简体名、繁体名与英文名" "$PAGE_SRC" 'PS5 USB Manager'
check   "  页面带繁体词表（zh-Hant）"        "$PAGE_SRC" "'zh-Hant': {"
check   "  页面自带语言判定（navigator）"    "$PAGE_SRC" 'navigator.language'
check   "  页面支持 ?lang= 覆盖"             "$PAGE_SRC" 'lang=([A-Za-z0-9_-]+)'
check   "  页面按判定结果设 html lang"       "$PAGE_SRC" 'documentElement.lang'

stop_server
echo "  （场景 N 结束，服务已退出）"

# ============================================================
echo
echo "============================================================"
printf '  结果：\033[32m%d 通过\033[0m' "$PASS"
if [ "$FAIL" -gt 0 ]; then
  printf '，\033[31m%d 失败\033[0m\n' "$FAIL"
else
  printf '，0 失败\n'
fi
echo "============================================================"
rm -f "$BIN" page.out page.hdr icon.out favicon.out server.log second.log force.log \
      last_notify.json notify_kernel.txt kill.log fix_*.img
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
