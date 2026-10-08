/*
 * stub.c —— 宿主机侧替换 PS5/FreeBSD 专有系统调用。
 *
 * 提供：
 *   getfsstat()  伪造挂载表（可控，用于穷举各种卷组合）
 *   unmount()    记录调用并模拟成功/失败，可从表里摘掉卷
 *   sync()       Windows 无此调用，空实现
 *   WSAStartup   构造函数，在 main 之前初始化 Winsock（usbmanage 直接用 BSD socket）
 *
 * 由 USBMANAGE_FAKE_EMPTY=1 可模拟"没插 U 盘"（只有内部卷）
 * 由 USBMANAGE_FAKE_EBUSY=1 可模拟"卷被占用，unmount 失败"
 * 由 USBMANAGE_FAKE_LABELS=1 把外接卷的设备名指向 mkfixtures.py 造的合成镜像，
 *   用来真跑"读引导扇区解析卷标"这一段
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <winsock2.h>

#include "sys/mount.h"
#include "sys/sysctl.h"
#include "sys/user.h"

/* ---- Winsock 初始化（必须在 usbmanage.c 的 main 之前）---- */
__attribute__((constructor))
static void usbmanage_host_wsastart(void)
{
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0)
        fprintf(stderr, "[stub] WSAStartup failed\n");
}

/*
 * Windows 上 close() 只认 CRT 文件描述符，而 socket() 返回的是 SOCKET 句柄，
 * 两者不同域。usbmanage.c 里 close() 的对象全是 socket，所以宿主机编译时用
 * -Dclose=usbmanage_host_close 把这处调用换成 closesocket（PS5 上无需此转换）。
 */
int usbmanage_host_close(int fd)
{
    return closesocket((SOCKET)fd);
}

/*
 * getpeername 桩。
 *
 * /shutdown 只放行两种来源：本机回环，或带 X-Requested-With: usbmanage 的
 * 页面请求。宿主机上所有连接都是回环，"非回环必须被拒"这一面测不到，
 * 于是允许用 USBMANAGE_FAKE_PEER_IP 伪造对端地址，把那半边也验到。
 *
 * 第一个参数按 int 收：名字已被 prologue.h 改成 usbmanage_host_getpeername
 * （避开 ws2_32 的同名导入符号），参数类型与那里的声明对齐即可。
 */
int usbmanage_host_getpeername(int fd, struct sockaddr *name, int *namelen)
{
    const char *ip = getenv("USBMANAGE_FAKE_PEER_IP");
    struct sockaddr_in *in;

    (void)fd;
    if (!ip || !*ip)
        ip = "127.0.0.1";

    if (!name || !namelen || *namelen < (int)sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }

    memset(name, 0, sizeof(struct sockaddr_in));
    in = (struct sockaddr_in *)name;
    in->sin_family = AF_INET;
    in->sin_addr.s_addr = inet_addr(ip);
    *namelen = (int)sizeof(struct sockaddr_in);
    return 0;
}

/* ---- 伪造挂载表 ---- */
#define FAKE_MAX 256
static struct statfs g_tab[FAKE_MAX];
static int g_n;
static int g_ready;

/* 记录最近一次 unmount 调用，供测试脚本从日志核对 */
static char g_last_unmount[1024];

static void add(const char *mnt, const char *dev, const char *fst,
                uint64_t blocks, uint64_t bfree)
{
    struct statfs *s;

    if (g_n >= FAKE_MAX)
        return;
    s = &g_tab[g_n++];
    memset(s, 0, sizeof *s);
    s->f_bsize = 4096;
    s->f_blocks = blocks;
    s->f_bfree = bfree;
    s->f_bavail = bfree;
    snprintf(s->f_mntonname, sizeof s->f_mntonname, "%s", mnt);
    snprintf(s->f_mntfromname, sizeof s->f_mntfromname, "%s", dev);
    snprintf(s->f_fstypename, sizeof s->f_fstypename, "%s", fst);
}

