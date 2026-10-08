/*
 * usbmanage.c — PS5 USB 存储管理服务（枚举 + 安全卸载）
 *
 * 定位：常驻型 PS5 payload。PS5（Orbis OS）对媒体 U 盘只挂载、不提供图形化
 *       弹出入口，本 payload 在主机侧补上"列出 + 安全卸载"能力。
 *
 * 接口（局域网 HTTP，默认端口 9100）：
 *   GET /                      -> 内置选择页（HTML），浏览器里点选要卸载哪个卷
 *   GET /icon.png              -> 应用图标（96x96 PNG）；/favicon.ico 返回同一份
 *   GET /ping                  -> "pong"
 *   GET /version               -> {"app","name","name_hant","name_en","version","lang"}
 *   GET /list                  -> JSON，列出可安全卸载的外接卷（仅 /mnt/usb*）
 *   GET /list?all=1            -> JSON，列出全部挂载点（只读诊断用）
 *   GET /eject?mount=/mnt/usb0 -> sync 落盘 + unmount 指定卷，返回 JSON 结果
 *                                失败若是 EBUSY，响应里直接带上"谁在占用"
 *   GET /eject?mount=...&force=1 -> 强制卸载（MNT_FORCE）
 *   GET /holders?mount=/mnt/usb0 -> 谁打开着这个盘下面的文件（进程 + 路径）
 *   GET /release?mount=...&confirm=1 -> 解除占用（终止占用进程 + 卸掉沙箱
 *                                nullfs 挂接）并重试卸载，常规卸不掉自动补 force
 *   GET /procs                 -> 进程清单（诊断用，含是否可被解除占用）
 *   GET /openui                -> 让主机把界面弹到它自己的屏幕上（浏览器 + 通知）
 *   GET /diag                  -> 诊断：端口、单实例判定、UI 与通知的返回码
 *   GET /shutdown              -> 令本进程彻底退出。来源限本机回环，或带
 *                                 X-Requested-With: usbmanage 的页面请求
 *
 * 界面语言：简体中文 / 繁体中文 / 英文，规则见下方「界面语言」一节。
 *
 * ============================================================
 * 一、卸载流程：sync -> unmount -> 回读校验
 *
 *   三步缺一不可。只 sync 不卸载，卷还挂在主机上；只卸载不 sync，写缓存里的
 *   数据留在盘上，下次挂载就是"文件错误，需要修复"。回读校验是第三步：卸完
 *   重新读一次挂载表，确认它真的不在了，而不是把"调用没报错"当成"已经卸掉"。
 *
 * ============================================================
 * 二、占用查询与解除（unmount 报 EBUSY 时）
 *
 *   只说一句 Device busy 等于没说。Orbis 是 FreeBSD 内核，进程与文件描述符表
 *   都能通过 sysctl 取到，所以占用做成可查、可解：
 *
 *     /holders  列出打开了该挂载点下任何文件的进程（pid / 进程名 / 打开的是
 *               哪个路径 / 是普通 fd 还是 cwd、可执行体本身）
 *     /release  把这些进程终止掉，然后重试卸载
 *
 *   取数据的方式（与 SDK 官方 samples/ps 同源，真机未知项只有"内核是否放行
 *   这两条 sysctl"）：
 *     sysctl(CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0)        进程表
 *     sysctl(CTL_KERN, KERN_PROC, KERN_PROC_FILEDESC, pid) 该进程的 fd 表
 *   两者都按结构体首字段的 *_structsize 步进（官方样例就是这么遍历的）。
 *   SDK 只给了 kinfo_getallproc/kinfo_getfile 的声明、没给实现（无 libutil），
 *   所以这里自己按 libutil 的做法封了一层。
 *
 *   解除占用的取舍：
 *     只对"确实打开了该挂载点下文件"的进程动手，判据是收紧的；即便如此仍保留
 *     三道不杀的护栏——不动自己（否则等于自杀，服务就没了）、不动 pid <= 1、
 *     不动 Sce* 前缀的 Sony 系统组件。宁可留一个解除不掉的，也不要误杀系统
 *     进程。先 SIGTERM 再 SIGKILL，动作与结果都如实回报。
 *
 *   还有一类占用查不到人：应用沙箱把 U 盘以 nullfs 挂进
 *   /mnt/sandbox/<TITLEID>_000/...，这种引用不属于任何进程，fd 表里没有，
 *   unmount 时却是 EBUSY。/release 与 /eject 的失败路径都会先卸掉"源是这块盘"
 *   的 nullfs 挂接（只碰 nullfs、只碰源指向这块盘的），常规 unmount 失败再自动
 *   补一发 MNT_FORCE。"解除占用并卸载"的语义就是把盘安全卸下来：能自动兜底的
 *   都自动做，每一步成败都写进回执。
 *
 *   页面这一侧有个坑值得记一笔：load() 用 box.innerHTML 整表重建，任何直接
 *   append 到卡片上的节点（比如刚算出来的那颗「解除占用并卸载」按钮）下一次
 *   刷新就没了。就地反馈必须先记进 pending，由 applyPending() 在重绘后挂回。
 *   这类问题落在页面 JS 这一层，只有 curl 打接口的测试覆盖不到，所以另有
 *   hosttest/web/（把页面脚本抽出来在极简 DOM 上跑，断言按钮真的出现）。
 * ============================================================
 *
 * ============================================================
 * 三、单实例：重复加载不会堆积进程
 *
 *   工具箱（DB's 工具箱 / pldmgr）加载 payload 时，**每个都用同一个进程名
 *   `payload.elf`**，也不会杀掉上一次加载的实例，界面里还写着"请勿重复加载"。
 *   所以"认自己"不能靠进程名，只能靠"端口 + /version 里的自述"。
 *
 *   启动规则（都在 bind **之前**判定，不能等 bind 失败再补救——Windows 的
 *   SO_REUSEADDR 允许两个进程同时监听同一端口，等 bind 报错时旧实例还活着）：
 *
 *      端口空闲                      -> 正常启动
 *      已有实例、且版本相同          -> **直接激活**：把界面弹到屏幕上，
 *                                       本进程立刻退出，绝不起第二个
 *      已有实例、但版本不同          -> 请旧实例退出后接管（保证换版本生效）
 *      端口上有非本程序的东西        -> 明确报错退出，不硬抢
 *
 *   --force 跳过上述判定，一律接管。
 *
 *   页面上的「关闭服务」（/shutdown）是明确的退出入口：点了就从进程里彻底退出
 *   （= 释放端口，可以重新加载），不必去工具箱里杀进程。
 *   鉴权：来源必须是本机回环，或者请求带 X-Requested-With: usbmanage
 *   （页面自己的 XHR 会带；网页上的 <img src>、地址栏回车都带不上，因此既拦得住
 *   CSRF，也拦得住"地址栏手滑"）。
 *
 * ============================================================
 * 四、卷标
 *
 *   光看 /mnt/usb0、/dev/da0s1 分不清哪块是哪个盘，插了两块更容易卸错。
 *   内核的 statfs 不带卷标（只有 f_mntfromname/f_fstypename），所以直接按引导
 *   扇区解析：exFAT 与 FAT12/16/32（PS5 只认这两种外接格式）。
 *   只读打开设备、只读 512B 引导扇区 + 最多 64KB 根目录，绝不写盘。
 *   解析分两层：纯函数（可在宿主机上拿合成扇区穷举）与 I/O（打开设备）。
 *   读不到就报空串，不影响卸载。
 *
 * ============================================================
 * 五、屏幕通知的两条通道
 *
 *   sceNotificationSend（InteractiveToastTemplateB 那套）与内核接口
 *   sceKernelSendNotificationRequest（SDK 的 samples/notify_debug 走这条）。
 *   两条都发，返回码都进 /diag（rc.notify / rc.knotify），哪条生效一看便知。
 *   真机上两条都不显示也不影响使用：界面会自己弹出来。
 * ============================================================
 *
 * ============================================================
 * 六、界面与自动弹出
 *
 *   选择页是本服务自己提供的网页：http://<PS5_IP>:9100/
 *   在 PC / 手机 / 电视的浏览器里打开它，就能看到可卸载卷的列表与「安全弹出」
 *   按钮。页面编译进 elf（web/index.html 经 xxd 转成数组），不依赖外部文件、
 *   不依赖联网。
 *
 *   加载 payload 之后主机还会自己把界面拉起来（电视上直接看到选择页），并弹
 *   一条带局域网地址的通知，方便用手机/电脑打开。靠的是 Orbis 的两个接口
 *   （SDK 自带官方样例）：
 *
 *     sceSystemServiceLaunchWebBrowser(url, 0)   -> 打开主机内置浏览器
 *                                                   （samples/browser/main.c）
 *     sceNotificationSend(0xFE, true, json)      -> 弹一条屏幕通知
 *                                                   （samples/notify/main.c）
 *
 *   链接库分别是 -lSceSystemService -lSceUserService 与 -lSceNotification。
 *   调 sceSystemServiceLaunchWebBrowser 之前需先 sceUserServiceInitialize(0)。
 *   两个调用的返回码都留在全局，由 /diag 原样报出——主机上不生效时据此判断是
 *   哪个环节被拒，而不是靠猜。启动参数 --no-ui 可关掉自动拉起，/openui 可随时
 *   让主机重新打开界面。
 *
 *   界面与通知里的地址一律用**主机在局域网里的 IP**（local_ip() 取的那个），
 *   不用 127.0.0.1：电视上看到的地址与从手机/电脑打开的是同一个，页脚显示的
 *   也就是你实际能用的那个。取不到局域网地址时才回退 127.0.0.1。
 *
 *   防误会的两条：
 *     - 浏览器里直接打开 /list 或 /eject 时 302 跳到 `/`，不会甩出裸 JSON
 *       （判据是请求头 Accept 含 text/html）。顺带副作用：/eject 不可能被
 *       "地址栏回车"误触发，只能从界面点。
 *     - /list?all=1 会带上 "shown" 与 "truncated"。主机挂载点很多（实测 64 条
 *       以上），诊断视图可能被上限或缓冲截断，明确标出来。
 *
 *   应用图标同样编进 elf：web/icon.png 经 xxd 转成 web/icon_png.h，由
 *   GET /icon.png 提供，/favicon.ico 返回同一份字节（浏览器按内容嗅探，PNG 放
 *   在 .ico 路径上没问题）。图标主文件在 assets/icon/，96x96 是页面里 32~40 px
 *   显示尺寸的 2~3 倍图；换图只需换 assets/icon/ 下的定稿再重跑 build.sh。
 * ============================================================
 *
 * ============================================================
 * 挂载点的事实（本机实测 + 社区文档核对，勿凭直觉改）：
 *
 *   /mnt/usb0            <-- 外置 USB 存储，**只有这个前缀可以卸载**
 *   /mnt/ext0  ufs  /dev/es0.crypt   内部：扩展存储加密卷
 *   /mnt/ext1  bfs  /dev/ssd1.user   内部：内置 SSD 用户分区
 *
 *   白名单因此只放行 /mnt/usb<数字>，其余全拒。判据必须是正向白名单：/list 在
 *   无 U 盘时返回的恰好就是 /mnt/ext0、/mnt/ext1，把判据放宽成"/mnt 前缀"就会
 *   去 unmount 主机内部存储；而 /mnt 本身是 tmpfs、/mnt/sandbox/** 是沙箱视图，
 *   也都不是能卸的东西。配套两道：/eject 前先在当前挂载表里校验该挂载点确实
 *   存在；/list?all=1 只作只读诊断视图，不参与卸载。
 *
 *   真机上剩下的挂载点绝大多数是伪 FS（/dev 是 devfs、/system_tmp、/mnt 是
 *   tmpfs）与 /mnt/sandbox/<TITLEID>_000/** 的 nullfs 沙箱视图。
 * ============================================================
 *
 * 安全约束：
 *   - 服务本身无鉴权，仅用于可控局域网，勿暴露到公网。
 *
 * 查询串解码：
 *   HTTP 规范允许客户端把查询串里的 '/' 编码成 %2F，Python 的
 *   urllib.parse.urlencode 默认就这么干。挂载点必须按字面量匹配，
 *   所以解析出 mount= 之后先做一次百分号解码，再进白名单。
 *
 * 编译：见仓库根 README.md「构建 payload」一节（build.sh 已实测）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>

/*
 * kill(2)。这里显式声明，不依赖 <signal.h> 一定给出它。
 *
 * 原因：FreeBSD 的 <signal.h> 声明了 POSIX 的 kill，而宿主机（mingw-w64）
 * 的 <signal.h> 只有 ISO C 那部分、不含 kill —— 宿主机端到端测试要编同一份
 * 源码，缺了声明就编不过。FreeBSD 的签名是 int kill(__pid_t, int)，__pid_t
 * 就是 int，与此处完全一致，因此不构成冲突声明。
 */
int kill(int pid, int sig);

/* SIGTERM / SIGKILL 取值来自 <signal.h>（两个平台一致），此处仅为转述。 */
#define SIG_TERM 15
#define SIG_KILL 9

/*
 * 内置选择页（web/index.html 的字节数组，由 build.sh 用 xxd 生成）。
 * 每次编译都重新生成，保证页面与源码一致；若不慎缺失，给出清晰报错。
 */
#include "web/index_html.h"

/*
 * 应用图标（web/icon.png 的字节数组，同样由 build.sh 用 xxd 生成）。
 * 页面用 <link rel="icon"> 指向 /icon.png，所以它必须编进 elf ——
 * 不依赖外部文件、不依赖联网，与页面同一次构建。
 */
#include "web/icon_png.h"

/*
 * Orbis 系统服务接口。
 *
 * 这三行是与 SDK 官方样例逐字对齐的声明（samples/browser/main.c、
 * samples/notify/main.c）——SDK 没有为它们提供头文件，官方样例就是自己
 * 声明。签名抄错会直接崩，故此处不做任何"顺手改动"。
 */
int sceUserServiceInitialize(void *);
int sceUserServiceTerminate(void);
int sceSystemServiceLaunchWebBrowser(const char *uri, void *);

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE
int sceNotificationSend(int userId, bool isLogged, const char *payload);

