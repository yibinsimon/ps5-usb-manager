#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mkfixtures.py —— 给"卷标解析"造几张合成镜像（只在宿主机测试里用）。

为什么要在宿主机上造镜像：卷标解析是纯字节活（引导扇区 + 根目录项），
与 PS5 一点关系都没有。用合成镜像就能把 exFAT 长名、FAT32 的 LFN、
FAT16 的固定根目录、以及"根本不是文件系统"这几种情况全部穷举，
不必等到真机上插四块盘。生成的文件只在 hosttest/http 的目录里存在，
用完即删，不进 elf、不上真机。

生成的镜像（都是"能过引导扇区两道校验、结构自洽"的最小体量）：

    fix_exfat.img      exFAT，根目录项 0x83 里是 UTF-16 中文      -> 移动硬盘
    fix_fat32.img      FAT32，根目录里 0x0F 长名项 + 0x08 短项     -> 备份盘
    fix_fat16.img      FAT16，固定根目录里的 8.3 短项              -> BACKUP
    fix_fat32bpb.img   FAT32，根目录空，只有引导扇区里的 8.3 卷标  -> OLDDISK
    fix_blob.img       纯垃圾字节（过不了 55AA 校验）             -> 无卷标

用法：python3 mkfixtures.py <输出目录>
"""
import os
import struct
import sys


def u16(v):
    return struct.pack("<H", v)


def u32(v):
    return struct.pack("<I", v)


def put(buf, off, data):
    buf[off:off + len(data)] = data


def fat_boot(rootent, fatsz16, fatsz32, rootclus, bpb_label):
    """512 字节 FAT 引导扇区。fatsz16==0 且 rootent==0 -> 判定为 FAT32。"""
    is32 = (fatsz16 == 0 and rootent == 0)
    s = bytearray(512)
    put(s, 0x00, b"\xEB\x58\x90")        # 跳转指令（解析的第一道门）
    put(s, 0x03, b"MSDOS5.0")            # OEM 名
    put(s, 0x0B, u16(512))               # 每扇区字节数
    put(s, 0x0D, b"\x01")                # 每簇扇区数
    put(s, 0x0E, u16(1))                 # 保留扇区数
    put(s, 0x10, b"\x01")                # FAT 个数
    put(s, 0x11, u16(rootent))           # 根目录项数（FAT32 为 0）
    put(s, 0x13, u16(0))                 # 总扇区数（16 位，0 表示用 32 位）
    put(s, 0x15, b"\xF8")                # 介质描述符
    put(s, 0x16, u16(fatsz16))           # FAT 大小（16 位）
    put(s, 0x18, u16(32))                # 每道扇区数
    put(s, 0x1A, u16(64))                # 磁头数
    put(s, 0x1C, u32(0))                 # 隐藏扇区数
    put(s, 0x20, u32(0x4000))            # 总扇区数（32 位）
    put(s, 0x24, u32(fatsz32))           # FAT 大小（32 位）
    put(s, 0x2C, u32(rootclus))          # 根目录首簇（FAT32）
    # 8.3 卷标只有一套偏移：FAT12/16 在 0x2B，FAT32 在 0x47。
    # 两处都写会把 FAT32 的根目录首簇（0x2C-0x2F）连同 FSInfo 一起覆盖掉，
    # 解析出的簇号就成了名字里的字节。
    put(s, 0x47 if is32 else 0x2B, bpb_label[:11].ljust(11))
    put(s, 0x1FE, b"\x55\xAA")           # 结束标记（第二道门）
    return bytes(s)


def exfat_boot(heap_off=1, root_clus=2):
    """512 字节 exFAT 引导扇区。"""
    s = bytearray(512)
    put(s, 0x00, b"\xEB\x76\x90")
    put(s, 0x03, b"EXFAT   ")            # 认 exFAT 就看这 8 个字节
    put(s, 0x40, bytes(8))               # 分区偏移（规范要求为 0）
    put(s, 0x48, struct.pack("<Q", 0x800))
    put(s, 0x50, u32(0))                 # 第一个 FAT 的扇区偏移
    put(s, 0x54, u32(0))
    put(s, 0x58, u32(heap_off))          # 簇堆起始扇区
    put(s, 0x5C, u32(64))                # 簇总数
    put(s, 0x60, u32(root_clus))         # 根目录首簇
    put(s, 0x64, u32(0x12345678))
    put(s, 0x68, u16(0x0100))
    put(s, 0x6C, b"\x09")                # 每扇区字节数 = 1<<9 = 512
    put(s, 0x6D, b"\x00")                # 每簇扇区数 = 1<<0 = 1
    put(s, 0x6E, b"\x01")
    put(s, 0x1FE, b"\x55\xAA")
    return bytes(s)


def exfat_label_entry(name):
    """exFAT 卷标目录项：EntryType 0x83 + 字符数 + UTF-16LE 名字。"""
    e = bytearray(32)
    u = name.encode("utf-16-le")
    e[0] = 0x83
    e[1] = len(u) // 2
    e[2:2 + len(u)] = u
    return bytes(e)


def utf16_units(name):
    u = name.encode("utf-16-le")
    return [u[i] | (u[i + 1] << 8) for i in range(0, len(u), 2)]


def fat_lfn_entry(units13, order, last):
    """FAT 长名项：13 个 UTF-16 码元分散在三段里（5 + 6 + 2）。"""
    e = bytearray(32)
    e[0] = order | (0x40 if last else 0)
    e[11] = 0x0F                         # 属性 = 长名项
    for i in range(13):
        v = units13[i] if i < len(units13) else 0xFFFF
        if i < 5:
            off = 1 + i * 2
        elif i < 11:
            off = 14 + (i - 5) * 2
        else:
            off = 28 + (i - 11) * 2
        put(e, off, u16(v))
    return bytes(e)


def fat_short_label_entry(name11):
    """FAT 卷标短项：11 字节名字 + 属性 0x08。"""
    e = bytearray(32)
    put(e, 0, name11[:11].ljust(11).encode("ascii"))
    e[11] = 0x08
    return bytes(e)


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)
    print("  %-18s %6d bytes" % (os.path.basename(path), len(data)))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."

    # 1) exFAT：簇堆从 1 号扇区起，根目录簇 2 -> 字节偏移 512
    root = exfat_label_entry("移动硬盘") + bytes(32 * 7)
    write(os.path.join(out, "fix_exfat.img"), exfat_boot() + root)

    # 2) FAT32：数据区从 (1 + 1*1 + 0) 扇区起，根目录簇 2 -> 字节偏移 1024
    lfn = fat_lfn_entry(utf16_units("备份盘"), 1, True)
    root = lfn + fat_short_label_entry("BEIFEN~1") + bytes(32 * 6)
    write(os.path.join(out, "fix_fat32.img"),
          fat_boot(0, 0, 1, 2, b"NO NAME") + bytes(512) + root)

    # 3) FAT16：固定根目录在 (1 + 1*1) 扇区处、共 16 项 = 512 字节。
    #    引导扇区里故意写另一个名字，用来证明"读到的是根目录里那一条"。
    root = fat_short_label_entry("BACKUP") + bytes(32 * 15)
    write(os.path.join(out, "fix_fat16.img"),
          fat_boot(16, 1, 0, 0, b"BPBLABEL") + bytes(512) + root)

    # 4) FAT32 但根目录里什么都没有 -> 只能退回引导扇区里的 8.3 卷标
    write(os.path.join(out, "fix_fat32bpb.img"),
          fat_boot(0, 0, 1, 2, b"OLDDISK") + bytes(512) + bytes(512))

    # 5) 不是文件系统（0xA5 连 55AA 都没有）-> 必须老老实实报"没有卷标"
    write(os.path.join(out, "fix_blob.img"), b"\xA5" * 4096)


if __name__ == "__main__":
    main()
