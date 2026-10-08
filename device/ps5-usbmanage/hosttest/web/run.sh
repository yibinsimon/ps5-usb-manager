#!/usr/bin/env bash
#
# 页面逻辑离机测试。不需要 PS5、不需要浏览器、不需要联网。
#
#   bash hosttest/web/run.sh
#
# 它把 web/index.html 里的 <script> 抽出来，在一个极简 DOM 上执行，
# 断言"卸载失败后卡片上真的出现了「解除占用并卸载」按钮"。
# 这是 curl 级 HTTP 测试覆盖不到的一层——「按钮消失」这类 bug 就出在这里。
#
# NODE 环境变量可覆盖 node 路径。
set -u
cd "$(dirname "$0")"
NODE_BIN="${NODE:-node}"

if ! command -v "$NODE_BIN" >/dev/null 2>&1; then
    echo "找不到 node（可用 NODE=/path/to/node 指定）" >&2
    exit 2
fi

echo "== 页面逻辑测试 (web/index.html) =="
# 版本号从 usbmanage.c 的 #define VERSION 现读，不写死在测试里：
# 写死的后果是源码改了版本、测试还断言旧值却照样全绿。
VER=$(sed -n 's/^#[[:space:]]*define[[:space:]]\{1,\}VERSION[[:space:]]\{1,\}"\([^"]*\)".*$/\1/p' ../../usbmanage.c | head -1)
if [ -z "$VER" ]; then
    echo "FATAL: 无法从 usbmanage.c 读出 VERSION" >&2
    exit 2
fi
echo "   （源码 VERSION = $VER）"
"$NODE_BIN" page_test.js ../../web/index.html "$VER"
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "== 页面逻辑测试通过 =="
else
    echo "== 页面逻辑测试失败 (rc=$rc) ==" >&2
fi
exit "$rc"
