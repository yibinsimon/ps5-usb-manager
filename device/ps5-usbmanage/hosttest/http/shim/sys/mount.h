/*
 * sys/mount.h 的宿主机桩件。
 *
 * 宿主机（Windows/Git Bash）没有 FreeBSD 的 <sys/mount.h>，这里只提供
 * usbmanage.c 用到的部分：struct statfs 的字段、MNT_NOWAIT、两个原型。
 * 目的是让 usbmanage.c **原文**（不改一个字符）能在宿主机上编译成可执行文件，
 * 从而把 HTTP 层真实跑起来验证。
 *
 * 字段顺序/类型与 FreeBSD 不必一致——宿主机上没有任何东西依赖真实 ABI，
 * 只依赖 usbmanage.c 里对这些字段的用法。
 */
#ifndef USBMANAGE_HOST_SHIM_SYS_MOUNT_H
#define USBMANAGE_HOST_SHIM_SYS_MOUNT_H

#include <stdint.h>

#define MNT_NOWAIT 2
#define MNT_WAIT   1
/* 取值与真机 SDK 的 sys/mount.h 一致：MNT_FORCE = 0x0000000000080000 */
#define MNT_FORCE  0x00080000

struct statfs {
    uint32_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    char     f_mntonname[1024];
    char     f_mntfromname[1024];
    char     f_fstypename[64];
};

int getfsstat(struct statfs *buf, int bufsize, int flags);
int unmount(const char *dir, int flags);

#endif /* USBMANAGE_HOST_SHIM_SYS_MOUNT_H */
