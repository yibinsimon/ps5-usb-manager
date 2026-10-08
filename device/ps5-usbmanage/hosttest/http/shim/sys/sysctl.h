/*
 * 宿主机（Windows / mingw-w64）没有 <sys/sysctl.h>，这里补一个最小替身，
 * 只为让 usbmanage.c **原文**在宿主机上能编过。
 *
 * 只用得到这几个常量与这一个原型，行为由 stub.c 提供（伪造进程表与 fd 表）。
 *
 * 数值取自真机 SDK 的 target/include/sys/sysctl.h，保持一致以免误导：
 *   CTL_KERN=1  KERN_PROC=14  KERN_PROC_PROC=8  KERN_PROC_FILEDESC=33
 *
 * 真机编译时用的是 SDK 自带的真头文件，不会走到这里。
 */
#ifndef USBMANAGE_SHIM_SYS_SYSCTL_H
#define USBMANAGE_SHIM_SYS_SYSCTL_H

#include <sys/types.h>

#define CTL_KERN           1
#define KERN_PROC          14
#define KERN_PROC_PROC     8
#define KERN_PROC_FILEDESC 33

typedef unsigned int u_int;

int sysctl(const int *name, u_int namelen, void *oldp, size_t *oldlenp,
           const void *newp, size_t newlen);

#endif