/*
 * 第二条通知通道：内核通知（SDK 的 samples/notify_debug 用的就是它）。
 *
 * 真机上 sceNotificationSend 那套（InteractiveToastTemplateB）不显示，
 * 而这条是"经典"的屏幕通知，PS4/PS5 上大量 payload 用它。结构体布局逐字
 * 对齐官方样例（notify_debug/main.c）——45 字节无用前缀 + 3075 字节文本。
 */
typedef struct {
    char useless1[45];
    char message[3075];
} usbmanage_notify_request_t;

int sceKernelSendNotificationRequest(int, usbmanage_notify_request_t *, size_t, int);

#define LISTEN_PORT 9100
#define VERSION      "1.0.0"

/*
 * 名称。分两类，改的时候别混：
 *
 *   显示名（给人看）：中文「PS5 USB管理器」、英文「PS5 USB Manager」。
 *     用在界面标题、屏幕通知、接口的自述字段上。简繁两个中文名目前同字
 *     （管理器三个字简繁一致），分成两个宏是为了将来真要分岔时只改一处。
 *
 *   机器标识 APP_ID（给机器看）：固定 `usbmanage`。
 *     它是 payload 文件名（usbmanage.elf）、工具箱里的插件目录名、
 *     以及 JSON 里的 app 字段。**不要跟着显示名一起改**——那会把
 *     工具箱的扫描路径、部署脚本和既有文档一起打断，纯风险零收益。
 */
#define APP_NAME_ZH   "PS5 USB管理器"
#define APP_NAME_HANT "PS5 USB管理器"
#define APP_NAME_EN   "PS5 USB Manager"
#define APP_ID        "usbmanage"

#define MAX_VOLUMES 64
#define BUFSZ       8192

/*
 * ---------------------------------------------------------------
 * 界面语言：简体中文 / 繁体中文 / 英文
 *
 * 约定：**系统语言是中文就上中文界面，简体走简体、繁体走繁体；
 * 其余一切语言（含日、韩、欧语）走英文**。
 *
 * 判定来源是请求头 Accept-Language —— 主机的内置浏览器会自动带上它，
 * 而它本身就由主机系统语言决定。没有任何可用信号时按英文走。
 * 另给 `?lang=zh-Hans|zh-Hant|en` 作为最高优先级的覆盖口，HTTP 层测试
 * 靠它把三种语言的输出都断言到，脚本与 PC 端也能显式指定。
 *
 * 屏幕通知是在任何请求发生**之前**发的，那时候没有请求头可用，所以改取
 * 环境变量 LANG / LC_ALL 作最佳努力判定，同样默认英文。通知在真机上本来
 * 也不显示（界面会自动弹出），这一项判错不影响可用性。
 * ---------------------------------------------------------------
 */
/* >>> lang-types >>> */
enum { LANG_ZH_HANS = 0, LANG_ZH_HANT = 1, LANG_EN = 2 };
/* <<< lang-types <<< */
static int g_lang = LANG_EN;

/* 不用 strncasecmp：宿主机那份 mingw 的 string.h 不保证有它，自己写三行更稳 */
static int starts_with_ci(const char *s, const char *pfx)
{
    while (*pfx) {
        char a = *s++;
        char b = *pfx++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

/*
 * 一个语言标记属于哪一档。请求头与环境变量都用它判。
 *
 * 先看主语言是不是 zh，再看地区或文字子标记：
 *   zh / zh-CN / zh-Hans / zh-SG   -> 简体
 *   zh-TW / zh-HK / zh-MO / zh-Hant-> 繁体
 *   其它一律                        -> 英文
 * 光秃秃的 zh 没有信息可判，按简体走（它是中文环境最常见的缺省写法）；
 * zh-XX 里认不出来的地区同理，仍是中文，给简体。
 */
static int lang_of(const char *v)
{
    const char *p = v;

    if (!p)
        return LANG_EN;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!starts_with_ci(p, "zh"))
        return LANG_EN;
    p += 2;
    if (*p == '\0' || *p == ',' || *p == ';' || *p == ' ')
        return LANG_ZH_HANS;
    if (*p != '-' && *p != '_')
        return LANG_EN;                 /* zhuang 之类前缀相同词，不是语言标记 */
    p++;
    if (starts_with_ci(p, "hant") || starts_with_ci(p, "tw") ||
        starts_with_ci(p, "hk") || starts_with_ci(p, "mo"))
        return LANG_ZH_HANT;
    return LANG_ZH_HANS;
}

/* 从整个请求缓冲里读 Accept-Language。必须在截断第一行**之前**调用。 */
static int accept_language_lang(const char *req)
{
    const char *h = strstr(req, "Accept-Language:");
    if (!h)
        h = strstr(req, "accept-language:");
    if (!h)
        return LANG_EN;
    h += 16;
    while (*h == ' ' || *h == '\t')
        h++;
    return lang_of(h);
}

/*
 * 决定本次请求用哪种语言。
 *
 * hdr_lang 由调用方在**截断请求行之前**从完整缓冲里读好（serve_client）——
 * 一旦第一行被 '\0' 截断，strstr 就再也出不了那一行，请求头全部不可见。
 * 这个坑真实踩过：Accept-Language 明明发了 zh-CN，服务端一律当英文。
 */
static void apply_lang(int hdr_lang, const char *query)
{
    const char *q = strstr(query, "lang=");

    if (q) {                            /* 显式指定优先，脚本与 PC 端用它 */
        g_lang = lang_of(q + 5);
        return;
    }
    g_lang = hdr_lang;
}

/*
 * 通知语言（best effort）：按 LC_ALL / LC_MESSAGES / LANG 的顺序找，
 * 但**跳过 C / POSIX 这类"并非语言选择"的值**继续往下找。
 *
 * 为什么要跳过：C.UTF-8 只表示"不做本地化、但用 UTF-8 编码"，它不表达任何
 * 语言偏好。实测 Git Bash 默认就设着 LC_ALL=C.UTF-8；若不跳过，排在后面的
 * LANG=zh_CN.UTF-8 永远读不到——用户明明设了中文，通知却一律英文。
 */
static int env_lang(void)
{
    static const char *names[3] = { "LC_ALL", "LC_MESSAGES", "LANG" };
    int i;

    for (i = 0; i < 3; i++) {
        const char *v = getenv(names[i]);

        if (!v || !*v)
            continue;
        if ((v[0] == 'C' || v[0] == 'c') &&
            (v[1] == '\0' || v[1] == '.' || v[1] == '_'))
            continue;                   /* C / C.UTF-8 / C.utf8 */
        if (starts_with_ci(v, "POSIX"))
            continue;
        return lang_of(v);
    }
    return LANG_EN;
}

/* 当前界面语言在 /version 里自述用的标记 */
static const char *lang_tag(void)
{
    if (g_lang == LANG_ZH_HANT)
        return "zh-Hant";
    return g_lang == LANG_ZH_HANS ? "zh-Hans" : "en";
}

/* 按当前界面语言三选一。三个参数都是字面量，调用方不需要释放。 */
static const char *T(const char *zh, const char *hant, const char *en)
{
    if (g_lang == LANG_EN)
        return en;
    return g_lang == LANG_ZH_HANT ? hant : zh;
}

/* 当前界面语言下的显示名（简体 / 繁体 / 英文） */
static const char *app_name(void)
{
    if (g_lang == LANG_EN)
        return APP_NAME_EN;
    return g_lang == LANG_ZH_HANT ? APP_NAME_HANT : APP_NAME_ZH;
}

/*
 * 占用进程相关的上限。
 *
 * 都刻意取小：一来响应体要能塞进 serve_client 的 out[]（BUFSZ+512），
 * 二来真机上一次能同时打开同一块 U 盘的进程本来就不会多。超出就截断并
 * 明说截断了，不做静默丢弃。
 */
#define MAX_HOLDERS    16      /* 一次最多回报多少个占用者 */
#define MAX_PROCS_OUT  48      /* /procs 最多列出多少条 */
#define MAX_PROCS_SCAN 1024    /* 进程表扫描上限，防御异常返回值 */
#define HOLDER_PATH    384     /* 每个占用者记多长的路径 */

/* 唯一允许卸载的挂载点前缀：/mnt/usb0、/mnt/usb1 ... */
static const char *USB_PREFIX = "/mnt/usb";

/* 主循环据此退出（由本机回环发来的 /shutdown 置位） */
static volatile int g_shutdown = 0;

/*
 * UI 自启的状态。
 *   g_ui_enabled  = 是否在启动时自动拉起界面（--no-ui 可关）
 *   g_ui_tries    = 尝试次数（启动 1 次 + 每次 /openui）
 *   g_*_rc        = 各系统调用的返回码；-1 表示尚未尝试
 * 这些值由 /diag 原样报出——真机上不生效时靠它定位，而不是靠猜。
 */
static int g_ui_enabled = 1;
static int g_ui_tries = 0;
static int g_rc_user_init = -999;
static int g_rc_browser = -999;
static int g_rc_notify = -999;
static int g_rc_knotify = -999;   /* 内核通知 sceKernelSendNotificationRequest */

/*
 * 启动原因，决定通知文案与 /diag 的自述。四档对应四种启动局面：
 *   UI_REASON_START     端口空闲，本次是唯一的实例（正常启动）
 *   UI_REASON_SAME      主机上已有同一版本在跑，本次只是"把界面叫出来"
 *   UI_REASON_BUSY      主机上有实例但不回话（多半卡在一次卸载里），同上处理
 *   UI_REASON_TAKEOVER  主机上跑的是另一个版本，本次接管（新版本真的生效了）
 */
#define UI_REASON_START    0
#define UI_REASON_SAME     1
#define UI_REASON_BUSY     2
#define UI_REASON_TAKEOVER 3
static int g_ui_reason = UI_REASON_START;
static char g_other_ver[32] = "";  /* 对方版本（TAKEOVER 时记的是被接管者） */

/*
 * --force：端口上已有同一版本的实例时，仍然按"接管"处理（请它退出、自己顶上）。
 * 默认策略是"同版本就直接激活"（见头部「单实例」一节），这个开关是逃生口，
 * 也给宿主机测试用——测试要靠它保证每个场景都是新进程、新伪造挂载表。
 */
static int g_force = 0;

/* 本机局域网地址与监听端口。main() 启动时填好，供界面/通知/诊断复用。 */
static char g_ip[64] = "127.0.0.1";
static int g_port = LISTEN_PORT;

/* 定义在 main() 之前，此处前置声明——/openui 路由要用到。 */
static void open_ui(const char *ip, int port, int reason, const char *other_ver);

/*
 * 构建标记。上位机（usbmanage）部署后从 elf 二进制里 grep 这个串，
 * 与主机 /version 返回值比对，用来判定"部署是否真的生效"。
 * 若主机返回的版本与之不符，说明跑的仍是旧实例。
 */
__attribute__((used))
static const char USBMANAGE_BUILD_ID[] = "USBMANAGE_BUILD=" VERSION ";";

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return c - 'A' + 10;
}

/*
 * 就地做百分号解码。
 *
 * 上位机（Python urllib、浏览器 fetch 等）按规范会把查询串里的 '/' 编码成
 * %2F，于是 `/mnt/usb0` 到手就是 `%2Fmnt%2Fusb0`。挂载点必须按字面量匹配，
 * 不解码就会把合法请求误判成非法路径（这是个真实踩过的坑）。
 * '+' 按表单惯例还原为空格。
 */
static void url_decode(char *s)
{
    char *r = s, *w = s;

    while (*r) {
        if (*r == '%' && is_hex(r[1]) && is_hex(r[2])) {
            *w++ = (char)((hex_val(r[1]) << 4) | hex_val(r[2]));
            r += 3;
        } else if (*r == '+') {
            *w++ = ' ';
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/*
 * 取查询串里的 mount= 参数。
 *
 * 刻意**不修改** query：同一个查询串里还有 force=1 / confirm=1 这类开关，
 * 那种"就地截断 '&'"的写法会把后面的开关一起抹掉。
 */
static void extract_mount(const char *query, char *out, size_t cap)
{
    const char *p, *v, *end;
    size_t n;

    if (!out || cap == 0)
        return;
    out[0] = '\0';
    if (!query)
        return;

    p = strstr(query, "mount=");
    if (!p)
        return;
    v = p + 6;
    end = strchr(v, '&');
    n = end ? (size_t)(end - v) : strlen(v);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, v, n);
    out[n] = '\0';

    /* 上位机可能把 '/' 编码成 %2F，先解码再匹配白名单 */
    url_decode(out);
}

/*
 * 仅返回 JSON 的端点。浏览器导航访问它们时统一 302 到选择页 —— 既避免
 * "打开 /list 只看到一坨 JSON"的困惑，也让 /eject、/release 这类**有副
 * 作用**的端点不可能被"地址栏回车"误触发，只能从界面里点。
 */
static int is_json_endpoint(const char *path)
{
    return strcmp(path, "/list") == 0 ||
           strcmp(path, "/eject") == 0 ||
           strcmp(path, "/release") == 0 ||
           strcmp(path, "/holders") == 0 ||
           strcmp(path, "/procs") == 0 ||
           strcmp(path, "/shutdown") == 0;
}

/*
 * 仅接受 /mnt/usb 紧跟一串数字，例如 /mnt/usb0。
 * 显式拒绝 /mnt/ext0、/mnt/ext1（内部存储），以及任何其它路径。
 */
static int path_allowed(const char *p)
{
    size_t n;
    const char *q;

    if (!p || !*p)
        return 0;
    n = strlen(USB_PREFIX);
    if (strncmp(p, USB_PREFIX, n) != 0)
        return 0;
    if (!is_digit(p[n]))
        return 0;
    for (q = p + n; *q; q++) {
        if (!is_digit(*q))
            return 0;
    }
    return 1;
}

/*
 * 枚举挂载卷。
 *   all=0 只取白名单内的（/mnt/usb*）；all=1 取全部（诊断用）。
 * 返回值 = **匹配到的总条数**，写入 out 的条数由 *written 带回。
 * 两者分开是必须的：真机挂载点很多（实测 80+ 条），out 的容量有上限，
 * 若把"写入条数"当总数报出去，就会出现"主机明明有 U 盘、接口却说没有"
 * 或者"少报"的假象。调用方据此给出 shown / truncated。
 */
static int enum_volumes(struct statfs *out, int max, int all, int *written)
{
    struct statfs *mnts;
    int n, i, k = 0, total = 0;

    *written = 0;

    n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0)
        return 0;
    if (n > 4096)               /* 防御异常返回值 */
        n = 4096;

    mnts = (struct statfs *)malloc((size_t)n * sizeof(struct statfs));
    if (!mnts)
        return 0;

    n = getfsstat(mnts, n * (int)sizeof(struct statfs), MNT_NOWAIT);
    if (n <= 0) {
        free(mnts);
        return 0;
    }

    for (i = 0; i < n; i++) {
        if (!all && !path_allowed(mnts[i].f_mntonname))
            continue;
        total++;
        if (k < max)
            out[k++] = mnts[i];
    }

    free(mnts);
    *written = k;
    return total;
}

/* 指定挂载点当前是否真的挂着。卸载前必须过这一关。 */
static int mount_present(const char *mount)
{
    struct statfs *mnts;
    int n, i, found = 0;

    n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0)
        return 0;
    if (n > 4096)
        n = 4096;

    mnts = (struct statfs *)malloc((size_t)n * sizeof(struct statfs));
    if (!mnts)
        return 0;

    n = getfsstat(mnts, n * (int)sizeof(struct statfs), MNT_NOWAIT);
    for (i = 0; i < n && !found; i++) {
        if (strcmp(mnts[i].f_mntonname, mount) == 0)
            found = 1;
    }

    free(mnts);
    return found;
}

