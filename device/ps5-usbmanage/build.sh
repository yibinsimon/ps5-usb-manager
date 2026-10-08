#!/usr/bin/env bash
# ============================================================
#  usbmanage.elf —— PS5 payload 编译脚本
#  适用：Git Bash / MSYS2（Windows）、Linux、macOS
#
#  依赖：
#    1) ps5-payload-sdk（含 target/ 与 win/）
#    2) 宿主机 clang + ld.lld。Windows 下用 llvm-mingw 即可，
#       无需 WSL、无需 Docker。
#
#  环境变量（不设则用下面的默认值）：
#    PS5_PAYLOAD_SDK   指向 ps5-payload-sdk 根目录
#    PS5_CLANG         指向 clang 可执行文件
#
#  用法：  bash build.sh
#  产物：  usbmanage.elf
#
# ------------------------------------------------------------
#  本脚本的参数顺序、库、可见性开关，逐项对齐 SDK 官方的
#  bin/prospero-clang，只做了一处 Windows 适配：
#    官方 bin/prospero-clang 在 clang 主版本 >= 20 时会清空
#    LIBS_CRT（clang 20 起驱动会自动追加 crt 目标文件），
#    而 win/prospero-clang.cmd 漏了这个判断，直接传绝对路径
#    crt1.o，导致 "duplicate symbol: payload_exit" 链接失败。
#    本脚本因此不显式传 crt1.o。
# ------------------------------------------------------------
#  Windows 上的两个坑（已在此处理）：
#   1) PATH 条目必须是 POSIX 形态（/c/...）。放 "C:/..." 会让
#      MSYS 的 PATH 转换出错，clang 报
#      "unable to execute command: program not executable"。
#   2) PATH 必须同时包含 clang 目录（供 ld.lld）与 SDK/win
#      （供 prospero-lld.exe）——SDK 的 prospero-lld.exe 只是
#      壳，内部再调 ld.lld。
#   另外设置 SCE_PROSPERO_SDK_DIR，让驱动把 --sysroot 指向
#   SDK 自身（与官方 wrapper 行为一致）。
# ============================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

# ---- 1. 定位 SDK ----
# 环境变量优先；没设就在几个常见位置找一找。找不到就明确报错，
# 绝不回退到某个写死的绝对路径——那样只会给出误导性的报错。
SDK="${PS5_PAYLOAD_SDK:-}"
if [ -z "$SDK" ]; then
  for cand in "$HOME/ps5-payload-sdk" "/opt/ps5-payload-sdk" \
              "$HERE/ps5-payload-sdk" "$HERE/../ps5-payload-sdk" \
              "$HERE/../../ps5-payload-sdk"; do
    if [ -d "$cand/target/include" ]; then SDK="$cand"; break; fi
  done
fi
if [ -z "$SDK" ] || [ ! -d "$SDK/target/include" ]; then
  echo "[error] 找不到 ps5-payload-sdk${SDK:+（已试 $SDK）}" >&2
  echo "        请下载并解压 SDK，然后设置 PS5_PAYLOAD_SDK 指向它的根目录：" >&2
  echo "          export PS5_PAYLOAD_SDK=\"\$HOME/ps5-payload-sdk\"   # Git Bash / Linux / macOS" >&2
  echo "        获取：https://github.com/ps5-payload-dev/sdk/releases/latest" >&2
  exit 1
fi

# ---- 2. 定位 clang ----
# PS5_CLANG 优先；否则从 PATH 找；再找不到就报错，不猜路径。
if [ -n "${PS5_CLANG:-}" ]; then
  CLANG="$PS5_CLANG"
else
  CLANG="$(command -v clang.exe || command -v clang || true)"
fi
if [ -z "$CLANG" ] || [ ! -x "$CLANG" ]; then
  echo "[error] 找不到 clang。请安装 LLVM / llvm-mingw，或用 PS5_CLANG 指定：" >&2
  echo "          export PS5_CLANG=\"/c/llvm-mingw/bin/clang.exe\"" >&2
  exit 1
fi
CLANG_DIR="$(cd "$(dirname "$CLANG")" && pwd)"

# ---- 3. 组装 PATH（见头部「坑 1 / 坑 2」）----
to_posix() {
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -u "$1"
  else
    case "$1" in
      [A-Za-z]:[\\/]*) printf '/%s%s\n' "$(echo "${1:0:1}" | tr 'A-Z' 'a-z')" "${1:2}" ;;
      *) printf '%s\n' "$1" ;;
    esac
  fi
}

NEWPATH="$(to_posix "$CLANG_DIR")"
[ -d "$SDK/win" ] && NEWPATH="$NEWPATH:$(to_posix "$SDK/win")"
export PATH="$NEWPATH:$PATH"