static void build_table(void)
{
    g_n = 0;

    if (getenv("USBMANAGE_FAKE_MANY")) {
        /*
         * 复刻真机的形态：主机挂载点非常多（实测 64 条以上：/、/dev、/system*、
         * /user、/data 加一大票 nullfs 沙箱目录），而 USB 盘排在最后。
         * 用来验证"上限截断不会把 U 盘藏起来"。
         */
        int i;
        char m[128];
        for (i = 0; i < 80; i++) {
            snprintf(m, sizeof m, "/mnt/sandbox/dummy%02d/app0", i);
            add(m, "/system/vsh/app/dummy", "nullfs", 100, 10);
        }
        add("/mnt/usb0", "/dev/da2p1", "exfatfs", 16776673, 1022447);
        add("/mnt/usb1", "/dev/da2p2", "exfatfs", 21376583, 8941511);
        g_ready = 1;
        return;
    }

    if (getenv("USBMANAGE_FAKE_EMPTY")) {
        /* 没插 U 盘：只有内部卷——旧版白名单若含 /mnt/ext* 就会在这里误伤 */
        add("/mnt/ext0", "/dev/es0.crypt", "ufs", 1000000, 400000);
        add("/mnt/ext1", "/dev/ssd1.user", "bfs", 5000000, 1000000);
    } else if (getenv("USBMANAGE_FAKE_LABELS")) {
        /*
         * 卷标场景：设备名指向 mkfixtures.py 造出来的合成镜像，
         * 于是 usbmanage.c 里"打开设备读引导扇区"这一段是真跑的——
         * 只有目录名与真实设备不同，解析代码一个字符都没换。
         * 镜像由 run.sh 在本目录生成，服务进程的工作目录就是本目录。
         */
        add("/", "/dev/ssd0.ufs", "ufs", 10000000, 3000000);
        add("/mnt/usb2", "fix_exfat.img", "exfatfs", 1000000, 500000);
        add("/mnt/usb3", "fix_fat32.img", "msdosfs", 1000000, 500000);
        add("/mnt/usb4", "fix_fat16.img", "msdosfs", 1000000, 500000);
        add("/mnt/usb5", "fix_fat32bpb.img", "msdosfs", 1000000, 500000);
        add("/mnt/usb6", "fix_blob.img", "msdosfs", 1000000, 500000);
        add("/mnt/usb7", "no_such_image.img", "msdosfs", 1000000, 500000);
    } else {
        add("/", "/dev/ssd0.ufs", "ufs", 10000000, 3000000);
        add("/mnt/ext0", "/dev/es0.crypt", "ufs", 1000000, 400000);
        add("/mnt/ext1", "/dev/ssd1.user", "bfs", 5000000, 1000000);
        add("/mnt/usb0", "/dev/da0s1", "exfat", 614400000, 300000000);
        add("/mnt/usb1", "/dev/da1s1", "exfat", 766400000, 400000000);
        /*
         * 复刻"应用沙箱把 U 盘挂进自己的视图"——这是真机上
         * "没有进程占用却 Device busy"最可能的根源。两条视图分别覆盖
         * "整盘挂入"与"盘内目录挂入"两种形态，验证 from 前缀匹配的两种分支。
         */
        if (getenv("USBMANAGE_FAKE_USB_VIEWS")) {
            add("/mnt/sandbox/CUSA12345_000/mnt/usb0", "/mnt/usb0",
                "nullfs", 0, 0);
            add("/mnt/sandbox/CUSA67890_000/data/media", "/mnt/usb0/movies",
                "nullfs", 0, 0);
        }
    }
    g_ready = 1;
}

int getfsstat(struct statfs *buf, int bufsize, int flags)
{
    int max, k, i;

    (void)flags;
    if (!g_ready)
        build_table();

    if (!buf || bufsize <= 0)
        return g_n;

    max = bufsize / (int)sizeof(struct statfs);
    k = g_n < max ? g_n : max;
    for (i = 0; i < k; i++)
        buf[i] = g_tab[i];
    return k;
}