static void json_escape(const char *src, char *dst, size_t cap)
{
    size_t j = 0;
    for (; *src && j + 2 < cap; src++) {
        if (*src == '"' || *src == '\\') {
            dst[j++] = '\\';
            dst[j++] = *src;
        } else if ((unsigned char)*src < 0x20) {
            continue;
        } else {
            dst[j++] = *src;
        }
    }
    dst[j] = '\0';
}

/* ============================================================
 * 卷标：从设备的引导扇区与根目录里读出来
 *
 * 为什么要自己解析：内核的 statfs 只给设备名与文件系统名，不给卷标；而
 * "哪块盘是哪个"恰恰是插两块 U 盘时最容易卸错的地方。
 *
 * 只认 exFAT 与 FAT12/16/32 —— PS5 对外接存储只接受这两种（exFAT 就是它
 * 格式化扩展存储时用的）。认不出来就返回空，界面显示「—」，不做猜测；
 * 全程只读，除了引导扇区与根目录，别的地方一个字节都不碰。
 *
 * 分两层：fsboot_parse / utf16le_to_utf8 / *_label_in_dir 是纯函数，
 * 可以在宿主机上拿合成扇区穷举（hosttest/run.sh 第 10 节）；
 * read_volume_label() 才是碰设备的那一层。
 * ============================================================ */

/* >>> fs-label-types >>> */
#define FS_UNKNOWN   0
#define FS_FAT12_16  1
#define FS_FAT32     2
#define FS_EXFAT     3

#define LABEL_MAX      64        /* 界面上显示这么长 */
#define LABEL_SCAN_MAX 65536u    /* 根目录最多读这么多字节 */

struct fsboot {
    int    kind;
    size_t bps;               /* 每扇区字节数 */
    size_t spc;               /* 每簇扇区数 */
    size_t heap_off;          /* exFAT：簇堆起始扇区号 */
    size_t root_clus;         /* exFAT 根目录首簇 / FAT32 根目录首簇 */
    size_t data_off;          /* FAT32：数据区（簇 2）的字节偏移 */
    size_t root_off;          /* FAT12/16：固定根目录区的字节偏移 */
    size_t root_len;          /* FAT12/16：固定根目录区的字节长度 */
    char   bpb_label[11];     /* 引导扇区里的 8.3 卷标（可能为空或无效） */
};
/* <<< fs-label-types <<< */

static size_t rd16(const unsigned char *p)
{
    return (size_t)p[0] | ((size_t)p[1] << 8);
}

static size_t rd32(const unsigned char *p)
{
    return (size_t)p[0] | ((size_t)p[1] << 8) |
           ((size_t)p[2] << 16) | ((size_t)p[3] << 24);
}

/*
 * 解析引导扇区。返回 1 = 认出来了（g 被填好），0 = 不认识。
 *
 * 两道门：① 结束标记 55 AA；② 头三字节是跳转指令（EB/E9）。
 * 内部卷是 ufs/bfs，本来就过不了这两道；这两道同时也是防"把别的
 * 校验通过的结构当 FAT 解"的护栏。
 */
static int fsboot_parse(const unsigned char *s, size_t n, struct fsboot *g)
{
    size_t fatsz, rsvd, nfat, rootent, rootsz;

    memset(g, 0, sizeof *g);
    g->kind = FS_UNKNOWN;

    if (!s || n < 512)
        return 0;
    if (s[0x1FE] != 0x55 || s[0x1FF] != 0xAA)
        return 0;
    if (s[0] != 0xEB && s[0] != 0xE9)
        return 0;

    /* ---- exFAT（Microsoft exFAT 规范 3.1 节的字段偏移）---- */
    if (memcmp(s + 3, "EXFAT   ", 8) == 0) {
        unsigned bshift = s[0x6C], cshift = s[0x6D];

        if (bshift < 9 || bshift > 12)      /* 512 ~ 4096 */
            return 0;
        if (cshift > 25)
            return 0;

        g->kind     = FS_EXFAT;
        g->bps      = (size_t)1 << bshift;
        g->spc      = (size_t)1 << cshift;
        g->heap_off = rd32(s + 0x58);
        g->root_clus = rd32(s + 0x60);
        if (g->root_clus < 2)
            return 0;
        return 1;
    }

    /* ---- FAT12 / FAT16 / FAT32 ---- */
    g->bps  = rd16(s + 0x0B);
    g->spc  = (size_t)s[0x0D];
    rsvd    = rd16(s + 0x0E);
    nfat    = (size_t)s[0x10];
    rootent = rd16(s + 0x11);

    if (g->bps != 512 && g->bps != 1024 && g->bps != 2048 && g->bps != 4096)
        return 0;
    if (g->spc == 0 || (g->spc & (g->spc - 1)) != 0)   /* 必须是 2 的幂 */
        return 0;
    if (rsvd == 0 || nfat == 0 || nfat > 4)
        return 0;

    if (rd16(s + 0x16) != 0) {
        fatsz = rd16(s + 0x16);
        g->kind = FS_FAT12_16;
    } else {
        fatsz = rd32(s + 0x24);
        if (fatsz == 0 || rootent != 0)
            return 0;
        g->kind = FS_FAT32;
    }
    if (fatsz == 0 || fatsz > (size_t)0x100000)        /* 防御畸形 FAT 长度 */
        return 0;

    memcpy(g->bpb_label, s + (g->kind == FS_FAT32 ? 0x47 : 0x2B), 11);

    rootsz = (rootent * 32 + g->bps - 1) / g->bps;      /* 根目录占几个扇区 */

    if (g->kind == FS_FAT32) {
        g->root_clus = rd32(s + 0x2C);
        if (g->root_clus < 2)
            return 0;
        g->data_off = (rsvd + nfat * fatsz + rootsz) * g->bps;
    } else {
        g->root_off = (rsvd + nfat * fatsz) * g->bps;
        g->root_len = rootent * 32;
        if (g->root_len == 0)
            return 0;
    }
    return 1;
}

/*
 * UTF-16LE -> UTF-8（BMP 加代理对）。控制字符丢掉，0x0000/0xFFFF 视为结束。
 * 返回写入字节数。cap 不足时截断，绝不越界。
 */
