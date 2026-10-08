#!/bin/sh
# safeeject.sh - USB 存储安全卸载脚本（PS5 / Orbis）
#
# 用途：在没有图形化"弹出"入口的主机上，手动完成"落盘 + 卸载 + 回读校验"。
#
# 定位：这是**不依赖 payload 的手动备用手段**。PS5 上的常规路径是用 usbmanage
#       payload（界面 + 一键卸载，见 device/ps5-usbmanage/）。
#       本脚本**未经实机验证**，仅在需要手工排查时使用。
#
# 用法：
#   sh safeeject.sh            # 自动探测并卸载所有可移动卷
#   sh safeeject.sh /mnt/usb0  # 卸载指定挂载点
#
# 退出码：0 全部成功；1 部分或全部失败。

set -u

TARGET="${1:-}"

say() { printf '%s\n' "$*"; }

# --- 1. 落盘：把页缓存刷到介质 ---
say "[1/3] sync 落盘 ..."
sync || { say "  sync 失败"; }

# --- 2. 探测可卸载卷 ---
# 只认 /mnt/usb<数字>：这是 Orbis 上唯一可卸载的外接存储挂载点。
# 内部存储与 /mnt/sandbox/** 沙箱视图都不在此列。
if [ -n "$TARGET" ]; then
    CANDS="$TARGET"
else
    CANDS=""
    for m in /mnt/usb0 /mnt/usb1 /mnt/usb2 /mnt/usb3; do
        if mount 2>/dev/null | grep -q " $m "; then CANDS="$CANDS $m"; fi
    done
fi

if [ -z "${CANDS:-}" ]; then
    say "[2/3] 未发现已挂载的可移动卷，无需卸载。"
    exit 0
fi

say "[2/3] 待卸载：$CANDS"

# --- 3. 逐个卸载 ---
RC=0
for V in $CANDS; do
    if umount "$V" 2>/dev/null; then
        say "  [OK] umount $V"
    elif umount -f "$V" 2>/dev/null; then
        say "  [OK] umount -f $V（强制）"
    else
        say "  [FAIL] 无法卸载 $V（可能被应用占用；请先退出媒体应用/关闭播放）"
        RC=1
    fi
done

# --- 回读校验 ---
say "[3/3] 校验 ..."
for V in $CANDS; do
    if mount 2>/dev/null | grep -q " $V "; then
        say "  仍处于挂载：$V"
        RC=1
    fi
done

if [ "$RC" -eq 0 ]; then
    say "完成：可安全拔出。"
else
    say "存在未成功项，请勿拔出。"
fi
exit "$RC"