int unmount(const char *dir, int flags)
{
    int i;

    snprintf(g_last_unmount, sizeof g_last_unmount, "%s", dir);
    fprintf(stderr, "[stub] unmount(\"%s\", flags=0x%x%s) called\n",
            dir, flags, (flags & MNT_FORCE) ? " MNT_FORCE" : "");

    if (getenv("USBMANAGE_FAKE_EBUSY")) {
        errno = EBUSY;
        return -1;
    }

    /*
     * 复刻用户实际遇到的那条链路：第一次 unmount 因占用失败，解除占用之后
     * 再卸就成功。没有这个开关，"占用 -> 解除 -> 卸载成功"只能分段验，
     * 串不起来。
     */
    if (getenv("USBMANAGE_FAKE_EBUSY_ONCE")) {
        static int n;
        if (n++ == 0) {
            errno = EBUSY;
            return -1;
        }
    }

    if (!g_ready)
        build_table();

    for (i = 0; i < g_n; i++) {
        if (strcmp(g_tab[i].f_mntonname, dir) == 0) {
            int j;
            for (j = i; j < g_n - 1; j++)
                g_tab[j] = g_tab[j + 1];
            g_n--;
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}

/* Windows 没有 sync(2)；PS5 上它是真正的落盘动作，宿主机上为空实现 */
void sync(void)
{
}

/* ============================================================
 * Orbis 系统服务桩件 —— "自动弹界面"用的三个接口
 *
 * 宿主机上没有这些接口，这里顶替掉，并**把调用记录下来**，
 * 让测试能断言"usbmanage 确实去调了、参数是什么"，而不是只看进程没崩。
 *
 * USBMANAGE_FAKE_NOUI_FAIL=1 时返回 -1，用于验证失败码是否被如实透传到 /diag。
 * （真机上失败返回的是 0x8002xxxx 一类的负值，桩件用 -1 只作标记。）
 * ============================================================ */
#include <stdbool.h>

int sceUserServiceInitialize(void *p)
{
    (void)p;
    fprintf(stderr, "[stub] sceUserServiceInitialize called\n");
    return 0;
}

int sceUserServiceTerminate(void)
{
    return 0;
}

int sceSystemServiceLaunchWebBrowser(const char *uri, void *p)
{
    (void)p;
    fprintf(stderr, "[stub] sceSystemServiceLaunchWebBrowser(\"%s\") called\n",
            uri ? uri : "(null)");
    if (getenv("USBMANAGE_FAKE_NOUI_FAIL"))
        return -1;
    return 0;
}

/*
 * 通知内容落一份到 last_notify.json，测试脚本据此核对内容（比如地址与端口）。
 * 同时打一行摘要到 stderr，便于从 server.log 直接看调用是否发生。
 */
int sceNotificationSend(int userId, bool isLogged, const char *payload)
{
    FILE *f;
    size_t len = payload ? strlen(payload) : 0;

    fprintf(stderr,
            "[stub] sceNotificationSend(user=%d, logged=%d, bytes=%zu, hasMsg=%d, hasAction=%d) called\n",
            userId, (int)isLogged, len,
            payload && strstr(payload, "\"message\"") ? 1 : 0,
            payload && strstr(payload, "\"actionUrl\"") ? 1 : 0);

    if (payload) {
        f = fopen("last_notify.json", "wb");
        if (f) {
            fwrite(payload, 1, len, f);
            fclose(f);
        }
    }

    if (getenv("USBMANAGE_FAKE_NOUI_FAIL"))
        return -1;
    return 0;
}

/*
 * 内核通知桩（对应 SDK 的 samples/notify_debug）。
 *
 * 结构体在这里按同样的布局重新声明一份，而不是共用一个头：桩件与
 * usbmanage.c 是两个翻译单元，链接只看符号名，不必（也不该）把测试用的
 * 类型塞进源码。文本落一份 notify_kernel.txt，供测试核对内容。
 */
typedef struct {
    char useless1[45];
    char message[3075];
} host_notify_request_t;

int sceKernelSendNotificationRequest(int unk, host_notify_request_t *req,
                                     size_t len, int flags)
{
    const char *msg = (req && len > sizeof(req->useless1)) ? req->message : "";
    FILE *f;

    fprintf(stderr, "[stub] sceKernelSendNotificationRequest(unk=%d, len=%zu, "
                    "msg=\"%s\") called\n", unk, len, msg);

    f = fopen("notify_kernel.txt", "wb");
    if (f) {
        fwrite(msg, 1, strlen(msg), f);
        fclose(f);
    }

    if (getenv("USBMANAGE_FAKE_NOUI_FAIL"))
        return -1;
    return 0;
}

/* ============================================================
 * 进程与文件描述符桩件 —— "谁在占用这个盘"用的 sysctl
 *
 * 宿主机上没有 sysctl，也没有 kinfo_proc/kinfo_file 这两张表。
 * 这里用环境变量伪造，让"占用发现"和"解除占用"这两条真实逻辑能在
 * 宿主机上被穷举验证（真机只剩"内核放不放行这两条 sysctl"一项未知）。
 *
 *   USBMANAGE_FAKE_PROCS   进程表： "87:elfldr.elf;93:payload.elf"
 *                          （pid:进程名，分号分隔）
 *   USBMANAGE_FAKE_FDS     fd 表：  "93:-1:/mnt/usb0;99:12:/mnt/usb0/a.mkv"
 *                          （pid:fd:路径；fd 为负即 KF_FD_TYPE_* 特殊项）
 *                          没列到的进程会得到一个 cwd=/ 的条目——真机上
 *                          每个进程至少也有 cwd 与 root，不会是空表
 *   USBMANAGE_FAKE_SELF_PID getpid() 的返回值，用来验证"不杀自己"
 *   USBMANAGE_FAKE_PROC_FAIL=1      KERN_PROC_PROC 整体失败
 *   USBMANAGE_FAKE_FILEDESC_FAIL=1  KERN_PROC_FILEDESC 整体失败（模拟内核不放行）
 * ============================================================ */

#define FK_MAX 64

/*
 * 被 kill 桩"杀掉"的 pid。
 * 不加这一层的话，"解除占用之后就不再占用"这条链路根本没法验证——
 * 进程表纹丝不动，重扫当然还是原样。真机上进程确实会消失，桩件也照做。
 * USBMANAGE_FAKE_KILL_NOREMOVE=1 时保留（用来验证 TERM 打不动的场景）。
 */
static int g_killed[FK_MAX];
static int g_killed_n;

static int is_killed(int pid)
{
    int i;
    for (i = 0; i < g_killed_n; i++) {
        if (g_killed[i] == pid)
            return 1;
    }
    return 0;
}

static void mark_killed(int pid)
{
    if (g_killed_n < FK_MAX)
        g_killed[g_killed_n++] = pid;
}

static int parse_procs(struct kinfo_proc *out, int max)
{
    const char *s = getenv("USBMANAGE_FAKE_PROCS");
    int n = 0;

    if (!s)
        return 0;

    while (*s && n < max) {
        const char *semi = strchr(s, ';');
        const char *e = semi ? semi : s + strlen(s);
        const char *colon = memchr(s, ':', (size_t)(e - s));

        if (colon && !is_killed(atoi(s))) {
            size_t l = (size_t)(e - colon - 1);
            struct kinfo_proc *k = &out[n];

            memset(k, 0, sizeof *k);
            k->ki_structsize = (int)sizeof *k;
            k->ki_pid = atoi(s);
            k->ki_ppid = 1;
            if (l > COMMLEN)
                l = COMMLEN;
            memcpy(k->ki_comm, colon + 1, l);
            k->ki_comm[l] = '\0';
            n++;
        }
        if (!semi)
            break;
        s = semi + 1;
    }
    return n;
}

static int parse_files(int want_pid, struct kinfo_file *out, int max)
{
    const char *s = getenv("USBMANAGE_FAKE_FDS");
    int n = 0;

    if (!s || is_killed(want_pid))
        return 0;

    while (*s && n < max) {
        const char *semi = strchr(s, ';');
        const char *e = semi ? semi : s + strlen(s);
        const char *c1 = memchr(s, ':', (size_t)(e - s));

        if (c1) {
            const char *c2 = memchr(c1 + 1, ':', (size_t)(e - c1 - 1));
            if (c2 && atoi(s) == want_pid) {
                size_t l = (size_t)(e - c2 - 1);
                struct kinfo_file *k = &out[n];

                memset(k, 0, sizeof *k);
                k->kf_structsize = (int)sizeof *k;
                k->kf_pid = want_pid;
                k->kf_fd = atoi(c1 + 1);
                if (l >= PATH_MAX)
                    l = PATH_MAX - 1;
                memcpy(k->kf_path, c2 + 1, l);
                k->kf_path[l] = '\0';
                n++;
            }
        }
        if (!semi)
            break;
        s = semi + 1;
    }
    return n;
}

/*
 * sysctl 桩。只实现 usbmanage.c 用到的两个查询，两段式协议（先问长度、
 * 再取内容）按真实语义实现——调用方正是那么用的。
 */
int sysctl(const int *name, u_int namelen, void *oldp, size_t *oldlenp,
           const void *newp, size_t newlen)
{
    (void)newp;
    (void)newlen;

    if (namelen >= 3 && name[0] == CTL_KERN && name[1] == KERN_PROC) {

        if (name[2] == KERN_PROC_PROC) {
            static struct kinfo_proc tab[FK_MAX];
            size_t len;
            int n;

            if (getenv("USBMANAGE_FAKE_PROC_FAIL")) {
                errno = EPERM;
                return -1;
            }
            n = parse_procs(tab, FK_MAX);
            len = (size_t)n * sizeof(struct kinfo_proc);
            if (!oldp || !oldlenp) {
                if (oldlenp)
                    *oldlenp = len;
                return 0;
            }
            if (*oldlenp < len)
                len = *oldlenp;
            memcpy(oldp, tab, len);
            *oldlenp = (size_t)n * sizeof(struct kinfo_proc);
            return 0;
        }

        if (name[2] == KERN_PROC_FILEDESC && namelen >= 4) {
            static struct kinfo_file tab[FK_MAX];
            size_t len;
            int n;

            if (getenv("USBMANAGE_FAKE_FILEDESC_FAIL")) {
                errno = EPERM;
                return -1;
            }
            n = parse_files(name[3], tab, FK_MAX);
            if (n == 0) {
                /* 真机上每个进程至少也有 cwd 与 root，不会是空表 */
                struct kinfo_file *k = &tab[0];
                memset(k, 0, sizeof *k);
                k->kf_structsize = (int)sizeof *k;
                k->kf_pid = name[3];
                k->kf_fd = KF_FD_TYPE_CWD;
                snprintf(k->kf_path, sizeof k->kf_path, "/");
                n = 1;
            }
            len = (size_t)n * sizeof(struct kinfo_file);
            if (!oldp || !oldlenp) {
                if (oldlenp)
                    *oldlenp = len;
                return 0;
            }
            if (*oldlenp < len)
                len = *oldlenp;
            memcpy(oldp, tab, len);
            *oldlenp = (size_t)n * sizeof(struct kinfo_file);
            return 0;
        }
    }

    errno = EINVAL;
    return -1;
}

/* getpid 桩：让测试能指定"自己"是哪个 pid，以验证"不杀自己"这条护栏 */
int getpid(void)
{
    const char *s = getenv("USBMANAGE_FAKE_SELF_PID");
    return s ? atoi(s) : 4242;
}

/*
 * kill 桩：不真的杀进程（Windows 上也没有这套语义），只把调用记下来，
 * 并把该 pid 标成"已消失"，让后续的进程表/fd 表查询看不到它——
 * 这样"解除占用之后就不再占用"这条链路才能在宿主机上被验证。
 *
 * 落一份 kill.log 供测试脚本断言"对谁、发了什么信号"。
 * USBMANAGE_FAKE_KILL_FAIL=1     调用返回失败（验证失败是否被如实回报）
 * USBMANAGE_FAKE_KILL_NOREMOVE=1 记下调用但进程不消失（验证 TERM 打不动的场景）
 */
int kill(int pid, int sig)
{
    FILE *f;
    const char *sig_name = (sig == 15) ? "TERM" : (sig == 9) ? "KILL" : "?";

    fprintf(stderr, "[stub] kill(pid=%d, sig=%d/%s) called\n", pid, sig, sig_name);

    f = fopen("kill.log", "ab");
    if (f) {
        fprintf(f, "%d %s\n", pid, sig_name);
        fclose(f);
    }

    if (getenv("USBMANAGE_FAKE_KILL_FAIL")) {
        errno = EPERM;
        return -1;
    }
    if (!getenv("USBMANAGE_FAKE_KILL_NOREMOVE"))
        mark_killed(pid);
    return 0;
}