static int utf16le_to_utf8(const unsigned char *p, int uchars, char *out, size_t cap)
{
    int i, k = 0;

    if (!out || cap == 0)
        return 0;
    out[0] = '\0';

    for (i = 0; i < uchars; i++) {
        unsigned long cp = (unsigned long)p[i * 2] | ((unsigned long)p[i * 2 + 1] << 8);

        if (cp == 0 || cp == 0xFFFF)
            break;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < uchars) {
            unsigned long lo = (unsigned long)p[(i + 1) * 2] |
                               ((unsigned long)p[(i + 1) * 2 + 1] << 8);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000UL + ((cp - 0xD800UL) << 10) + (lo - 0xDC00UL);
                i++;
            }
        }
        if (cp < 0x20 || cp == 0x7F)
            continue;
        if (cp < 0x80) {
            if (k + 1 >= (int)cap) break;
            out[k++] = (char)cp;
        } else if (cp < 0x800) {
            if (k + 2 >= (int)cap) break;
            out[k++] = (char)(0xC0 | (cp >> 6));
            out[k++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            if (k + 3 >= (int)cap) break;
            out[k++] = (char)(0xE0 | (cp >> 12));
            out[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[k++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (k + 4 >= (int)cap) break;
            out[k++] = (char)(0xF0 | (cp >> 18));
            out[k++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[k++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[k] = '\0';
    return k;
}

/*
 * 8.3 形式的 11 字节卷标 -> 可显示字符串。
 *
 * 只接受纯 ASCII：这段字段是 OEM 代码页，中文标签在这里是 GBK 字节，
 * 原样输出就是乱码。非 ASCII 时返回 0，交给调用方去根目录里找 LFN。
 * "NO NAME" 是 Windows 的"没有卷标"占位写法，同样按没有处理。
 *
 * 契约：raw 至少有 11 字节（FAT 规范里这个字段就是定长 11，调用点都在
 * 结构体/定长缓冲里，不存在读越界的可能）。
 */
static int label_from_83(const unsigned char *raw, char *out, size_t cap)
{
    int i, k = 0;

    if (!raw || !out || cap == 0)
        return 0;
    out[0] = '\0';

    for (i = 0; i < 11; i++) {
        if (raw[i] == 0)
            break;
        if (k + 1 >= (int)cap)
            break;
        out[k++] = (char)raw[i];
    }
    while (k > 0 && out[k - 1] == ' ')      /* 尾部空格是填充 */
        k--;
    out[k] = '\0';
    if (k == 0)
        return 0;

    for (i = 0; i < k; i++) {
        unsigned char c = (unsigned char)out[i];
        if (c < 0x20 || c > 0x7E)
            return 0;
    }
    if (k == 7 && (strncmp(out, "NO NAME", 7) == 0 || strncmp(out, "no name", 7) == 0))
        return 0;
    return 1;
}

/*
 * exFAT 根目录里的卷标项：EntryType 0x83（bit7 = 在用），
 * 第 2 字节是字符数（≤11），第 3 字节起是 UTF-16LE 的名字。
 */
static int exfat_label_in_dir(const unsigned char *d, size_t n, char *out, size_t cap)
{
    size_t i;

    if (!out || cap == 0)
        return 0;
    out[0] = '\0';

    for (i = 0; i + 32 <= n; i += 32) {
        unsigned t = d[i];

        if (t == 0x00)                  /* 目录项结束 */
            break;
        if (t != 0x83)                  /* 只认在用的卷标项（0x03 是已删除） */
            continue;
        if (d[i + 1] == 0 || d[i + 1] > 11)
            return 0;
        utf16le_to_utf8(d + i + 2, (int)d[i + 1], out, cap);
        return out[0] ? 1 : 0;
    }
    return 0;
}

/*
 * FAT 根目录里的卷标。
 *
 * 中文卷标在 FAT 上是"长文件名项 + 8.3 短项"成对存在的：短项的 8.3 字段
 * 放不下，真正的名字在前面的 0x0F 长名项里（UTF-16）。所以这里先按长名
 * 项把名字拼回来，碰到属性带 0x08 的短项时收网；拼不出来再退回 8.3 字段。
 *
 * 长名项的物理顺序是"名字末尾在前"（order 递减），所以按 order 算字符位置：
 * 第 order 项的内容落在 (order-1)*13 处。
 */
static int fat_label_in_dir(const unsigned char *d, size_t n, char *out, size_t cap)
{
    static const int off[3] = {1, 14, 28};    /* 三个 UTF-16 段的起始偏移 */
    static const int cnt[3] = {5, 6, 2};      /* 每段的码元数 */
    unsigned char u16[13 * 20 * 2];           /* 20 个长名槽 */
    int nunit = 0, t, j;
    size_t i;

    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    memset(u16, 0, sizeof u16);

    for (i = 0; i + 32 <= n; i += 32) {
        unsigned attr = d[i + 11];
        unsigned first = d[i];

        if (first == 0x00)                  /* 目录项结束 */
            break;
        if (first == 0xE5) {                /* 已删除：丢弃前面攒的长名 */
            memset(u16, 0, sizeof u16);
            nunit = 0;
            continue;
        }

        if (attr == 0x0F) {                 /* 长名项 */
            unsigned order = first & 0x3F;
            if (order < 1 || order > 20)
                continue;
            for (t = 0; t < 3; t++) {
                int base = (order - 1) * 13 + (t == 0 ? 0 : (t == 1 ? 5 : 11));
                for (j = 0; j < cnt[t]; j++) {
                    u16[(base + j) * 2]     = d[i + off[t] + j * 2];
                    u16[(base + j) * 2 + 1] = d[i + off[t] + j * 2 + 1];
                }
            }
            if ((int)(order * 13) > nunit)
                nunit = (int)(order * 13);
            continue;
        }

        if (attr & 0x08) {                  /* 属性带 0x08 = 卷标短项 */
            if (nunit > 0 && utf16le_to_utf8(u16, nunit, out, cap) > 0)
                return 1;
            return label_from_83(d + i, out, cap);
        }
    }
    return 0;
}

/*
 * 读一块外接盘的卷标。dev 取自 statfs 的 f_mntfromname（如 /dev/da0s1）。
 *
 * 用 FILE* 而不是 open/read：宿主机编译时 close() 被替换成 closesocket()
 * （socket 与 CRT 文件描述符不同域），走 fopen/fclose 就不必与那处转换纠缠。
 *
 * 设备不在、没权限、不是 FAT 系 -> 返回 0，界面显示「—」。服务不因此报错：
 * 卷标只是"帮人别卸错盘"的信息，不是卸载能否成功的前提。
 */
static int read_volume_label(const char *dev, char *out, size_t cap)
{
    unsigned char s0[512];
    unsigned char *buf;
    struct fsboot g;
    FILE *fp;
    size_t off = 0, len = 0, got = 0;
    int ok = 0;

    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!dev || !*dev)
        return 0;

    fp = fopen(dev, "rb");
    if (!fp)
        return 0;
    got = fread(s0, 1, sizeof s0, fp);
    if (got != sizeof s0 || !fsboot_parse(s0, sizeof s0, &g)) {
        fclose(fp);
        return 0;
    }

    if (g.kind == FS_EXFAT) {
        off = (g.heap_off + (g.root_clus - 2) * g.spc) * g.bps;
        len = g.spc * g.bps;
    } else if (g.kind == FS_FAT32) {
        off = g.data_off + (g.root_clus - 2) * g.spc * g.bps;
        len = g.spc * g.bps;
    } else {
        off = g.root_off;
        len = g.root_len;
    }
    if (len == 0)
        len = 1;
    if (len > LABEL_SCAN_MAX)
        len = LABEL_SCAN_MAX;

    /*
     * 32 位 long 的宿主（Windows）上 fseek 只认到 2GB；PS5 上 long 是 64 位，
     * 不受限。真机上这些偏移都很小（根目录紧跟在 FAT 之后），此处只为不越界。
     */
    if (sizeof(long) <= 4 && off > (size_t)0x7FFFFFFFUL) {
        fclose(fp);
        return 0;
    }

    buf = (unsigned char *)malloc(len);
    if (!buf) {
        fclose(fp);
        return 0;
    }

    if (fseek(fp, (long)off, SEEK_SET) == 0)
        got = fread(buf, 1, len, fp);
    else
        got = 0;

    if (got >= 32) {
        if (g.kind == FS_EXFAT)
            ok = exfat_label_in_dir(buf, got, out, cap);
        else
            ok = fat_label_in_dir(buf, got, out, cap);
    }
    free(buf);
    fclose(fp);

    if (!ok)
        ok = label_from_83((const unsigned char *)g.bpb_label, out, cap);
    if (!ok)
        out[0] = '\0';
    return ok;
}

/* ============================================================
 * 占用进程：发现与解除
 * ============================================================ */

/* 一个"占用者"：某进程打开了该挂载点下的某个东西 */
struct holder {
    int  pid;
    char comm[64];              /* 进程名（ki_comm，真机上限 COMMLEN=19 字节） */
    int  fd;                    /* kf_fd；负值是 KF_FD_TYPE_* 特殊项 */
    char path[HOLDER_PATH];     /* 打开的具体路径 */
    int  is_self;               /* 是不是本进程自己 */
};

/*
 * 这些缓冲放静态区而不是栈上。
 * serve_client 一帧里已经有 req/body/out 三个各 8K 的栈缓冲，再往里堆
 * 十几个几百字节的条目容易把栈顶爆掉；服务是单线程的，静态缓冲安全。
 */
static struct holder g_holders_a[MAX_HOLDERS];
static struct holder g_holders_b[MAX_HOLDERS];

/*
 * 剩余可用空间。
 *
 * off == 0 必须返回 cap，不能返回 0 —— snprintf 拿到 0 会什么都不写（也不
 * 写结尾的 '\0'），而 off 照旧按"本该写入的长度"累加，结果是真正的输出被
 * 写到中途、前缀留着上一次请求的残留。这个坑踩过一次，测试场景 G 守着它。
 * off 越过 cap 时（snprintf 的返回值可以大于 cap）必须夹到 0，否则相减回绕。
 */
static size_t room(size_t cap, int off)
{
    if (off < 0 || (size_t)off >= cap)
        return 0;
    return cap - (size_t)off;
}

/* kf_fd 的可读类别 */
static const char *fd_kind(int fd)
{
    switch (fd) {
    case KF_FD_TYPE_CWD:   return "cwd";     /* 工作目录落在这个盘上 */
    case KF_FD_TYPE_ROOT:  return "root";
    case KF_FD_TYPE_JAIL:  return "jail";
    case KF_FD_TYPE_TRACE: return "trace";
    case KF_FD_TYPE_TEXT:  return "text";    /* 可执行体本身就在这个盘上 */
    case KF_FD_TYPE_CTTY:  return "ctty";
    default:               return fd >= 0 ? "fd" : "other";
    }
}

/*
 * path 是否落在 mount 之下。
 * 必须比到"边界"上：/mnt/usb00 不能被当成 /mnt/usb0 的子路径，
 * 所以要求下一个字符是 '\0' 或 '/'。
 */
static int path_under(const char *path, const char *mount)
{
    size_t n;

    if (!path || !mount || !*path || !*mount)
        return 0;
    n = strlen(mount);
    if (strncmp(path, mount, n) != 0)
        return 0;
    return path[n] == '\0' || path[n] == '/';
}

/*
 * 取进程表。
 * 与 SDK 官方 samples/ps/main.c 同一套 mib，也同一套遍历方式（按
 * ki_structsize 步进，而非假设定长）。真机行为未知的环节里，能对齐官方
 * 样例的地方就对齐。
 * 返回 malloc 的缓冲（字节长度写入 *lenp），失败返回 NULL 并置 errno。
 */
static struct kinfo_proc *proc_table(size_t *lenp)
{
    int mib[4];
    size_t len = 0;
    struct kinfo_proc *buf;

    *lenp = 0;

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PROC;
    mib[3] = 0;

    if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0 || len == 0)
        return NULL;

    /* 防御异常返回值：真机上百来个进程，给足余量但别无限 */
    if (len > (size_t)MAX_PROCS_SCAN * sizeof(struct kinfo_proc))
        len = (size_t)MAX_PROCS_SCAN * sizeof(struct kinfo_proc);

    buf = (struct kinfo_proc *)malloc(len);
    if (!buf)
        return NULL;

    if (sysctl(mib, 4, buf, &len, NULL, 0) != 0) {
        free(buf);
        return NULL;
    }
    *lenp = len;
    return buf;
}

/*
 * 取某进程的文件描述符表。
 * 做法与 FreeBSD libutil 的 kinfo_getfile() 完全一致——SDK 只给了它的
 * 声明、没给实现（包里没有 libutil），所以按 libutil 的做法自己封一层：
 * 先问长度，再取内容。
 */
static struct kinfo_file *file_table(int pid, size_t *lenp)
{
    int mib[4];
    size_t len = 0;
    struct kinfo_file *buf;

    *lenp = 0;

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_FILEDESC;
    mib[3] = pid;

    if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0 || len == 0)
        return NULL;

    buf = (struct kinfo_file *)malloc(len);
    if (!buf)
        return NULL;

    if (sysctl(mib, 4, buf, &len, NULL, 0) != 0) {
        free(buf);
        return NULL;
    }
    *lenp = len;
    return buf;
}

/*
 * 找出所有"打开了该挂载点下面某个文件"的进程。
 *
 * 返回值 = 写入 out 的条数。
 *   *scanned  = 实际扫过的进程数
 *   *readable = 其中 fd 表**成功读到**的进程数
 *   *err      = 底层失败原因（0 表示没出错）
 *
 * readable 单独回报是必须的：内核若不放行 KERN_PROC_FILEDESC，每个进程的
 * fd 表都读不到，条数自然为 0 —— 那看起来像"没人占用"，真相却是"根本查
 * 不了"。调用方据此给出 supported 字段，不含糊过去。
 */
static int find_holders(const char *mount, struct holder *out, int max,
                        int *scanned, int *readable, int *err)
{
    struct kinfo_proc *procs;
    size_t plen = 0;
    char *ptr, *pend;
    int k = 0, full = 0, self;

    *scanned = 0;
    *readable = 0;
    *err = 0;

    self = (int)getpid();

    procs = proc_table(&plen);
    if (!procs) {
        *err = errno ? errno : EPERM;
        return 0;
    }

    pend = (char *)procs + plen;
    for (ptr = (char *)procs; ptr < pend && !full; ) {
        struct kinfo_proc *ki = (struct kinfo_proc *)ptr;
        struct kinfo_file *fds;
        size_t flen = 0;
        char *fp, *fend;
        int pid;

        /* 官方样例的遍历方式：按记录自报的长度步进 */
        if (ki->ki_structsize <= 0)
            break;
        ptr += ki->ki_structsize;

        pid = (int)ki->ki_pid;
        if (pid <= 0)
            continue;

        (*scanned)++;

        fds = file_table(pid, &flen);
        if (!fds)
            continue;                   /* 读不到：不计入 readable，继续下一个 */
        (*readable)++;

        fend = (char *)fds + flen;
        for (fp = (char *)fds; fp < fend; ) {
            struct kinfo_file *kf = (struct kinfo_file *)fp;

            if (kf->kf_structsize <= 0)
                break;
            fp += kf->kf_structsize;

            if (kf->kf_path[0] == '\0')
                continue;
            if (!path_under(kf->kf_path, mount))
                continue;

            if (k >= max) {             /* 满了：截断，由调用方如实报出 */
                full = 1;
                break;
            }
            out[k].pid = pid;
            snprintf(out[k].comm, sizeof out[k].comm, "%s", ki->ki_comm);
            out[k].fd = kf->kf_fd;
            snprintf(out[k].path, sizeof out[k].path, "%s", kf->kf_path);
            out[k].is_self = (pid == self);
            k++;
        }
        free(fds);
    }

    free(procs);
    return k;
}

/*
 * 该占用者能不能被我们终止。
 *
 * 走到这一步的前提是"它确实打开了该挂载点下的文件"，所以判据本身是收紧的；
 * 但即便如此仍保留三道不杀的护栏。宁可留一个解除不掉的让用户自己处理，
 * 也不要误杀系统进程——主机上少一条关键进程，可能就意味着整机要重启。
 */
static int holder_killable(const struct holder *h)
{
    if (h->is_self)
        return 0;                       /* 自杀等于把服务一起带走 */
    if (h->pid <= 1)
        return 0;                       /* init / 内核 */
    if (strncmp(h->comm, "Sce", 3) == 0)
        return 0;                       /* Sony 系统组件（SceShellCore 等） */
    if (strcmp(h->comm, "init") == 0 || strcmp(h->comm, "kernel") == 0)
        return 0;
    return 1;
}

/* 不能终止的原因，给人看的一句话 */
static const char *holder_skip_reason(const struct holder *h)
{
    if (h->is_self)
        return T("本服务自身（终止它等于终止服务）",
                 "本服務自身（終止它等於終止服務）",
                 "this service itself (killing it kills the service)");
    if (h->pid <= 1)
        return T("系统关键进程（pid 1 或内核）",
                 "系統關鍵處理程序（pid 1 或核心）",
                 "critical system process (pid 1 or kernel)");
    if (strncmp(h->comm, "Sce", 3) == 0)
        return T("Sony 系统组件（Sce* 前缀，不代为终止）",
                 "Sony 系統元件（Sce* 前綴，不代為終止）",
                 "Sony system component (Sce* prefix, never killed)");
    if (strcmp(h->comm, "init") == 0 || strcmp(h->comm, "kernel") == 0)
        return T("系统关键进程", "系統關鍵處理程序", "critical system process");
    return T("不可终止", "不可終止", "not killable");
}

/* 把一组占用者写成 JSON 数组，追加到 resp 的 *off 处 */
static void append_holders(char *resp, size_t cap, int *off,
                           const struct holder *hs, int n)
{
    int i, o = *off;

    o += snprintf(resp + o, room(cap, o), "[");
    for (i = 0; i < n; i++) {
        char comm[160], path[512];
        int killable = holder_killable(&hs[i]);

        json_escape(hs[i].comm, comm, sizeof comm);
        json_escape(hs[i].path, path, sizeof path);
        o += snprintf(resp + o, room(cap, o),
                      "%s{\"pid\":%d,\"comm\":\"%s\",\"fd\":%d,\"kind\":\"%s\","
                      "\"path\":\"%s\",\"self\":%s,\"killable\":%s",
                      i ? "," : "", hs[i].pid, comm, hs[i].fd,
                      fd_kind(hs[i].fd), path,
                      hs[i].is_self ? "true" : "false",
                      killable ? "true" : "false");
        if (!killable) {
            char why[192];
            json_escape(holder_skip_reason(&hs[i]), why, sizeof why);
            o += snprintf(resp + o, room(cap, o), ",\"why\":\"%s\"", why);
        }
        o += snprintf(resp + o, room(cap, o), "}");
    }
    o += snprintf(resp + o, room(cap, o), "]");
    *off = o;
}

/* ---- /holders ---- */
static int handle_holders(const char *mount, char *resp, size_t cap)
{
    char mnt[512];
    int n, scanned = 0, readable = 0, err = 0, off = 0;

    if (!mount || !*mount) {
        snprintf(resp, cap, "{\"ok\":false,\"msg\":\"missing 'mount' parameter\"}");
        return (int)strlen(resp);
    }
    if (!path_allowed(mount)) {
        json_escape(mount, mnt, sizeof mnt);
        snprintf(resp, cap, "{\"ok\":false,\"msg\":\"path not allowed: %s\"}", mnt);
        return (int)strlen(resp);
    }

    n = find_holders(mount, g_holders_a, MAX_HOLDERS, &scanned, &readable, &err);

    if (err == 0 && scanned > 0 && readable == 0)
        err = EPERM;

    json_escape(mount, mnt, sizeof mnt);
    off += snprintf(resp + off, room(cap, off),
                    "{\"ok\":true,\"mount\":\"%s\",\"count\":%d,"
                    "\"scanned\":%d,\"readable\":%d,\"self_pid\":%d,"
                    "\"supported\":%s",
                    mnt, n, scanned, readable, (int)getpid(),
                    err ? "false" : "true");
    if (err)
        off += snprintf(resp + off, room(cap, off),
                        ",\"err\":%d,\"errmsg\":\"%s\"", err, strerror(err));
    off += snprintf(resp + off, room(cap, off), ",\"truncated\":%s",
                    n >= MAX_HOLDERS ? "true" : "false");
    off += snprintf(resp + off, room(cap, off), ",\"holders\":");
    append_holders(resp, cap, &off, g_holders_a, n);
    off += snprintf(resp + off, room(cap, off), "}");
    return off;
}

/* ---- /procs：进程清单（诊断用）---- */
static int handle_procs(char *resp, size_t cap)
{
    size_t plen = 0;
    struct kinfo_proc *procs;
    char *ptr, *pend;
    int total = 0, shown = 0, off = 0, self = (int)getpid();

    procs = proc_table(&plen);
    if (!procs) {
        int e = errno ? errno : EPERM;
        snprintf(resp, cap,
                 "{\"ok\":false,\"supported\":false,\"err\":%d,"
                 "\"msg\":\"sysctl(KERN_PROC_PROC) failed: %s\"}", e, strerror(e));
        return (int)strlen(resp);
    }

    off += snprintf(resp + off, room(cap, off),
                    "{\"ok\":true,\"supported\":true,\"self_pid\":%d,\"procs\":[", self);

    pend = (char *)procs + plen;
    for (ptr = (char *)procs; ptr < pend; ) {
        struct kinfo_proc *ki = (struct kinfo_proc *)ptr;
        char comm[160];

        if (ki->ki_structsize <= 0)
            break;
        ptr += ki->ki_structsize;

        if (ki->ki_pid <= 0)
            continue;
        total++;
        if (shown >= MAX_PROCS_OUT)
            continue;                   /* 继续数总数，只是不再写出来 */

        json_escape(ki->ki_comm, comm, sizeof comm);
        off += snprintf(resp + off, room(cap, off),
                        "%s{\"pid\":%d,\"ppid\":%d,\"uid\":%u,\"comm\":\"%s\",\"self\":%s}",
                        shown ? "," : "", (int)ki->ki_pid, (int)ki->ki_ppid,
                        (unsigned)ki->ki_uid, comm,
                        ((int)ki->ki_pid == self) ? "true" : "false");
        shown++;
    }

    off += snprintf(resp + off, room(cap, off),
                    "],\"count\":%d,\"shown\":%d,\"truncated\":%s}",
                    total, shown, shown < total ? "true" : "false");

    free(procs);
    return off;
}

/* 把一次终止动作追加进 killed 数组 */
static void append_one_kill(char *resp, size_t cap, int *off,
                            const struct holder *h, const char *sig, int first)
{
    char comm[160];

    json_escape(h->comm, comm, sizeof comm);
    *off += snprintf(resp + *off, room(cap, *off),
                     "%s{\"pid\":%d,\"comm\":\"%s\",\"sig\":\"%s\"}",
                     first ? "" : ",", h->pid, comm, sig);
}

/*
 * 解除"内核侧挂接"：把源是这块 U 盘的 nullfs 视图先卸掉。
 *
 * PS5 的应用沙箱会把外部卷以 nullfs 挂进 /mnt/sandbox/<TITLEID>_000/... ——
 * 真机 /list?all=1 里那一大票 nullfs 就是它们。这种引用**不体现为任何
 * 进程的 fd**（find_holders 查不到人），unmount 目标卷时却就是 EBUSY。
 * 对策是 FreeBSD 的标准语义：先卸掉引用者，目标才卸得掉。
 *
 * 护栏（缺一不碰）：
 *   - fstypename 必须是 nullfs（其它类型一律不动）；
 *   - 源必须恰好是 mount，或位于 mount 之内（"挂的是这块盘里的东西"）；
 *   - 目标不能是 mount 自己。
 * 常规 unmount 失败再用 MNT_FORCE 重试一次。结果逐条写进回执，成败都可见。
 */
static void release_child_mounts(const char *mount, char *resp, size_t cap, int *off,
                                 int *released)
{
    struct statfs mnts[MAX_VOLUMES];
    int n, i, mlen = (int)strlen(mount);

    *released = 0;
    n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0 || n > MAX_VOLUMES)
        n = (n > MAX_VOLUMES) ? MAX_VOLUMES : n;
    if (n > 0 &&
        getfsstat(mnts, n * (int)sizeof(struct statfs), MNT_NOWAIT) <= 0)
        return;

    for (i = 0; i < n; i++) {
        const char *ty = mnts[i].f_fstypename;
        const char *fr = mnts[i].f_mntfromname;
        const char *to = mnts[i].f_mntonname;
        char toj[512];
        int rc, e;

        if (strncmp(ty, "nullfs", sizeof mnts[i].f_fstypename) != 0)
            continue;
        if (strcmp(fr, mount) != 0 &&
            !(strncmp(fr, mount, (size_t)mlen) == 0 && fr[mlen] == '/'))
            continue;                       /* 源不是这块盘 */
        if (strcmp(to, mount) == 0)
            continue;                       /* 不是自己 */

        rc = unmount(to, 0);
        if (rc != 0) {
            e = errno;
            rc = unmount(to, MNT_FORCE);
        }
        e = (rc == 0) ? 0 : errno;
        json_escape(to, toj, sizeof toj);
        *off += snprintf(resp + *off, room(cap, *off),
                         "%s{\"mount\":\"%s\",\"ok\":%s,\"err\":%d}",
                         *released ? "," : "", toj,
                         rc == 0 ? "true" : "false", e);
        if (rc == 0)
            (*released)++;
    }
}

/* ---- /release：解除占用，然后重试卸载 ---- */
static int handle_release(const char *mount, const char *query,
                          char *resp, size_t cap)
{
    char mnt[512];
    int before, after, scanned = 0, readable = 0, err = 0, off = 0;
    int i, killed = 0, skipped = 0, confirmed, force, do_unmount, rc;
    int child_rel = 0;

    if (!mount || !*mount) {
        snprintf(resp, cap, "{\"ok\":false,\"msg\":\"missing 'mount' parameter\"}");
        return (int)strlen(resp);
    }
    if (!path_allowed(mount)) {
        json_escape(mount, mnt, sizeof mnt);
        snprintf(resp, cap, "{\"ok\":false,\"msg\":\"path not allowed: %s\"}", mnt);
        return (int)strlen(resp);
    }
    if (!mount_present(mount)) {
        json_escape(mount, mnt, sizeof mnt);
        snprintf(resp, cap,
                 "{\"ok\":false,\"msg\":\"not mounted: %s%s\"}", mnt,
                 T("（该挂载点当前不存在）", "（該掛載點目前不存在）", " (that mount point does not exist)"));
        return (int)strlen(resp);
    }

    /*
     * 必须显式 confirm=1 才真动手。
     * 不给默认动作的理由很直接：这个端点会**终止别的进程**，不该被
     * "地址栏回车"或者随便一个爬虫误触发。界面里点的那个按钮会带上它。
     */
    confirmed = (strstr(query, "confirm=1") != NULL);
    force = (strstr(query, "force=1") != NULL);
    do_unmount = (strstr(query, "nounmount=1") == NULL);

    before = find_holders(mount, g_holders_a, MAX_HOLDERS, &scanned, &readable, &err);
    if (err == 0 && scanned > 0 && readable == 0)
        err = EPERM;

    json_escape(mount, mnt, sizeof mnt);
    off += snprintf(resp + off, room(cap, off),
                    "{\"ok\":true,\"mount\":\"%s\",\"confirmed\":%s,"
                    "\"supported\":%s,\"before\":%d,\"scanned\":%d",
                    mnt, confirmed ? "true" : "false",
                    err ? "false" : "true", before, scanned);
    if (err)
        off += snprintf(resp + off, room(cap, off),
                        ",\"err\":%d,\"errmsg\":\"%s\"", err, strerror(err));

    off += snprintf(resp + off, room(cap, off), ",\"holders\":");
    append_holders(resp, cap, &off, g_holders_a, before);

    if (!confirmed) {
        off += snprintf(resp + off, room(cap, off),
                        ",\"killed\":[],\"after\":%d,\"unmounted\":false,"
                        "\"msg\":\"%s\"}", before,
                        T("预览：未执行任何终止动作（需要 confirm=1）",
                          "預覽：未執行任何終止動作（需要 confirm=1）",
                          "preview: nothing was terminated (confirm=1 required)"));
        return off;
    }

    /*
     * killed 数组要跨两个阶段：先 TERM，等一拍再对残留者 KILL。
     * 所以这里就把 '[' 写上，两阶段都往里追加，最后才收尾——否则 KILL
     * 那一轮的动作只会计数、不出现在回执里。
     */
    off += snprintf(resp + off, room(cap, off), ",\"killed\":[");

    if (!err) {
        /* 第一阶段：SIGTERM。给它机会自己收尾（关文件、写回播放进度） */
        for (i = 0; i < before; i++) {
            if (!holder_killable(&g_holders_a[i])) {
                skipped++;
                continue;
            }
            if (kill(g_holders_a[i].pid, SIG_TERM) == 0) {
                append_one_kill(resp, cap, &off, &g_holders_a[i], "TERM", killed == 0);
                killed++;
            }
        }
        if (killed > 0)
            usleep(400000);                 /* 给内核一点回收时间 */

        /* 第二阶段：SIGKILL，只针对 TERM 之后仍然占用的 */
        after = find_holders(mount, g_holders_b, MAX_HOLDERS, &scanned, &readable, &err);
        for (i = 0; i < after; i++) {
            if (!holder_killable(&g_holders_b[i]))
                continue;
            if (kill(g_holders_b[i].pid, SIG_KILL) == 0) {
                append_one_kill(resp, cap, &off, &g_holders_b[i], "KILL", killed == 0);
                killed++;
            }
        }
        if (killed > 0)
            usleep(300000);

        /* 复扫一遍拿"仍在占用"名单 */
        after = find_holders(mount, g_holders_b, MAX_HOLDERS, &scanned, &readable, &err);
    } else {
        /*
         * 内核不放行 KERN_PROC_FILEDESC——谁占着查不到，就不硬杀。
         * 但**不能就此放弃**：占用也可能是沙箱 nullfs 视图这类"无主"引用，
         * 下面的子挂载解除 + 强制卸载对它们照样有效。
         */
        after = 0;
    }

    off += snprintf(resp + off, room(cap, off), "]");

    off += snprintf(resp + off, room(cap, off),
                    ",\"skipped\":%d,\"after\":%d,\"still\":", skipped, after);
    append_holders(resp, cap, &off, g_holders_b, after);

    /*
     * 内核侧挂接：先于 unmount 执行。即使一个进程都没杀，也可能就差这一步
     * ——真机上"明明没人用却 Device busy"，多半就是沙箱 nullfs 视图没卸。
     */
    off += snprintf(resp + off, room(cap, off), ",\"child_released\":[");
    release_child_mounts(mount, resp, cap, &off, &child_rel);
    off += snprintf(resp + off, room(cap, off), "],\"child_released_count\":%d",
                    child_rel);

    if (!do_unmount) {
        off += snprintf(resp + off, room(cap, off),
                        ",\"unmounted\":false,\"msg\":\"%s\"}",
                        T("已解除占用（未尝试卸载）",
                          "已解除佔用（未嘗試卸載）",
                          "holders released (unmount was not attempted)"));
        return off;
    }

    /* 解除之后立刻重试卸载，省得用户再点一次 */
    sync();
    errno = 0;
    rc = unmount(mount, force ? MNT_FORCE : 0);
    if (rc != 0) {
        /*
         * "解除占用并卸载"的语义就是把盘卸下来：常规卸不掉（EBUSY 等），
         * 自动补一发 MNT_FORCE 再试。结果照实回报，由界面转述给用户。
         */
        sync();
        errno = 0;
        off += snprintf(resp + off, room(cap, off), ",\"forced_retry\":true");
        rc = unmount(mount, MNT_FORCE);
    }
    if (rc != 0) {
        int e = errno;
        sync();
        off += snprintf(resp + off, room(cap, off),
                        ",\"unmounted\":false,\"code\":%d,"
                        "\"msg\":\"%s%s%s\"}", e,
                        T("解除占用后仍无法卸载：", "解除佔用後仍無法卸載：", "still cannot unmount after releasing: "),
                        strerror(e),
                        after > 0 ? T("（仍有进程占用，见 still）",
                                      "（仍有處理程序佔用，見 still）",
                                      " (some processes still hold it, see 'still')") : "");
        return off;
    }

    sync();
    off += snprintf(resp + off, room(cap, off),
                    ",\"unmounted\":true,\"msg\":\"%s%s%s%s\"}",
                    T("已解除占用并卸载", "已解除佔用並卸載", "released and unmounted"),
                    killed > 0 ? T("（已终止占用进程）", "（已終止佔用處理程序）", " (holder processes terminated)") : "",
                    child_rel > 0 ? T("（已解除系统内部挂接）", "（已解除系統內部掛接）", " (system mounts released)") : "",
                    T("，现在可以安全拔出了", "，現在可以安全拔出了", " — it is now safe to unplug"));
    return off;
}

static int handle_list(char *resp, size_t cap, int all)
{
    struct statfs vols[MAX_VOLUMES];
    int total, avail, i, off = 0, shown = 0;
    char mnt[512], dev[512], fst[256];
    char lab[LABEL_MAX], labesc[LABEL_MAX * 2];

    /* total = 真实匹配总数；avail = 真正拿到手的条数（受 MAX_VOLUMES 限制） */
    total = enum_volumes(vols, MAX_VOLUMES, all, &avail);

    off += snprintf(resp + off, cap - (size_t)off,
                    "{\"count\":%d,\"scope\":\"%s\",\"volumes\":[",
                    total, all ? "all" : "usb");
    for (i = 0; i < avail && off < (int)cap - 512; i++) {
        /*
         * 卷标只对外接盘读：诊断视图里那一堆 /system、/user、沙箱挂接
         * 既不是外接盘也没有卷标，去读它们的设备既没意义又多一份风险。
         */
        lab[0] = '\0';
        if (path_allowed(vols[i].f_mntonname))
            read_volume_label(vols[i].f_mntfromname, lab, sizeof lab);

        json_escape(vols[i].f_mntonname, mnt, sizeof mnt);
        json_escape(vols[i].f_mntfromname, dev, sizeof dev);
        json_escape(vols[i].f_fstypename, fst, sizeof fst);
        json_escape(lab, labesc, sizeof labesc);
        off += snprintf(resp + off, cap - (size_t)off,
                        "%s{\"mount\":\"%s\",\"device\":\"%s\",\"fstype\":\"%s\","
                        "\"ejectable\":%s,\"label\":\"%s\",",
                        i ? "," : "", mnt, dev, fst,
                        path_allowed(vols[i].f_mntonname) ? "true" : "false",
                        labesc);
        off += snprintf(resp + off, cap - (size_t)off,
                        "\"bsize\":%u,\"blocks\":%llu,\"bfree\":%llu,"
                        "\"total\":%llu,\"free\":%llu}",
                        (unsigned)vols[i].f_bsize,
                        (unsigned long long)vols[i].f_blocks,
                        (unsigned long long)vols[i].f_bfree,
                        (unsigned long long)vols[i].f_blocks *
                            (unsigned long long)vols[i].f_bsize,
                        (unsigned long long)vols[i].f_bfree *
                            (unsigned long long)vols[i].f_bsize);
        shown++;
    }
    /*
     * 主机挂载点很多（实测 80 条以上：/、/dev、/system*、/user、/data、
     * 一大票 nullfs 沙箱目录…）。诊断视图 /list?all=1 会被
     *   (a) MAX_VOLUMES 上限，或 (b) 响应缓冲
     * 截断。明确报出 count（真实总数）/ shown（实际列出条数）/ truncated，
     * 免得让人以为"一共就这么多"。
     *
     * /list（只列 /mnt/usb*）条数极少，不受影响——这一点由测试场景 D 守住：
     * 82 条挂载点、U 盘排在最后，仍必须被列出并能卸载。
     */
    off += snprintf(resp + off, cap - (size_t)off,
                    "],\"shown\":%d,\"truncated\":%s}",
                    shown, (shown < total) ? "true" : "false");
    return off;
}

static int handle_eject(const char *mount, const char *query,
                        char *resp, size_t cap)
{
    char mnt[512];
    int rc, off, force;

    force = query && strstr(query, "force=1") != NULL;

    if (!mount || !*mount) {
        snprintf(resp, cap, "{\"ok\":false,\"msg\":\"missing 'mount' parameter\"}");
        return (int)strlen(resp);
    }
    if (!path_allowed(mount)) {
        json_escape(mount, mnt, sizeof mnt);
        snprintf(resp, cap,
                 "{\"ok\":false,\"msg\":\"path not allowed: %s%s\"}", mnt,
                 T("（只允许 /mnt/usb<数字>；/mnt/ext* 为主机内部存储）",
                   "（只允許 /mnt/usb<數字>；/mnt/ext* 為主機內部儲存）",
                   " (only /mnt/usb<number> is allowed; /mnt/ext* is internal storage)"));
        return (int)strlen(resp);
    }
    /* 双保险：该挂载点必须确实存在，防止误伤与拼写错误 */
    if (!mount_present(mount)) {
        json_escape(mount, mnt, sizeof mnt);
        snprintf(resp, cap,
                 "{\"ok\":false,\"msg\":\"not mounted: %s%s\"}", mnt,
                 T("（该挂载点当前不存在）", "（該掛載點目前不存在）", " (that mount point does not exist)"));
        return (int)strlen(resp);
    }

    /* 1) 落盘：把页缓存与元数据刷到介质 */
    sync();

    /* 2) 卸载 */
    errno = 0;
    rc = unmount(mount, force ? MNT_FORCE : 0);
    if (rc != 0) {
        /*
         * 失败时不再只说一句 "Device busy" 就完事——那跟"出错了"没区别，
         * 用户拿不到任何可行动的线索。这里把**占用者名单**一并带回去，
         * 界面可以直接列出"是哪个进程、打开的是哪个文件"，并给出
         * 一颗「解除占用并卸载」的按钮。
         */
        int e = errno;
        int n, scanned = 0, readable = 0, herr = 0;

        sync();
        json_escape(mount, mnt, sizeof mnt);

        n = find_holders(mount, g_holders_a, MAX_HOLDERS, &scanned, &readable, &herr);
        if (herr == 0 && scanned > 0 && readable == 0)
            herr = EPERM;

        /*
         * msg 必须给出**下一步动作**，而不是复述错误：
         *   查到占用者   -> 指向下方列表 + 解除按钮
         *   查不到占用者 -> 说明多半是沙箱 nullfs 挂接（无主引用），
         *                   「解除占用并卸载」会先卸这些挂接再强制卸载
         * release_url 直接给出，页面与脚本都不用自己拼。
         */
        off = snprintf(resp, cap,
                       "{\"ok\":false,\"code\":%d,\"mount\":\"%s\",\"forced\":%s,"
                       "\"busy\":%s,\"msg\":\"%s%s%s%s\"",
                       e, mnt, force ? "true" : "false",
                       (e == EBUSY) ? "true" : "false",
                       T("卸载失败：", "卸載失敗：", "unmount failed: "), strerror(e),
                       T("。", "。", ". "),
                       (n > 0)
                         ? T("谁在占用见列表，点「解除占用并卸载」自动处理",
                             "誰在佔用見清單，點「解除佔用並卸載」自動處理",
                             "see the holder list below, or use the release button")
                         : T("未查到占用进程（多为系统沙箱挂接），点「解除占用并卸载」强制处理",
                             "未查到佔用處理程序（多為系統沙盒掛接），點「解除佔用並卸載」強制處理",
                             "no holding process found (usually a system sandbox mount); "
                             "use the release button to force it"));
        off += snprintf(resp + off, room(cap, off),
                        ",\"holders_supported\":%s,\"holders\":",
                        herr ? "false" : "true");
        append_holders(resp, cap, &off, g_holders_a, n);
        off += snprintf(resp + off, room(cap, off), ",\"holder_count\":%d", n);
        off += snprintf(resp + off, room(cap, off),
                        ",\"release_url\":\"/release?mount=%s&confirm=1\"", mnt);
        if (n == 0) {
            off += snprintf(resp + off, room(cap, off),
                            ",\"hint\":\"%s\"",
                            herr
                              ? T("这台主机不允许读取进程的文件句柄表（KERN_PROC_FILEDESC "
                                  "被拒），因此列不出占用者；解除占用将跳过终止进程，"
                                  "直接解除系统挂接并强制卸载",
                                  "這台主機不允許讀取處理程序的檔案句柄表（KERN_PROC_FILEDESC 被拒），因此列不出佔用者；解除佔用將跳過終止處理程序，直接解除系統掛接並強制卸載",
                                  "this host refuses to expose the process file-descriptor "
                                  "table (KERN_PROC_FILEDESC denied), so holders cannot be "
                                  "listed; releasing will skip termination and go straight "
                                  "to releasing system mounts and forcing the unmount")
                              : T("没有进程打开这块盘上的文件，占用大概率来自系统沙箱"
                                  "的 nullfs 挂接（应用把盘里的目录挂进了自己的视图）；"
                                  "解除占用会先卸掉这些挂接再强制卸载",
                                  "沒有處理程序開啟這顆磁碟上的檔案，佔用多半來自系統沙盒的 nullfs 掛接（應用程式把磁碟裡的目錄掛進自己的檢視）；解除佔用會先卸掉這些掛接再強制卸載",
                                  "no process has a file open on this volume, so the holder "
                                  "is most likely a system sandbox nullfs mount (an app "
                                  "mounted directories from this volume into its own view); "
                                  "releasing will drop those mounts first, then force the "
                                  "unmount"));
        }
        off += snprintf(resp + off, room(cap, off), "}");
        return off;
    }

    sync();
    json_escape(mount, mnt, sizeof mnt);
    snprintf(resp, cap,
             "{\"ok\":true,\"mount\":\"%s\",\"forced\":%s,"
             "\"msg\":\"%s\"}",
             mnt, force ? "true" : "false",
             T("已落盘并卸载，现在可以安全拔出了",
               "已落盤並卸載，現在可以安全拔出了",
               "synced and unmounted — it is now safe to unplug"));
    return (int)strlen(resp);
}

/* 对端是否为回环地址（127.0.0.1）。/shutdown 只允许本机触发。 */
static int peer_is_loopback(int fd)
{
    struct sockaddr_in p;
    socklen_t l = sizeof p;

    if (getpeername(fd, (struct sockaddr *)&p, &l) != 0)
        return 0;
    return p.sin_family == AF_INET && p.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
}

/*
 * 读 HTTP 响应，直到对端关闭连接或缓冲满。
 *
 * **不能只 recv() 一次。** 响应头与响应体是两次 send()，TCP 很容易把它们分到
 * 两个报文段里；只收一次就会出现"看到头、看不到体"，而恰恰是体里有我们要的
 * 东西（/version 的 version、/shutdown 的 state）。这个坑真实踩过：
 * 更名后 /version 的头部不再含标识串，探针就时灵时不灵。
 *
 * 本服务每个响应都带 Connection: close 且发完即关，所以读到 0 就是读完了。
 * SO_RCVTIMEO 仍然生效，对端不关也不会无限等。
 */
static int recv_all(int fd, char *buf, size_t cap)
{
    size_t got = 0;

    if (!buf || cap == 0)
        return 0;
    while (got + 1 < cap) {
        ssize_t n = recv(fd, buf + got, cap - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    buf[got] = '\0';
    return (int)got;
}

/*
 * 请求上一个占用本端口的实例退出。
 *
 * 之所以走"请它自己退出"而不是 kill(pid)：越狱工具箱加载 payload 时
 * 一律命名为 payload.elf，进程名无从分辨，而 pid 复用可能误杀其它 payload。
 * 走本机 HTTP 则只可能命中"正在监听该端口的那一个"，且旧实例自带
 * 回环校验，天然不会误伤。
 *
 * 返回 1 表示对方确认接管；0 表示端口上要么没人、要么是个不支持该端点的实例
 * ——两种情况都由调用方按 bind 结果处理。
 *
 * 认两种回执：JSON 的 "state":"shutting_down"，以及更早的纯文本
 * "shutting down"。认得出回执才谈得上平滑接管。
 */
static int retire_previous(int port)
{
    int fd, n;
    char buf[512];
    struct sockaddr_in a;
    struct timeval tv;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;

    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port);

    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return 0;
    }

    snprintf(buf, sizeof buf,
             "GET /shutdown HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n");
    if (send(fd, buf, strlen(buf), 0) < 0) {
        close(fd);
        return 0;
    }

    n = recv_all(fd, buf, sizeof buf);
    close(fd);
    if (n <= 0)
        return 0;
    return strstr(buf, "\"state\":\"shutting_down\"") != NULL ||
           strstr(buf, "shutting down") != NULL;
}

/*
 * 看看这个端口上是不是已经有一个自己在跑。
 *
 * 为什么不用进程名：工具箱加载 payload 时一律命名为 payload.elf，认不出自己；
 * 而 pid 会复用，按名字杀会误伤别的 payload。只监听这个端口的进程才可能是
 * "上一次加载的自己"，所以判据就是"端口 + 自述的机器标识"。
 *
 * 判定 /version 而不是另开一个端点，是为了让任何版本的实例都认得出：
 * /version 是本服务最早的接口之一，自述字段只会越来越多。
 *
 * 返回：
 *   1 = 是本程序（ver 带回对方版本，失败时为空串）
 *   0 = 端口上没人
 *  -1 = 有 HTTP 服务但它不自述为 usbmanage（别人的程序占着端口）
 *  -2 = 连上了但没回话（多半正卡在一次卸载里）——按"已有实例"处理，
 *       宁可把界面叫出来，也不要再起一个、更不要自杀式退出
 */
static int probe_instance(int port, char *ver, size_t vercap)
{
    int fd, n;
    char buf[768];
    char req[256];
    struct sockaddr_in a;
    struct timeval tv;
    const char *p;

    if (ver && vercap)
        ver[0] = '\0';

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;

    tv.tv_sec = 0;
    tv.tv_usec = 600000;                        /* 探测必须快，不拖启动 */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port);

    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return 0;                               /* 没人监听 */
    }

    snprintf(req, sizeof req,
             "GET /version HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n");
    if (send(fd, req, strlen(req), 0) < 0) {
        close(fd);
        return -2;
    }

    n = recv_all(fd, buf, sizeof buf);
    close(fd);
    if (n <= 0)
        return -2;                              /* 连上了但不回话 */

    /*
     * 认自己**只认机器标识那个字段**，不认界面上给人看的名字。
     *
     * 这条判据不能写成"响应里有没有 usbmanage 这个子串"：一旦英文显示名不再
     * 等于机器标识（现在是 "PS5 USB Manager"），/version 的正文里就再没有小写的
     * usbmanage，于是每次重复加载都会被判成"别人的程序占着端口"，换版本接管
     * 彻底失效。机器标识是协议字段，只能按字段认。
     *
     * 认两个字段：现行的 "app"（与 /shutdown 的同名字段一致，是正式的机器标识），
     * 以及更早版本用的 "name_en"（那时显示名就等于机器标识）。
     */
    if (!strstr(buf, "\"app\":\"" APP_ID "\"") &&
        !strstr(buf, "\"name_en\":\"" APP_ID "\""))
        return -1;

    if (ver && vercap) {
        p = strstr(buf, "\"version\":\"");
        if (p) {
            size_t i = 0;
            p += 11;
            while (*p && *p != '"' && i + 1 < vercap)
                ver[i++] = *p++;
            ver[i] = '\0';
        }
    }
    return 1;
}

