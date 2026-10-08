#!/usr/bin/env bash
# ============================================================
#  hosttest/run.sh —— usbmanage 纯逻辑回归测试（宿主机执行）
#
#  做什么：
#    1) 用 awk 按函数名从 usbmanage.c 原文抽出与平台无关的纯函数，
#       生成 pure.inc（保证测的就是即将编进 elf 的那份代码）；
#    2) 用宿主机 clang 编译 test_host.c 并运行断言。
#
#  为什么需要它：
#    挂载点白名单、路径穿越、URL 解码、卷标解析、界面语言判定这些逻辑
#    与 PS5 无关，完全可以在宿主机上跑穷举断言；一旦判错，在真机上暴露
#    的代价很高（白名单放宽会去卸主机内部存储，语言判错会让用户看到
#    另一种文字）。所以这些都在这里拦住。
#
#  用法：bash hosttest/run.sh
#  退出码：0 = 全通过，1 = 有断言失败或编译失败
# ============================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/../usbmanage.c"
cd "$HERE"

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

# ---- 1. 按函数名抽取纯函数（不碰函数体，避免误删代码）----
# 卷标与语言那两段另有一套规则：类型与常量用显式标记夹出来，保证
# "测试里用的类型"与"实现里用的类型"是同一份，不会各自漂移。
echo "[hosttest] 从 usbmanage.c 抽取纯函数 -> pure.inc"
awk '
  /^static (int|void|size_t) (is_digit|is_hex|hex_val|url_decode|path_allowed|json_escape|room|path_under|holder_killable|extract_mount|rd16|rd32|fsboot_parse|utf16le_to_utf8|label_from_83|exfat_label_in_dir|fat_label_in_dir|starts_with_ci|lang_of|accept_language_lang|env_lang)\(/ { p = 1 }
  p { print }
  p && /^}/ { p = 0 }

  /^\/\* >>> fs-label-types >>> \*\// { s = 1 }
  s { print }
  s && /^\/\* <<< fs-label-types <<< \*\// { s = 0 }

  /^\/\* >>> lang-types >>> \*\// { q = 1 }
  q { print }
  q && /^\/\* <<< lang-types <<< \*\// { q = 0 }
' "$SRC" > pure.inc

for f in is_digit is_hex hex_val url_decode path_allowed json_escape room path_under holder_killable extract_mount \
         rd16 rd32 fsboot_parse utf16le_to_utf8 label_from_83 exfat_label_in_dir fat_label_in_dir \
         starts_with_ci lang_of accept_language_lang env_lang; do
  if ! grep -q " $f(" pure.inc; then
    echo "[error] 抽取失败：缺少 $f()。usbmanage.c 的函数签名可能改过，请同步更新本脚本。" >&2
    exit 1
  fi
done
for mark in 'struct fsboot {' 'enum { LANG_ZH_HANS'; do
  if ! grep -qF "$mark" pure.inc; then
    echo "[error] 抽取失败：缺少『$mark』。请检查 usbmanage.c 里的类型标记。" >&2
    exit 1
  fi
done
echo "[hosttest] 已抽取 $(grep -c '^static ' pure.inc) 个函数 + 卷标/语言类型"

# ---- 2. 编译并运行 ----
echo "[hosttest] 编译 test_host.c"
"$CC" -O1 -Wall test_host.c -o test_host.exe

echo "[hosttest] 运行断言"
set +e
./test_host.exe
RC=$?
set -e

# clean
rm -f test_host.exe pure.inc
exit "$RC"