if ! command -v ld.lld >/dev/null 2>&1; then
  echo "[warn] PATH 中找不到 ld.lld —— SDK 的 prospero-lld 需要它，链接可能失败" >&2
fi

# 让驱动把 --sysroot 指向 SDK 自身（对齐官方 wrapper）
export SCE_PROSPERO_SDK_DIR="$SDK"

# ---- 4. 生成内置页面数组 ----
# web/index.html 是页面源码（可以直接用浏览器打开调试），
# 这里用 xxd 转成 C 字节数组，编译进 elf。不用手工转义，页面改了即刻生效。
if [ ! -f web/index.html ]; then
  echo "[error] 缺少 web/index.html" >&2
  exit 1
fi
if ! command -v xxd >/dev/null 2>&1; then
  echo "[error] 找不到 xxd（vim-common 自带）。可用 -p 版本替代，或安装 vim。" >&2
  exit 1
fi
xxd -i -n usbmanage_index_html web/index.html > web/index_html.h
echo "[build] web/index.html -> web/index_html.h ($(wc -c < web/index.html) bytes)"

# 应用图标同理：web/icon.png 是 assets/icon/ 里的 96x96 定稿，
# 编进 elf 后由 GET /icon.png 提供（/favicon.ico 返回同一份）。
# 换图标请改 assets/icon/ 下对应尺寸并重新拷贝到 web/icon.png，然后重跑本脚本。
if [ ! -f web/icon.png ]; then
  echo "[error] 缺少 web/icon.png（应从 assets/icon/ 拷贝，见 assets/icon/README.md）" >&2
  exit 1
fi
xxd -i -n usbmanage_icon_png web/icon.png > web/icon_png.h
echo "[build] web/icon.png   -> web/icon_png.h   ($(wc -c < web/icon.png) bytes)"

# ---- 5. 编译 ----
echo "[build] clang : $CLANG"
echo "[build] sdk   : $SDK"

"$CLANG" \
  --start-no-unused-arguments \
  -target x86_64-sie-ps5 \
  -fvisibility-nodllstorageclass=default \
  -isysroot "$SDK" \
  -isystem "$SDK/target/include" \
  -L "$SDK/target/lib" \
  -L "$SDK/target/user/homebrew/lib" \
  -fno-stack-protector -fno-plt -femulated-tls \
  -Os -Wall -Wextra \
  -lc -lkernel_sys \
  --end-no-unused-arguments \
  usbmanage.c -o usbmanage.elf \
  --start-no-unused-arguments \
  --sysroot "$SDK" \
  -lSceLibcInternal -lSceNet \
  -lSceSystemService -lSceUserService -lSceNotification \
  --end-no-unused-arguments
# 上面三个库对应"自动弹界面"：
#   -lSceSystemService -lSceUserService -> sceSystemServiceLaunchWebBrowser
#                                          （官方 samples/browser 就是这么链的）
#   -lSceNotification                   -> sceNotificationSend
#                                          （官方 samples/notify）

# ---- 6. 自校验 ----
if command -v llvm-readelf >/dev/null 2>&1; then
  RE=llvm-readelf
elif [ -x "$CLANG_DIR/llvm-readelf.exe" ]; then
  RE="$CLANG_DIR/llvm-readelf.exe"
else
  RE=""
fi

if [ -n "$RE" ]; then
  echo "[check] ELF header:"
  "$RE" -h usbmanage.elf | grep -E "Class|Type|Machine|OS/ABI" | sed 's/^/        /'
  echo "[check] NEEDED:"
  "$RE" -d usbmanage.elf | grep NEEDED | sed 's/^/        /'
  # "自动弹界面"依赖三个系统库与三个符号。只要有一处没链上，
  # 主机上就会静默不弹——这种失败在真机上极难定位，故在此直接判死。
  MISSING=""
  for lib in libSceSystemService libSceUserService libSceNotification; do
    "$RE" -d usbmanage.elf | grep -q "$lib" || MISSING="$MISSING $lib"
  done
  if [ -n "$MISSING" ]; then
    echo "[error] NEEDED 缺少库:$MISSING" >&2
    exit 1
  fi
  echo "[check] UI 系统库齐备: SceSystemService / SceUserService / SceNotification"

  MISSING=""
  for sym in sceSystemServiceLaunchWebBrowser sceUserServiceInitialize \
             sceNotificationSend sceKernelSendNotificationRequest; do
    "$RE" --dyn-syms usbmanage.elf | grep -q "$sym" || MISSING="$MISSING $sym"
  done
  if [ -n "$MISSING" ]; then
    echo "[error] 动态符号表里缺少:$MISSING" >&2
    exit 1
  fi
  echo "[check] UI 系统调用符号齐备"
  # 第二个通知通道 sceKernelSendNotificationRequest 由 libkernel_sys
  # 提供，而 libkernel_sys 本来就在 NEEDED 里 —— 少链一个符号不会多出一条
  # NEEDED，NEEDED 检查发现不了，所以必须逐个符号断言（上面那轮已含）。

  # 链接器会写入 16 字节随机 build-id，因此同样的源码两次编译 sha256 必然不同。
  # 实测：剥掉 .note.gnu.build-id 后，两次构建的其余字节完全一致
  # （语义上可复现）。所以 sha256 只当"这一份文件的传输完整性指纹"用，
  # 不要当成"源码 -> 字节"的复现凭据。
  echo "[check] build-id (每次链接都变，不影响功能):"
  "$RE" -n usbmanage.elf | grep -i "Build ID" | sed 's/^/        /'