/* 可靠发送：send() 可能短写，必须循环到发完。 */
static void send_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0)
            return;
        p += w;
        n -= (size_t)w;
    }
}

/*
 * 送出内置选择页。
 *
 * 页面直接以裸 send 分两段发出（头 + 体），不经 out[] 缓冲——页面有 6 KB 多，
 * 塞进栈上的缓冲不划算，分开发也更省栈。
 * Content-Length 用编译期常量，绝不用 strlen 之外的猜测。
 */
static void send_index(int fd)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html; charset=utf-8\r\n"
                     "Cache-Control: no-store\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n\r\n",
                     (unsigned)usbmanage_index_html_len);
    if (n <= 0)
        return;
    send_all(fd, hdr, (size_t)n);
    send_all(fd, (const char *)usbmanage_index_html, (size_t)usbmanage_index_html_len);
}

/*
 * 应用图标：96x96 PNG（assets/icon/icon-96.png），
 * 是页面里 32~40 px 显示尺寸的 2~3 倍图，够覆盖高 DPI，只有 12.6 KB。
 *
 * /favicon.ico 返回同一份字节：浏览器按内容嗅探，PNG 放在 .ico 路径上没问题，
 * 这样"地址栏敲 /favicon.ico"和页面里显式声明的 <link rel="icon" href="/icon.png">
 * 都能拿到图（此前这里是空的 204，浏览器标签页上只有个默认图标）。
 */
