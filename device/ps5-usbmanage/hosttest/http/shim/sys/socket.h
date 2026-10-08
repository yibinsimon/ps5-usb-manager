/*
 * 宿主机桩件：sys/socket.h
 *
 * llvm-mingw 精简掉了 mingw-w64 的 BSD 兼容头（sys/socket.h、netinet/in.h、
 * arpa/inet.h），而 Windows 的 socket API 全在 winsock2.h 里。
 * 这三个转发头只是把这层名字对回去，让 usbmanage.c 原文可编译。
 * PS5 构建走 SDK 自带的真实 BSD 头，与本目录无关。
 */
#ifndef USBMANAGE_HOST_SHIM_SYS_SOCKET_H
#define USBMANAGE_HOST_SHIM_SYS_SOCKET_H

#include <winsock2.h>

#ifndef _SOCKLEN_T_DEFINED
#define _SOCKLEN_T_DEFINED
typedef int socklen_t;
#endif

#endif