fi

# 页面是否真的进了二进制：拿页面里的独特标记去 elf 里找。
HTML_LEN=$(wc -c < web/index.html)
HDR_LEN=$(sed -n 's/^unsigned int usbmanage_index_html_len = \([0-9]*\);$/\1/p' web/index_html.h)
echo "[check] 内置页面: web/index.html=$HTML_LEN bytes, header=$HDR_LEN bytes"
if [ "$HTML_LEN" != "$HDR_LEN" ]; then
  echo "[error] 页面长度与生成数组不一致" >&2
  exit 1
fi
if grep -aq "data-eject" usbmanage.elf; then
  echo "[check] 页面已编入 elf: OK (found marker 'data-eject')"
else
  echo "[error] elf 里找不到页面标记，说明页面没被编进去" >&2
  exit 1
fi

# 图标同样要真的进二进制。这里用两个独立的判据：
#   1) 生成数组的长度与 web/icon.png 一致 —— 防"改了图没重新生成/拷贝"；
#   2) elf 里能找到 PNG 的 IHDR 标记 —— 防"数组因为没人引用被链接器丢掉"
#      （未引用的静态数组确实可能被优化掉，只查长度发现不了）。
ICON_LEN=$(wc -c < web/icon.png)
ICON_HDR=$(sed -n 's/^unsigned int usbmanage_icon_png_len = \([0-9]*\);$/\1/p' web/icon_png.h)
echo "[check] 内置图标: web/icon.png=$ICON_LEN bytes, header=$ICON_HDR bytes"
if [ "$ICON_LEN" != "$ICON_HDR" ]; then
  echo "[error] 图标长度与生成数组不一致" >&2
  exit 1
fi
if grep -aq "IHDR" usbmanage.elf; then
  echo "[check] 图标已编入 elf: OK (found PNG marker 'IHDR')"
else
  echo "[error] elf 里找不到 PNG 标记，说明图标没被编进去（或数组被优化掉）" >&2
  exit 1
fi

# 名称约定：显示名 中文「PS5 USB管理器」/ 英文「PS5 USB Manager」；
# 机器标识固定 `usbmanage`（payload 文件名、JSON 的 app 字段）。
# 中英文显示名都要真的进二进制（/version、屏幕通知、界面标题、页脚都要用），
# 这里逐条做兜底断言——少一个都会让另一种语言的界面露出另一种语言的字。
for pair in "PS5 USB管理器:中文名" "PS5 USB Manager:英文名" "usbmanage:机器标识"; do
  NAME="${pair%%:*}"; WHAT="${pair##*:}"
  if grep -aq "$NAME" usbmanage.elf; then
    echo "[check] $WHAT 已编入 elf: OK ($NAME)"
  else
    echo "[error] elf 里没有 $WHAT『$NAME』" >&2
    exit 1
  fi
done

# 三档语言都要真的进二进制：/version 自述用的标记，加上只在繁体档里出现的
# 词（卸載／儲存）。少一档就是"用户切到那种语言却看到别种文字"，
# 而这种缺失在真机上只会表现成"界面语言不对"，极难定位。
for pair in "zh-Hans:简体语言标记" "zh-Hant:繁体语言标记" "卸載:繁体正文" "儲存:繁体正文"; do
  NAME="${pair%%:*}"; WHAT="${pair##*:}"
  if grep -aq "$NAME" usbmanage.elf; then
    echo "[check] $WHAT 已编入 elf: OK ($NAME)"
  else
    echo "[error] elf 里没有 $WHAT『$NAME』——三种语言没编全" >&2
    exit 1
  fi
done

echo "[ok] usbmanage.elf  ($(wc -c < usbmanage.elf) bytes)"