static void send_icon(int fd)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: image/png\r\n"
                     "Cache-Control: no-store\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n\r\n",
                     (unsigned)usbmanage_icon_png_len);
    if (n <= 0)
        return;
    send_all(fd, hdr, (size_t)n);
    send_all(fd, (const char *)usbmanage_icon_png, (size_t)usbmanage_icon_png_len);
}

/* 极简 HTTP：只解析请求行 "GET /path?query HTTP/1.1" 与查询串 */
static void serve_client(int fd)
{
    char req[BUFSZ];
    char body[BUFSZ];
    char out[BUFSZ + 512];
    char path[512] = "";
    char query[1024] = "";
    char *sp, *qm, *nl;
    int n, blen;
    int wants_html = 0;     /* 请求来自浏览器导航，见下方 Accept 判断 */
    int page_xhr = 0;       /* 请求来自本页自己的 XHR，见下方 /shutdown 鉴权 */
    int hdr_lang = LANG_EN; /* 请求头 Accept-Language 判出的语言（截断前读） */

    n = (int)recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0)
        return;
    req[n] = '\0';

    /*
     * 判断这是不是"人在浏览器里点开的"请求。
     *
     * 必须在这一步判、且用整个请求缓冲——下面会把第一行截断，头就没了。
     * 浏览器导航请求的 Accept 一定含 text/html；curl、脚本、以及页面自己的
     * XHR（显式声明 Accept: application/json）都不含。
     *
     * 用途见下面 JSON 端点前的重定向：把"打开 /list 只看到一坨 JSON"
     * 这种困惑直接消掉。
     */
    if (strstr(req, "text/html") != NULL)
        wants_html = 1;

    /*
     * 本页自己的 XHR 会带 X-Requested-With: usbmanage（见 web/index.html 的
     * get()）。这是 /shutdown 的第二把钥匙：跨站的 <img>、<script>、地址栏
     * 回车都带不上自定义头，因此拿不到"关停服务"这个能力。
     */
    if (strstr(req, "X-Requested-With: usbmanage") != NULL ||
        strstr(req, "x-requested-with: usbmanage") != NULL)
        page_xhr = 1;

    /*
     * 界面语言也必须在截断之前读掉，理由与上面两条完全相同：头只存在于
     * 完整缓冲里。这里先算出一个布尔值，等查询串解析完再交给 apply_lang——
     * 那里还要处理优先级更高的 ?lang=。
     */
    hdr_lang = accept_language_lang(req);

    /* 取第一行 */
    nl = strchr(req, '\n');
    if (nl)
        *nl = '\0';

    if (strncmp(req, "GET ", 4) != 0) {
        snprintf(out, sizeof out,
                 "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        send(fd, out, strlen(out), 0);
        return;
    }

    sp = strchr(req + 4, ' ');
    if (sp)
        *sp = '\0';
    strncpy(path, req + 4, sizeof(path) - 1);

    qm = strchr(path, '?');
    if (qm) {
        *qm = '\0';
        strncpy(query, qm + 1, sizeof(query) - 1);
    }

    /*
     * 界面语言定案：hdr_lang 来自上面（截断前读的请求头），?lang= 优先级更高。
     * 之后所有 T() 都按这个结果走。
     */
    apply_lang(hdr_lang, query);

    /*
     * 选择页：浏览器打开 http://<PS5_IP>:9100/ 即为此页。
     * 页面与本服务同源，页内 fetch/XHR 直接调 /list 与 /eject。
     */
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        send_index(fd);
        return;
    }
    if (strcmp(path, "/icon.png") == 0 || strcmp(path, "/favicon.ico") == 0) {
        send_icon(fd);
        return;
    }

    /*
     * 浏览器里直接打开 /list、/eject、/release 等（比如照着 curl 的地址
     * 粘到地址栏），看到的会是裸 JSON —— 这正是最容易让人以为"没有界面"
     * 的一种情况。这里直接把人引到选择页。
     *
     * 顺带一个安全副作用：/eject 与 /release 就不再可能被"地址栏回车"
     * 误触发（/release 还会终止别的进程），只能从界面里点。
     * 命令行/脚本（Accept 不含 text/html）行为不变。
     */
    if (wants_html && is_json_endpoint(path)) {
        snprintf(out, sizeof out,
                 "HTTP/1.1 302 Found\r\n"
                 "Location: /\r\n"
                 "Cache-Control: no-store\r\n"
                 "Content-Length: 0\r\n"
                 "Connection: close\r\n\r\n");
        send(fd, out, strlen(out), 0);
        return;
    }

    if (strcmp(path, "/ping") == 0) {
        strcpy(body, "pong");
    } else if (strcmp(path, "/version") == 0) {
        /*
         * app   = 机器标识，供"认自己"用（probe_instance 就靠它），
         *         与 /shutdown 的同名字段一致。**它必须存在**：显示名改成
         *         "PS5 USB Manager" 之后，正文里再也没有可作为标识的串了。
         * name 给中文界面看，name_en 给英文界面看；两个都发，调用方自己挑。
         * lang 是本服务按 Accept-Language 判定的界面语言（zh-Hans / zh-Hant /
         * en），界面据此对齐自己的取值，脚本也能一眼看出服务端当前是哪一种。
         */
        snprintf(body, sizeof body,
                 "{\"app\":\"" APP_ID "\",\"name\":\"" APP_NAME_ZH "\","
                 "\"name_hant\":\"" APP_NAME_HANT "\","
                 "\"name_en\":\"%s\",\"version\":\"%s\",\"lang\":\"%s\"}",
                 APP_NAME_EN, VERSION, lang_tag());
    } else if (strcmp(path, "/list") == 0) {
        handle_list(body, sizeof body, strstr(query, "all=1") != NULL);
    } else if (strcmp(path, "/eject") == 0) {
        char mount[512];
        extract_mount(query, mount, sizeof mount);
        handle_eject(mount, query, body, sizeof body);
    } else if (strcmp(path, "/holders") == 0) {
        char mount[512];
        extract_mount(query, mount, sizeof mount);
        handle_holders(mount, body, sizeof body);
    } else if (strcmp(path, "/release") == 0) {
        /*
         * 解除占用。会终止别的进程，因此必须显式 confirm=1，
         * 并且被 is_json_endpoint 挡在"地址栏误触发"之外。
         */
        char mount[512];
        extract_mount(query, mount, sizeof mount);
        handle_release(mount, query, body, sizeof body);
    } else if (strcmp(path, "/procs") == 0) {
        handle_procs(body, sizeof body);
    } else if (strcmp(path, "/openui") == 0) {
        /*
         * 让主机重新把界面弹到它自己屏幕上。上位机（或手机）调一下这个端点，
         * 电视上就会打开选择页——省得走到电视前手动敲地址。
         * 地址用局域网 IP，不是回环地址：这样电视上看到的就是
         * 手机、电脑都能打开的那一个。
         */
        open_ui(g_ip, g_port, UI_REASON_START, NULL);
        snprintf(body, sizeof body,
                 "{\"ok\":true,\"msg\":\"asked the console to open http://%s:%d/\","
                 "\"user_init\":%d,\"browser\":%d,\"notify\":%d,\"knotify\":%d}",
                 g_ip, g_port, g_rc_user_init, g_rc_browser, g_rc_notify, g_rc_knotify);
    } else if (strcmp(path, "/diag") == 0) {
        /*
         * 诊断。真机上"没弹出来"时，先看这里：
         *   rc.browser / rc.notify 非 0  -> 系统调用被主机拒绝（返回码即原因）
         *   ui_attempts = 0              -> 根本没尝试（--no-ui，或没走到那一步）
         *   三个都是 -999                -> 同上，未尝试
         * procscan 则是"能不能查占用进程"——为 false 时 /holders 与
         * /release 都不可用，只能走强制卸载。
         */
        char ipesc[128];
        int nprocs = 0, pscan_ok = 0;
        {
            size_t plen = 0;
            struct kinfo_proc *pt = proc_table(&plen);
            if (pt) {
                char *q, *qe = (char *)pt + plen;
                for (q = (char *)pt; q < qe; ) {
                    struct kinfo_proc *ki = (struct kinfo_proc *)q;
                    if (ki->ki_structsize <= 0)
                        break;
                    q += ki->ki_structsize;
                    if (ki->ki_pid > 0)
                        nprocs++;
                }
                free(pt);
                pscan_ok = 1;
            }
        }
        json_escape(g_ip, ipesc, sizeof ipesc);
        snprintf(body, sizeof body,
                 "{\"name\":\"" APP_NAME_ZH "\",\"name_hant\":\"" APP_NAME_HANT "\","
                 "\"name_en\":\"%s\",\"version\":\"%s\",\"lang\":\"%s\","
                 "\"port\":%d,\"lan_ip\":\"%s\","
                 "\"ui_autolaunch\":%s,\"ui_attempts\":%d,"
                 "\"ui_reason\":\"%s\",\"other_version\":\"%s\","
                 "\"procscan\":{\"supported\":%s,\"procs\":%d},"
                 "\"rc\":{\"user_init\":%d,\"browser\":%d,\"notify\":%d,\"knotify\":%d}}",
                 APP_NAME_EN, VERSION, lang_tag(), g_port, ipesc,
                 g_ui_enabled ? "true" : "false", g_ui_tries,
                 g_ui_reason == UI_REASON_SAME ? "same-version" :
                 g_ui_reason == UI_REASON_BUSY ? "busy" :
                 g_ui_reason == UI_REASON_TAKEOVER ? "takeover" : "start",
                 g_other_ver,
                 pscan_ok ? "true" : "false", nprocs,
                 g_rc_user_init, g_rc_browser, g_rc_notify, g_rc_knotify);
    } else if (strcmp(path, "/shutdown") == 0) {
        /*
         * 关掉自己：界面上的「关闭服务」按钮与"新实例接管旧实例"都走这里。
         *
         * 两个允许的来源：
         *   ① 本机回环 —— 接管时新实例就是这么请求的（不带任何头也放行）；
         *   ② 页面自己的 XHR —— 带 X-Requested-With: usbmanage。
         * 网页里的 <img src="http://主机:9100/shutdown"> 或地址栏回车都带不上
         * 这个头，所以既拦得住 CSRF，也拦得住手滑；再叠加 is_json_endpoint
         * 那道 302（浏览器导航一律跳回选择页），误触发的路就都堵死了。
         */
        if (!peer_is_loopback(fd) && !page_xhr) {
            snprintf(body, sizeof body,
                     "{\"ok\":false,\"app\":\"" APP_ID "\","
                     "\"msg\":\"%s\"}",
                     T("shutdown 只允许本机触发，或由界面上的「关闭服务」发起",
                       "shutdown 只允許本機觸發，或由介面上的「關閉服務」發起",
                       "shutdown is only allowed from this machine, or from the "
                       "close-service button on the page"));
        } else {
            /*
             * state 的值 "shutting_down" 是 retire_previous() 判定"对方是否
             * 已退出"的依据，不是给人看的文案，**不要翻译**。
             */
            snprintf(body, sizeof body,
                     "{\"ok\":true,\"app\":\"" APP_ID "\",\"version\":\"" VERSION "\","
                     "\"state\":\"shutting_down\","
                     "\"msg\":\"%s\"}",
                     T("服务已关闭，可从越狱工具箱重新加载",
                       "服務已關閉，可從越獄工具箱重新載入",
                       "service stopped; you can reload it from the jailbreak toolbox"));
            g_shutdown = 1;
        }
    } else {
        snprintf(body, sizeof body,
                 "{\"ok\":false,\"msg\":\"unknown endpoint. "
                 "open http://%s:%d/ for the web page; "
                 "API: /list, /eject?mount=/mnt/usb0, /holders?mount=/mnt/usb0, "
                 "/release?mount=/mnt/usb0&confirm=1, /procs, /diag\"}",
                 g_ip, g_port);
    }

    blen = (int)strlen(body);
    snprintf(out, sizeof out,
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json; charset=utf-8\r\n"
             "Access-Control-Allow-Origin: *\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n\r\n%s",
             blen, body);
    send(fd, out, strlen(out), 0);
}

