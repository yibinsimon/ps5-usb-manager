/*
 * prologue.h —— 宿主机编译时用 -include 强制最先载入的适配层。
 *
 * 只处理 Windows 与 PS5/FreeBSD 的 API 形态差异，**不碰 usbmanage.c 一个字符**：
 *   1) Windows 没有 sync(2) -> 补声明（实现见 stub.c，空函数）
 *   2) winsock2.h 的 setsockopt 第 4 参是 const char*，而 BSD 上是 const void*。
 *      用同名函数宏把参数转一下，宏体内不再展开自身，因此仍指向真实函数。
 *   3) getpeername 在 winsock 里是真实的导入函数（__declspec(dllimport)），
 *      桩件同名定义会与 ws2_32 的导入库撞车（ld.lld: duplicate symbol）。
 *      这里把 usbmanage.c 里的这个名字整体改掉，桩件按新名字提供实现；
 *      PS5 构建不经过本文件，用的是真名。
 *
 * 注意第 3 条的宏必须放在 winsock2.h 之后：先让 winsock 用它自己的真名完成
 * 声明，再改 usbmanage.c 用到的那个名字，两边就不会互相打架。
 */
#ifndef USBMANAGE_HOST_PROLOGUE_H
#define USBMANAGE_HOST_PROLOGUE_H

#include <winsock2.h>

/* 1) sync() */
void sync(void);

/* 2) setsockopt 的 optval 类型差异 */
#undef setsockopt
#define setsockopt(s, l, o, v, n) setsockopt((s), (l), (o), (const char *)(v), (n))

/* 3) getpeername 与 ws2_32 的导入符号重名 */
#undef getpeername
#define getpeername usbmanage_host_getpeername

/* 宿主机上 socklen_t 就是 int（见 shim/sys/socket.h），此处按 int 声明。 */
int usbmanage_host_getpeername(int fd, struct sockaddr *name, int *namelen);

#endif
