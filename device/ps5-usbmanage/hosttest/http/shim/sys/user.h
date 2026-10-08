/*
 * 宿主机替身：<sys/user.h> 里 usbmanage.c 用到的那部分。
 *
 * 字段名与类型照抄真机 SDK（target/include/sys/user.h），但**布局是宿主机
 * 测试专用的**，真机用的是 SDK 自己的头文件。这一点必须说清楚，因为它决定
 * 了这套测试能证明什么、不能证明什么：
 *
 *   能证明的：遍历逻辑本身 —— 按 *_structsize 步进、路径与挂载点的边界
 *             比较、谁能被杀谁不能被杀的判定、以及 JSON 输出。
 *   不能证明的：真机上两个 sysctl 是否被内核放行、记录的真实布局。
 *             这两项只能上机验，已写在 README 的"上机验收"里。
 *
 * 之所以仍然照抄字段名，是因为按名字访问就绕开了偏移量，只要首字段是
 * *_structsize，遍历行为在两边同构 —— 这正是 SDK 官方 samples/ps 的写法。
 */
#ifndef USBMANAGE_SHIM_SYS_USER_H
#define USBMANAGE_SHIM_SYS_USER_H

#include <sys/types.h>

/* mingw 的 <sys/types.h> 没有 uid_t（真机 FreeBSD 有），这里补一个。
   真机上编译不会走到这行。 */
#ifndef USBMANAGE_SHIM_UID_T
#define USBMANAGE_SHIM_UID_T
typedef unsigned int uid_t;
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define COMMLEN     19
#define LOGNAMELEN  17
#define LOGINCLASSLEN 17

/* fd 表中的"特殊项"。数值取自真机 SDK 的 sys/user.h。 */
#define KF_FD_TYPE_CWD   (-1)   /* 当前工作目录 */
#define KF_FD_TYPE_ROOT  (-2)   /* 根目录 */
#define KF_FD_TYPE_JAIL  (-3)   /* jail 目录 */
#define KF_FD_TYPE_TRACE (-4)   /* ktrace vnode */
#define KF_FD_TYPE_TEXT  (-5)   /* 可执行体本身 */
#define KF_FD_TYPE_CTTY  (-6)   /* 控制终端 */

struct kinfo_proc {
    int   ki_structsize;
    pid_t ki_pid;
    pid_t ki_ppid;
    uid_t ki_uid;
    char  ki_stat;
    char  ki_login[LOGNAMELEN + 1];
    char  ki_comm[COMMLEN + 1];
    char  ki_loginclass[LOGINCLASSLEN + 1];
    int   ki_numthreads;
};

struct kinfo_file {
    int   kf_structsize;
    int   kf_type;
    int   kf_fd;
    int   kf_flags;
    int   kf_vnode_type;
    pid_t kf_pid;
    char  kf_path[PATH_MAX];
};

#endif