/*
 * 取本机在局域网里的 IPv4 地址。
 *
 * 用"UDP connect + getsockname"这个经典办法：UDP 的 connect 一个包都不发，
 * 只是让内核按路由表挑一个源地址，因此断网也不会卡住。取不到就回退 127.0.0.1。
 * 刻意不用 inet_ntoa——少依赖一个运行时符号，就少一处"万解不出来起不来"的位点。
 */
static void local_ip(char *out, size_t cap)
{
    int fd;
    struct sockaddr_in a, me;
    socklen_t ml = sizeof me;

    snprintf(out, cap, "127.0.0.1");

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;

    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = htonl(0x08080808UL);  /* 8.8.8.8，仅用于选路，不发包 */

    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0 &&
        getsockname(fd, (struct sockaddr *)&me, &ml) == 0 &&
        me.sin_family == AF_INET && me.sin_addr.s_addr != 0) {
        const unsigned char *b = (const unsigned char *)&me.sin_addr.s_addr;
        snprintf(out, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    }
    close(fd);
}

/*
 * 屏幕通知的内容（JSON）。
 *
 * 字段结构照抄 SDK 的 samples/notify/main.c（InteractiveToastTemplateB），
 * 只替换文案。图标用 Predefined，避免引用一个不存在的图片路径导致整条通知失败。
 */
/*
 * 组一条系统通知（"消息窗口"，与 dbs 工具箱 / web-file-manager 同一套机制）。
 *
 * JSON 结构逐字对齐 SDK 官方 samples/notify/main.c 的 toast_tmpl：
 * PS5 的通知系统认的是 rawData/InteractiveToastTemplateB 这一整套结构，
 * 自造的简化格式（裸 message 字段）真机上是不显示的。
 *
 * 两个刻意的选择：
 *   1. 不带 actions（DeepLink）。官方样例的 actionUrl 用的是 pssettings: 协议，
 *      http 深链没人验证过主机接不接受——万一不认，整条通知都可能渲染失败。
 *      打开页面有专门的 sceSystemServiceLaunchWebBrowser，不缺这条路。
 *   2. createdDateTime 用真实当前时间。样例写死了 2025 年，固件可能按时间
 *      排序/过滤通知，陈旧时间戳有被归档或丢弃的风险。
 */
static void build_notification(char *out, size_t cap, const char *msg,
                               const char *sub, int port)
{
    char now[40];
    time_t t = time(NULL);
    struct tm *tmv = gmtime(&t);

    if (tmv)
        strftime(now, sizeof now, "%Y-%m-%dT%H:%M:%S.000Z", tmv);
    else
        snprintf(now, sizeof now, "1970-01-01T00:00:00.000Z");

    snprintf(out, cap,
        "{"
          "\"rawData\":{"
            "\"viewTemplateType\":\"InteractiveToastTemplateB\","
            "\"channelType\":\"Downloads\","
            "\"useCaseId\":\"IDC\","
            "\"toastOverwriteType\":\"No\","
            "\"isImmediate\":true,"
            "\"priority\":100,"
            "\"viewData\":{"
              "\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":\"download\"}},"
              "\"message\":{\"body\":\"%s\"},"
              "\"subMessage\":{\"body\":\"%s\"}"
            "},"
            "\"platformViews\":{"
              "\"previewDisabled\":{"
                "\"viewData\":{"
                  "\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":\"download\"}},"
                  "\"message\":{\"body\":\"%s\"}"
                "}"
              "}"
            "}"
          "},"
          "\"createdDateTime\":\"%s\","
          "\"localNotificationId\":\"9100%d\"",
        msg, sub, sub, now, port);
}

/*
 * 把界面弹到主机自己的屏幕上——这就是"别的 payload 加载后会自己弹东西"的实现。
 *
 *   1) sceUserServiceInitialize   官方 samples/browser 在调用浏览器接口前会先做
 *   2) sceSystemServiceLaunchWebBrowser  让主机打开内置浏览器，指向本服务
 *   3) sceNotificationSend        屏幕上弹一条通知（附带局域网地址，方便手机打开）
 *
 * 三个返回码全部留在全局变量里，由 /diag 报出。若主机上不生效，凭返回码就能
 * 判断是"接口被拒"还是"根本没跑到"，不必靠猜。
 *
 * 取舍说明：本函数在调用它的线程里同步执行，没有 fork 隔离。这么做是为了让
 * 整段逻辑能在宿主机上被真实跑通（Windows 没有 fork）。代价是万一系统调用把
 * 进程带崩，HTTP 服务会一起消失——启动参数 --no-ui 是退路，可先只留接口。
 */
static void open_ui(const char *ip, int port, int reason, const char *other_ver)
{
    char url[128];
    char note[2048];
    char msg[192], sub[288];
    usbmanage_notify_request_t req;
    const char *host;

    g_ui_tries++;
    g_ui_reason = reason;
    if (other_ver)
        snprintf(g_other_ver, sizeof g_other_ver, "%s", other_ver);

    /*
     * 用局域网 IP 而不是 127.0.0.1：
     * 电视上打开的这个地址，与手机/电脑上要打开的地址是同一个，
     * 页脚显示的、通知里写的也就都是"真能用的那个"。
     * 取不到局域网地址时 local_ip() 会回退 127.0.0.1，此时仍可用。
     */
    host = (ip && *ip) ? ip : "127.0.0.1";
    snprintf(url, sizeof url, "http://%s:%d/", host, port);

    /*
     * 三种启动局面的文案要分出差别：重复加载时最要命的是"以为换了版本、
     * 其实主机上跑的还是旧的"，这句话必须说在屏幕上，而不是只写进日志
     * （真机上没人看得到 stdout）。
     */
    if (reason == UI_REASON_SAME) {
        snprintf(msg, sizeof msg, "%s%s", app_name(),
                 T("：界面已为你打开", "：介面已為你開啟", ": the page has been opened"));
        snprintf(sub, sizeof sub, "%s http://%s:%d/%s",
                 T("管理地址", "管理位址", "address"), host, port,
                 T("（主机上已有同一版本在运行，未重复启动）",
                   "（主機上已有同一版本在執行，未重複啟動）",
                   " (same version already running; no second instance started)"));
    } else if (reason == UI_REASON_BUSY) {
        snprintf(msg, sizeof msg, "%s%s", app_name(),
                 T("：界面已为你打开", "：介面已為你開啟", ": the page has been opened"));
        snprintf(sub, sizeof sub, "%s http://%s:%d/%s",
                 T("管理地址", "管理位址", "address"), host, port,
                 T("（主机上已有实例在运行但未应答，多半正忙，未重复启动）",
                   "（主機上已有實例在執行但未回應，多半正忙，未重複啟動）",
                   " (an instance is running but did not answer; likely busy; "
                   "no second instance started)"));
    } else if (reason == UI_REASON_TAKEOVER) {
        /*
         * 空格放在译串里，不要放在格式串里：中文用全角冒号「已接管：」，
         * 后面再跟一个半角空格就成了"已接管： v1.0.0"，很难看；
         * 英文 " took over: " 则需要那个空格。格式串里再带一个就重复了。
         */
        snprintf(msg, sizeof msg, "%s%sv%s -> v%s", app_name(),
                 T(" 已接管：", " 已接管：", " took over: "),
                 g_other_ver[0] ? g_other_ver : "?", VERSION);
        snprintf(sub, sizeof sub, "%s http://%s:%d/",
                 T("管理地址", "管理位址", "address"), host, port);
    } else {
        snprintf(msg, sizeof msg, "%s v%s%s", app_name(), VERSION,
                 T(" 已启动", " 已啟動", " started"));
        snprintf(sub, sizeof sub, "%s http://%s:%d/",
                 T("管理地址", "管理位址", "address"), host, port);
    }

    g_rc_user_init = sceUserServiceInitialize(0);
    printf("[usbmanage] sceUserServiceInitialize -> %d\n", g_rc_user_init);

    g_rc_browser = sceSystemServiceLaunchWebBrowser(url, 0);
    printf("[usbmanage] sceSystemServiceLaunchWebBrowser(%s) -> %d\n", url, g_rc_browser);

    build_notification(note, sizeof note, msg, sub, port);
    g_rc_notify = sceNotificationSend(SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM, true, note);
    printf("[usbmanage] sceNotificationSend -> %d\n", g_rc_notify);

    /*
     * 第二条通道：内核通知（samples/notify_debug 走的就是这条）。
     * 真机上 sceNotificationSend 那一套不显示，而这条是经典的系统通知，
     * PS4/PS5 上被大量 payload 使用。两条都发、两个返回码都进 /diag，
     * 不预判哪条生效——真机上一看返回码就知道。
     */
    memset(&req, 0, sizeof req);
    snprintf(req.message, sizeof req.message, "%s\n%s", msg, sub);
    g_rc_knotify = sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
    printf("[usbmanage] sceKernelSendNotificationRequest -> %d\n", g_rc_knotify);

    fflush(stdout);
}

int main(int argc, char **argv)
{
    int port = LISTEN_PORT;
    int srv, cli;
    int on = 1;
    int i;
    struct sockaddr_in addr;
    char took_from[32] = "";    /* 接管时对方的版本，用于启动通知的文案 */

    /*
     * 参数：数字 = 改端口；--no-ui = 不自动拉起界面（只保留 /openui 手动触发）；
     *       --force = 端口上已有同版本实例时也照样接管（默认是"激活它、不起
     *                 第二个"，见头部「单实例」一节）。
     * 越狱工具箱的 /loadpayload: 通常不支持追加参数，所以这些主要在命令行
     * 注入（nc 到 9021 那条路）以及宿主机测试时有用。
     */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-ui") == 0)
            g_ui_enabled = 0;
        else if (strcmp(argv[i], "--ui") == 0)
            g_ui_enabled = 1;
        else if (strcmp(argv[i], "--force") == 0)
            g_force = 1;
        else if (is_digit(argv[i][0]))
            port = atoi(argv[i]);
    }
    if (port <= 0)
        port = LISTEN_PORT;
    g_port = port;

    /*
     * 界面语言（只影响屏幕通知）。通知在任何 HTTP 请求之前就发出去了，没有
     * Accept-Language 可读，只能取进程环境里的语言变量作最佳努力判定——
     * Orbis 上未必设，取不到就按英文走（符合"中文以外一律英文"的约定）。
     * HTTP 层不受这里影响：每个请求都会按自己的请求头重新判定（serve_client）。
     */
    g_lang = env_lang();

    local_ip(g_ip, sizeof g_ip);

    /*
     * ---- 单实例判定 ----
     *
     * 工具箱重复加载同名 payload 时不会杀掉上一次的实例，所以这里先问一句
     * "这个端口上是不是已经有一个自己在跑"，再决定启动、激活还是接管：
     *
     *   同版本  -> 激活：把界面叫到屏幕上，本进程立刻退出。
     *              用户插了新盘再点一次，要的是"把界面调出来"，不是每点一次
     *              多一个进程。
     *   异版本  -> 接管：请旧的退出、自己顶上。否则刚上传的新版本永远不生效，
     *              而这恰恰是最难查的一种"部署了却没生效"。
     *   不回话  -> 也按"已有实例"处理（它多半正卡在一次卸载里）。宁可把界面
     *              叫出来等它忙完，也不要再起一个、更不要自杀式退出。
     * --force -> 一律走接管那一条。
     */
    {
        char ever[32];
        int st = probe_instance(port, ever, sizeof ever);

        if (st == -1) {
            fprintf(stderr,
                    "[" APP_ID "] port %d is held by something that does not identify "
                    "itself as " APP_ID ".\n"
                    "         Not touching it. Change the port: usbmanage.elf <port>\n",
                    port);
            return 1;
        }

        if (!g_force && (st == 1 || st == -2) &&
            (st == -2 || (ever[0] && strcmp(ever, VERSION) == 0))) {
            printf("[" APP_ID "] " APP_NAME_EN " v%s is already running on port %d "
                   "(probe=%d); activating it instead of starting a second instance.\n",
                   ever[0] ? ever : "?", port, st);
            printf("[usbmanage] to swap in another version, click "
                   "the shutdown button on the page first, then load again.\n");
            fflush(stdout);
            if (g_ui_enabled)
                open_ui(g_ip, port, st == -2 ? UI_REASON_BUSY : UI_REASON_SAME, ever);
            return 0;
        }

        if (st == 1) {
            snprintf(took_from, sizeof took_from, "%s", ever);
            printf("[usbmanage] port %d runs v%s; taking it over with v%s%s\n",
                   port, ever[0] ? ever : "?", VERSION, g_force ? " (--force)" : "");
            fflush(stdout);

            /*
             * 主动请旧的退出，而不是等 bind 失败才发现冲突。
             *
             * 两个理由：
             *   ① 真机上这省掉一次"先失败、再重试"的来回；
             *   ② 宿主机（Windows）的 SO_REUSEADDR 允许两个监听者绑同一端口
             *      —— 与 BSD 语义相反，光靠 bind 失败根本发现不了冲突，
             *      这段接管逻辑在测试里就成了死角。主动请求之后，两边行为
             *      一致，测试也就能真的覆盖到它。
             * 对方不响应（版本太老、没有 /shutdown 的实例）时不用就此放弃：
             * 继续往下走，bind 会给出最终结论。
             */
            if (retire_previous(port)) {
                printf("[usbmanage] previous instance acknowledged; waiting for the port\n");
                fflush(stdout);
                usleep(500000);         /* 留出端口释放的时间 */
            } else {
                fprintf(stderr,
                        "[usbmanage] the running instance did not answer /shutdown "
                        "(an older build without /shutdown?). Continuing anyway; "
                        "bind will decide.\n");
            }
        }
    }

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    {
        int attempt;
        for (attempt = 0; ; attempt++) {
            if (bind(srv, (struct sockaddr *)&addr, sizeof addr) == 0)
                break;

            /*
             * 端口被占。多半是上一次加载的同名 payload 还在跑（工具箱重复加载）。
             * 请它从 127.0.0.1 收到 /shutdown 后退出，再把自己的端口接过来。
             */
            if (attempt == 0) {
                printf("[usbmanage] port %d busy (%s), asking previous instance to retire...\n",
                       port, strerror(errno));
                fflush(stdout);
                if (retire_previous(port)) {
                    printf("[usbmanage] previous instance acknowledged; taking over\n");
                    fflush(stdout);
                    usleep(500000);     /* 留出端口释放的时间 */
                    continue;
                }
                fprintf(stderr,
                        "[usbmanage] cannot take over port %d: the process holding it "
                        "is an older build without /shutdown.\n"
                        "         Kill it from the toolbox's process manager, then "
                        "load again.\n",
                        port);
            }
            perror("bind");
            close(srv);
            return 1;
        }
    }

    if (listen(srv, 8) < 0) {
        perror("listen");
        close(srv);
        return 1;
    }

    /* g_ip 在上面做单实例判定之前就填好了（激活路径也要用） */

    /* 主机侧日志走英文名：终端里不会有编码问题，也便于贴到 issue 里 */
    printf("[" APP_ID "] " APP_NAME_EN " v%s listening on 0.0.0.0:%d\n", VERSION, port);
    printf("[usbmanage] web UI: http://%s:%d/   (from anywhere on the LAN, or on the console itself)\n",
           g_ip, port);
    fflush(stdout);

    /*
     * 列表已经在 listen() 之后、accept() 之前的这一小段里发出去是不影响的：
     * 内核的 backlog 会替我们把浏览器的连接先收着，等我们进入 accept 再服务。
     */
    if (g_ui_enabled) {
        printf("[usbmanage] asking the console to open its browser and show a toast...\n");
        fflush(stdout);
        open_ui(g_ip, port, took_from[0] ? UI_REASON_TAKEOVER : UI_REASON_START, took_from);
        printf("[usbmanage] if the screen did not change, check /diag for the return codes\n");
    } else {
        printf("[usbmanage] auto-open disabled (--no-ui); use /openui or open the URL manually\n");
    }
    fflush(stdout);

    for (;;) {
        struct sockaddr_in cli_addr;
        socklen_t alen = sizeof(cli_addr);

        cli = accept(srv, (struct sockaddr *)&cli_addr, &alen);
        if (cli < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }
        serve_client(cli);
        close(cli);
        if (g_shutdown) {
            printf("[usbmanage] shutting down (requested by a newer instance)\n");
            fflush(stdout);
            break;
        }
    }

    close(srv);
    return 0;
}
