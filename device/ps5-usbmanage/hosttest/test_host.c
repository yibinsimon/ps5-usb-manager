/*
 * 宿主机逻辑自测：验证 usbmanage.c 中与平台无关的纯函数。
 * 被测代码由 awk 从 usbmanage.c 原文抽取（pure.inc），保证与交付 elf 同源。
 * 覆盖 URL 百分号解码、挂载点白名单、卷标解析、界面语言判定这几处。
 */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>     /* getenv —— env_lang() 要用 */

/* usbmanage.c 中的常量，抽取时未包含 */
static const char *USB_PREFIX = "/mnt/usb";

/* holder_killable 依赖的 struct holder（与 usbmanage.c 一致，抽取时未包含） */
#define HOLDER_PATH 384
struct holder {
    int  pid;
    char comm[64];
    int  fd;
    char path[HOLDER_PATH];
    int  is_self;
};

#include "pure.inc"

/* ---- 造引导扇区的小工具（卷标用例用）---- */
static void put8(unsigned char *b, int off, unsigned v)
{
    b[off] = (unsigned char)v;
}

static void put16(unsigned char *b, int off, unsigned v)
{
    b[off] = (unsigned char)(v & 0xFF);
    b[off + 1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put32(unsigned char *b, int off, unsigned v)
{
    b[off] = (unsigned char)(v & 0xFF);
    b[off + 1] = (unsigned char)((v >> 8) & 0xFF);
    b[off + 2] = (unsigned char)((v >> 16) & 0xFF);
    b[off + 3] = (unsigned char)((v >> 24) & 0xFF);
}

static void putbytes(unsigned char *b, int off, const char *s, int n)
{
    memcpy(b + off, s, (size_t)n);
}

/*
 * 512 字节的 FAT 引导扇区。fat16=1 用 FATSz16（判为 FAT12/16），
 * fat16=0 用 FATSz32 且根目录项数为 0（判为 FAT32）。
 * label 短于 11 字节时按 8.3 规范补空格。
 */
static void mk_fat(unsigned char *s, int fat16, const char *label)
{
    int i, done = 0;

    memset(s, 0, 512);
    put8(s, 0x00, 0xEB); put8(s, 0x01, 0x58); put8(s, 0x02, 0x90);
    putbytes(s, 0x03, "MSDOS5.0", 8);
    put16(s, 0x0B, 512);
    put8(s, 0x0D, 1);
    put16(s, 0x0E, 1);
    put8(s, 0x10, 1);
    put16(s, 0x11, fat16 ? 16 : 0);
    put8(s, 0x15, 0xF8);
    put16(s, 0x16, fat16 ? 1 : 0);
    put16(s, 0x18, 32);
    put16(s, 0x1A, 64);
    put32(s, 0x20, 0x4000);
    if (!fat16) {
        put32(s, 0x24, 1);    /* FATSz32 */
        put32(s, 0x2C, 2);    /* 根目录首簇 = 2 */
    }
    /*
     * 8.3 卷标两套偏移只能写一套：FAT12/16 在 0x2B，FAT32 在 0x47。
     * FAT32 的 0x2B 起是 FSVer(0x2A-0x2B) / RootClus(0x2C-0x2F) /
     * FSInfo(0x30-0x31)，两个都写就会把根目录簇号覆盖成名字里的字节。
     * （这里踩过：写成 "OLDDISK" 后 rd32(0x2C) 读回 0x4944404C = "LDDI"。）
     */
    for (i = 0; i < 11; i++) {
        unsigned char c = ' ';
        if (!done) {
            c = (unsigned char)label[i];
            if (c == 0) { c = ' '; done = 1; }
        }
        s[(fat16 ? 0x2B : 0x47) + i] = c;
    }
    put8(s, 0x1FE, 0x55); put8(s, 0x1FF, 0xAA);
}

/* 512 字节的 exFAT 引导扇区：512B/扇区、1 扇区/簇、根目录簇 2、簇堆在 1 号扇区 */
static void mk_exfat(unsigned char *s)
{
    memset(s, 0, 512);
    put8(s, 0x00, 0xEB); put8(s, 0x01, 0x76); put8(s, 0x02, 0x90);
    putbytes(s, 0x03, "EXFAT   ", 8);
    put32(s, 0x58, 1);
    put32(s, 0x5C, 64);
    put32(s, 0x60, 2);
    put8(s, 0x6C, 9);
    put8(s, 0x6D, 0);
    put8(s, 0x6E, 1);
    put8(s, 0x1FE, 0x55); put8(s, 0x1FF, 0xAA);
}

static int fails = 0;

static void ck(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    /* Windows 的 CRT 把 _IOLBF 当块缓冲处理，崩溃点会被吞掉半行；
       每条断言后刷一次，跑挂了才能一眼看出是停在哪一条上。 */
    fflush(stdout);
    if (!cond)
        fails++;
}

static void ck_path(const char *p, int want, const char *label)
{
    int got = path_allowed(p);
    char buf[256];
    snprintf(buf, sizeof buf, "path_allowed(%s) == %d  %s", p ? p : "NULL", want, label);
    ck(got == want, buf);
}

static void ck_dec(const char *in, const char *want)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", in);
    url_decode(buf);
    char msg[512];
    snprintf(msg, sizeof msg, "url_decode(\"%s\") -> \"%s\"", in, buf);
    ck(strcmp(buf, want) == 0, msg);
}

int main(void)
{
    /* 行缓冲：跑到哪一步崩的，一眼就能看出来（管道下默认是块缓冲，
       崩溃点会被吞掉半行，反而难查）。 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("== 1. 白名单：仅 /mnt/usb<数字> 放行 ==\n");
    ck_path("/mnt/usb0",  1, "(合法)");
    ck_path("/mnt/usb1",  1, "(合法)");
    ck_path("/mnt/usb12", 1, "(合法，多位)");
    ck_path("/mnt/usb999", 1, "(合法)");

    printf("== 2. 白名单：内部存储与畸形输入必须拒绝 ==\n");
    ck_path("/mnt/ext0",  0, "(扩展存储，内部卷)");
    ck_path("/mnt/ext1",  0, "(内置 SSD 用户分区)");
    ck_path("/mnt/usb",   0, "(无编号)");
    ck_path("/mnt/usb0/", 0, "(尾随斜杠)");
    ck_path("/mnt/usb/0", 0, "(斜杠分隔编号)");
    ck_path("/mnt/usbx",  0, "(非数字)");
    ck_path("/mnt/usb0a", 0, "(数字后接字母)");
    ck_path("/mnt/USB0",  0, "(大小写)");
    ck_path("//mnt/usb0", 0, "(双斜杠)");
    ck_path("/mnt/usb0/../ext0", 0, "(路径穿越)");
    ck_path("/",          0, "(根)");
    ck_path("",           0, "(空串)");
    ck_path(NULL,         0, "(NULL)");
    ck_path("/mnt/usb0\n", 0, "(尾随换行)");

    printf("== 3. URL 解码 ==\n");
    ck_dec("%2Fmnt%2Fusb0", "/mnt/usb0");
    ck_dec("%2fmnt%2fusb1", "/mnt/usb1");
    ck_dec("/mnt/usb0", "/mnt/usb0");
    ck_dec("a+b", "a b");
    ck_dec("%2F", "/");
    ck_dec("%ZZ", "%ZZ");      /* 非法转义保持原样 */
    ck_dec("%2", "%2");        /* 截断转义保持原样 */
    ck_dec("", "");

    printf("== 4. 端到端：查询串解码后仍须过白名单 ==\n");
    {
        char a[256] = "%2Fmnt%2Fusb0";
        url_decode(a);
        ck(path_allowed(a) == 1, "解码 %2Fmnt%2Fusb0 后放行 /mnt/usb0");
    }
    {
        char b[256] = "%2Fmnt%2Fext0";
        url_decode(b);
        ck(path_allowed(b) == 0, "解码 %2Fmnt%2Fext0 后仍拒绝内部存储");
    }

    printf("== 5. JSON 转义 ==\n");
    {
        char o[128];
        json_escape("a\"b\\c", o, sizeof o);
        ck(strcmp(o, "a\\\"b\\\\c") == 0, "引号与反斜杠被转义");
        json_escape("/mnt/usb0", o, sizeof o);
        ck(strcmp(o, "/mnt/usb0") == 0, "普通路径原样输出");
    }

    printf("== 6. room()：剩余空间计算 ==\n");
    ck(room(8192, 0) == 8192, "room(8192, 0) == 8192（off==0 必须返回全量，否则首写被丢弃）");
    ck(room(8192, 10) == 8182, "room(8192, 10) == 8182");
    ck(room(8192, 8192) == 0, "room(8192, 8192) == 0（越界夹到 0）");
    ck(room(8192, 9000) == 0, "room(8192, 9000) == 0（off 越过 cap）");
    ck(room(8192, -1) == 0, "room(8192, -1) == 0（负 off）");
    ck(room(0, 0) == 0, "room(0, 0) == 0（cap==0）");

    printf("== 7. path_under()：子路径边界判定 ==\n");
    ck(path_under("/mnt/usb0/movie.mkv", "/mnt/usb0") == 1, "文件在挂载点下 -> 命中");
    ck(path_under("/mnt/usb0", "/mnt/usb0") == 1, "路径等于挂载点 -> 命中");
    ck(path_under("/mnt/usb0/", "/mnt/usb0") == 1, "尾随斜杠 -> 命中");
    ck(path_under("/mnt/usb00/a", "/mnt/usb0") == 0, "/mnt/usb00 不是 /mnt/usb0 的子路径");
    ck(path_under("/mnt/usb1/a", "/mnt/usb0") == 0, "不同挂载点");
    ck(path_under("/mnt/usb0", "/mnt/usb01") == 0, "挂载点更长时不匹配");
    ck(path_under("", "/mnt/usb0") == 0, "空路径");
    ck(path_under("/mnt/usb0", "") == 0, "空挂载点");
    ck(path_under(NULL, "/mnt/usb0") == 0, "NULL 路径");

    printf("== 8. holder_killable()：三道不杀的护栏 ==\n");
    {
        struct holder h;
        memset(&h, 0, sizeof h);

        h.pid = 100; strcpy(h.comm, "player.elf"); h.is_self = 0;
        ck(holder_killable(&h) == 1, "普通进程 -> 可终止");

        h.is_self = 1;
        ck(holder_killable(&h) == 0, "本进程自己 -> 不杀");

        h.is_self = 0; h.pid = 1;
        ck(holder_killable(&h) == 0, "pid 1 -> 不杀");

        h.is_self = 0; h.pid = 0;
        ck(holder_killable(&h) == 0, "pid 0 -> 不杀");

        h.pid = 10; strcpy(h.comm, "SceShellCore.elf");
        ck(holder_killable(&h) == 0, "Sce* 前缀 -> 不杀");

        strcpy(h.comm, "init");
        ck(holder_killable(&h) == 0, "init -> 不杀");

        strcpy(h.comm, "kernel");
        ck(holder_killable(&h) == 0, "kernel -> 不杀");
    }

    printf("== 9. extract_mount()：带开关的查询串不再被截断 ==\n");
    {
        char m[64];

        extract_mount("mount=%2Fmnt%2Fusb0", m, sizeof m);
        ck(strcmp(m, "/mnt/usb0") == 0, "单参数：解码 %2F 并取出");

        extract_mount("mount=%2Fmnt%2Fusb0&force=1", m, sizeof m);
        ck(strcmp(m, "/mnt/usb0") == 0, "mount 在前：只取 mount 值，不吞 force");

        extract_mount("force=1&mount=%2Fmnt%2Fusb0&confirm=1", m, sizeof m);
        ck(strcmp(m, "/mnt/usb0") == 0, "mount 居中：前后开关都不影响");

        extract_mount("confirm=1&force=1", m, sizeof m);
        ck(strcmp(m, "") == 0, "无 mount：返回空");

        extract_mount(NULL, m, sizeof m);
        ck(strcmp(m, "") == 0, "NULL 查询串：返回空");

        extract_mount("mount=%2Fmnt%2Fusb0", m, 8);
        ck(strlen(m) < 8, "cap 截断：不越界");
    }

    printf("== 10. 卷标：引导扇区识别 ==\n");
    {
        unsigned char s[512];

        mk_exfat(s);
        {
            struct fsboot g;
            ck(fsboot_parse(s, sizeof s, &g) == 1 && g.kind == FS_EXFAT,
               "认得出 exFAT（EXFAT + 55AA + 跳转指令）");
            ck(g.bps == 512 && g.spc == 1 && g.root_clus == 2,
               "exFAT 几何量：512B/扇区、1 扇区/簇、根目录簇 2");
        }

        mk_fat(s, 0, "NO NAME");
        {
            struct fsboot g;
            ck(fsboot_parse(s, sizeof s, &g) == 1 && g.kind == FS_FAT32,
               "FATSz16=0 且根目录项数=0 -> 判为 FAT32");
            /* 数据区 = (保留 1 + FAT 1*1 + 根目录 0) 扇区 * 512 = 1024 */
            ck(g.data_off == 1024, "FAT32 数据区偏移 = 1024（簇 2 在这里）");
        }

        mk_fat(s, 1, "BPBLABEL");
        {
            struct fsboot g;
            ck(fsboot_parse(s, sizeof s, &g) == 1 && g.kind == FS_FAT12_16,
               "FATSz16 非 0 -> 判为 FAT12/16");
            /* 固定根目录 = (保留 1 + FAT 1*1) 扇区 * 512 = 1024，16 项 = 512 字节 */
            ck(g.root_off == 1024 && g.root_len == 512,
               "FAT12/16 固定根目录：偏移 1024、长度 512");
            ck(memcmp(g.bpb_label, "BPBLABEL   ", 11) == 0,
               "引导扇区里的 8.3 卷标被读进 bpb_label（兜底用）");
        }

        memset(s, 0xA5, sizeof s);
        {
            struct fsboot g;
            ck(fsboot_parse(s, sizeof s, &g) == 0, "没有 55AA 结束标记 -> 不认识");
        }
        memset(s, 0, sizeof s);
        {
            struct fsboot g;
            s[0x1FE] = 0x55; s[0x1FF] = 0xAA;
            ck(fsboot_parse(s, sizeof s, &g) == 0,
               "有 55AA 但不是跳转指令开头 -> 仍然不认识");
        }
        mk_fat(s, 0, "NO NAME");
        {
            struct fsboot g;
            ck(fsboot_parse(s, 100, &g) == 0, "不足 512 字节 -> 直接放弃（防御短读）");
        }
    }

    printf("== 11. 卷标：UTF-16LE -> UTF-8 ==\n");
    {
        char o[64];

        {
            static const unsigned char u[] = {'A',0, 'B',0, 'C',0};
            utf16le_to_utf8(u, 3, o, sizeof o);
            ck(strcmp(o, "ABC") == 0, "ASCII：逐字节搬运");
        }
        {
            /* 移 U+79FB 动 U+52A8 硬 U+786C 盘 U+76D8，写成 UTF-8 字节比较，
               避免依赖源码文件的编码（Windows 上窄字符串字面量可能是 GBK） */
            static const unsigned char u[] = {0xfb,0x79, 0xa8,0x52, 0x6c,0x78, 0xd8,0x76};
            static const char want[] = "\xE7\xA7\xBB\xE5\x8A\xA8\xE7\xA1\xAC\xE7\x9B\x98";
            utf16le_to_utf8(u, 4, o, sizeof o);
            ck(strcmp(o, want) == 0, "4 个汉字的代理区外码位 -> 正确 UTF-8");
        }
        {
            /* U+1F600 = D83D DE00（代理对）-> F0 9F 98 80 */
            static const unsigned char u[] = {0x3d,0xd8, 0x00,0xde};
            static const char want[] = "\xF0\x9F\x98\x80";
            utf16le_to_utf8(u, 2, o, sizeof o);
            ck(strcmp(o, want) == 0, "代理对 -> 4 字节 UTF-8");
        }
        {
            static const unsigned char u[] = {'A',0, 0x01,0, 'B',0, 0x7f,0, 'C',0};
            utf16le_to_utf8(u, 5, o, sizeof o);
            ck(strcmp(o, "ABC") == 0, "控制字符一律丢弃（不写进页面）");
        }
        {
            static const unsigned char u[] = {'A',0, 0x00,0, 'B',0};
            utf16le_to_utf8(u, 3, o, sizeof o);
            ck(strcmp(o, "A") == 0, "0x0000 视为结束，后面的不再读");
        }
        {
            static const unsigned char u[] = {'A',0, 0xfb,0x79, 'B',0};
            utf16le_to_utf8(u, 3, o, 2);
            ck(strlen(o) == 1, "cap 不足时截断而不是越界写");
        }
    }

    printf("== 12. 卷标：8.3 短字段 -> 可显示字符串 ==\n");
    {
        char o[32];

        ck(label_from_83((const unsigned char *)"BACKUP     ", o, sizeof o) == 1 &&
           strcmp(o, "BACKUP") == 0, "尾部空格是填充，去掉");
        ck(label_from_83((const unsigned char *)"MY DISK    ", o, sizeof o) == 1 &&
           strcmp(o, "MY DISK") == 0, "中间的空格是名字的一部分，不能删");
        ck(label_from_83((const unsigned char *)"NO NAME    ", o, sizeof o) == 0,
           "\"NO NAME\" = 没有卷标（Windows 的占位写法）");
        ck(label_from_83((const unsigned char *)"           ", o, sizeof o) == 0,
           "全空格 = 没有卷标");
        {
            /* 这个字段是定长 11 字节，测试也必须给足 11 字节 —— 只给 8 字节
               会让被测函数按契约读完，越界读到自己栈上的东西。 */
            static const unsigned char gbk[11] = {
                0xd2, 0xc6, 0xb6, 0xaf, ' ', ' ', ' ', ' ', ' ', ' ', ' '
            };
            ck(label_from_83(gbk, o, sizeof o) == 0,
               "非 ASCII 字节（GBK 的\"移动\"）拒收，交给根目录里的长名项");
        }
    }

    printf("== 13. 卷标：exFAT 根目录 0x83 项 ==\n");
    {
        unsigned char d[32 * 4];
        char o[64];
        static const char want[] = "\xE7\xA7\xBB\xE5\x8A\xA8\xE7\xA1\xAC\xE7\x9B\x98";

        memset(d, 0, sizeof d);
        d[0] = 0x83; d[1] = 4;
        d[2] = 0xfb; d[3] = 0x79; d[4] = 0xa8; d[5] = 0x52;
        d[6] = 0x6c; d[7] = 0x78; d[8] = 0xd8; d[9] = 0x76;
        ck(exfat_label_in_dir(d, sizeof d, o, sizeof o) == 1 && strcmp(o, want) == 0,
           "0x83 项里的 UTF-16 名字 -> UTF-8");

        memset(d, 0, sizeof d);
        ck(exfat_label_in_dir(d, sizeof d, o, sizeof o) == 0,
           "目录项以 0x00 开头 = 到此为止 -> 没有卷标");

        memset(d, 0, sizeof d);
        d[0] = 0x03; d[1] = 4;             /* 已删除的卷标项 */
        ck(exfat_label_in_dir(d, sizeof d, o, sizeof o) == 0,
           "0x03（已删除）不算卷标");

        memset(d, 0, sizeof d);
        d[0] = 0x83; d[1] = 99;            /* 字符数越界 */
        ck(exfat_label_in_dir(d, sizeof d, o, sizeof o) == 0,
           "字符数 > 11 的畸形项直接判无效");
    }

    printf("== 14. 卷标：FAT 根目录（长名项 + 8.3 短项）==\n");
    {
        unsigned char d[32 * 4];
        char o[64];
        static const char want[] = "\xE5\xA4\x87\xE4\xBB\xBD\xE7\x9B\x98";  /* 备份盘 */

        /* 长名项：order=1 且带 0x40（名字开头那一段），属性 0x0F */
        memset(d, 0, sizeof d);
        d[0] = 0x41; d[11] = 0x0F;
        {
            static const unsigned short u[] = {0x5907, 0x4efd, 0x76d8, 0xFFFF};
            int i;
            for (i = 0; i < 4; i++) {
                int base = (i < 5) ? 1 + i * 2 : (i < 11 ? 14 + (i - 5) * 2 : 28 + (i - 11) * 2);
                d[base]     = (unsigned char)(u[i] & 0xFF);
                d[base + 1] = (unsigned char)(u[i] >> 8);
            }
        }
        d[32] = 'B'; d[33] = 'E'; d[34] = 'I'; d[35] = 'F'; d[36] = '~';
        d[37] = '1'; d[43] = 0x08;          /* 短项，属性 0x08 = 卷标 */
        ck(fat_label_in_dir(d, sizeof d, o, sizeof o) == 1 && strcmp(o, want) == 0,
           "长名项拼回中文卷标（短项里放不下，真正的名字在这里）");

        /* 只有 8.3 短项 */
        memset(d, 0, sizeof d);
        memcpy(d, "BACKUP     ", 11);
        d[11] = 0x08;
        ck(fat_label_in_dir(d, sizeof d, o, sizeof o) == 1 && strcmp(o, "BACKUP") == 0,
           "没有长名项时退回 8.3 短项");

        memset(d, 0, sizeof d);
        ck(fat_label_in_dir(d, sizeof d, o, sizeof o) == 0, "空目录 -> 没有卷标");

        /* 被删掉的目录项必须把前面攒了一半的长名清掉：否则一个已删除的旧
           名字会挂到后面那个真实卷标上。构造 = 长名项 + 已删除项 + 卷标短项。 */
        memset(d, 0, sizeof d);
        d[0] = 0x41; d[11] = 0x0F;           /* 属于旧名字的长名项 */
        d[1] = 'Z'; d[2] = 0;                /* 旧名字是 Z，必须被丢掉 */
        d[32] = 0xE5;                        /* 已删除项 */
        d[33] = 'X';
        memcpy(d + 64, "NEW        ", 11); d[75] = 0x08;
        ck(fat_label_in_dir(d, sizeof d, o, sizeof o) == 1 && strcmp(o, "NEW") == 0,
           "被删除的目录项不会与后面的名字混在一起");
    }

    printf("== 15. 端到端：同一份扇区喂给两层（几何量必须自洽）==\n");
    {
        /* 这一段在守的是"偏移算得对不对"：HTTP 层那几张合成镜像是按同样的
           几何量摆的（引导扇区 512B -> FAT 512B -> 根目录 512B），
           这里若算错，真机上就会读到无关扇区。 */
        unsigned char img[1536];
        unsigned char *root = img + 1024;
        struct fsboot g;
        char o[64];

        memset(img, 0, sizeof img);
        mk_fat(img, 0, "OLDDISK");          /* FAT32，根目录簇 2 */
        memset(root, 0, 512);
        memcpy(root, "BACKUP     ", 11);
        root[11] = 0x08;

        ck(fsboot_parse(img, 512, &g) == 1 && g.kind == FS_FAT32, "（前置）识别为 FAT32");
        ck(g.bps == 512 && g.spc == 1 && g.root_clus == 2 && g.data_off == 1024,
           "几何量自洽：512B/扇区、1 扇区/簇、根目录簇 2、数据区在 1024");
        ck(fat_label_in_dir(img + g.data_off + (g.root_clus - 2) * g.spc * g.bps,
                            g.spc * g.bps, o, sizeof o) == 1 &&
           strcmp(o, "BACKUP") == 0,
           "按 data_off + 簇号算出的位置，正好落在真正的根目录上");

        /* FAT32 根目录为空时，必须退回引导扇区里的 8.3 卷标 */
        memset(img + 1024, 0, 512);
        ck(fat_label_in_dir(img + g.data_off, 512, o, sizeof o) == 0, "（前置）根目录里没有卷标");
        ck(label_from_83((const unsigned char *)g.bpb_label, o, sizeof o) == 1 &&
           strcmp(o, "OLDDISK") == 0, "兜底：用引导扇区里的 8.3 卷标");
    }

    printf("== 16. 界面语言判定（中文按简繁分档，其余一律英文）==\n");
    {
        /*
         * 约定：系统语言是中文就上中文界面，简体走简体、繁体走繁体；
         * 其余（含日、韩、欧语）一律英文。
         * 这里穷举"哪些标记算哪一档"——判错会让繁体用户拿到简体、
         * 或者让英文用户拿到中文。
         */
        ck(lang_of("zh") == LANG_ZH_HANS, "zh 算简体");
        ck(lang_of("zh-CN") == LANG_ZH_HANS, "zh-CN 算简体");
        ck(lang_of("zh-Hans") == LANG_ZH_HANS, "zh-Hans 算简体");
        ck(lang_of("zh-Hans-CN") == LANG_ZH_HANS, "zh-Hans-CN 算简体");
        ck(lang_of("zh-SG") == LANG_ZH_HANS, "zh-SG 算简体");
        ck(lang_of("zh_CN") == LANG_ZH_HANS, "下划线写法 zh_CN 也算");
        ck(lang_of("ZH-cn") == LANG_ZH_HANS, "大小写不敏感");
        ck(lang_of("  zh-CN") == LANG_ZH_HANS, "前导空格不影响");
        ck(lang_of("zh-CN,zh;q=0.9,en;q=0.8") == LANG_ZH_HANS, "带权重列表也认");
        ck(lang_of("ZH-HANS") == LANG_ZH_HANS, "ZH-HANS 大写也认");

        ck(lang_of("zh-TW") == LANG_ZH_HANT, "zh-TW 算繁体");
        ck(lang_of("zh-HK") == LANG_ZH_HANT, "zh-HK 算繁体");
        ck(lang_of("zh-MO") == LANG_ZH_HANT, "zh-MO 算繁体");
        ck(lang_of("zh-Hant") == LANG_ZH_HANT, "zh-Hant 算繁体");
        ck(lang_of("zh-Hant-TW") == LANG_ZH_HANT, "zh-Hant-TW 算繁体");
        ck(lang_of("zh_Hant") == LANG_ZH_HANT, "下划线写法 zh_Hant 也算繁体");
        ck(lang_of("zh-TW,zh;q=0.9") == LANG_ZH_HANT, "繁体在前带权重列表也认");

        ck(lang_of("zh-XX") == LANG_ZH_HANS, "认不出的中文地区仍算中文，给简体");
        ck(lang_of("en-US") == LANG_EN, "en-US 走英文");
        ck(lang_of("ja-JP") == LANG_EN, "ja-JP 走英文（不是中文）");
        ck(lang_of("ko-KR") == LANG_EN, "ko-KR 走英文");
        ck(lang_of("") == LANG_EN, "空串走英文");
        ck(lang_of(NULL) == LANG_EN, "NULL 走英文");
        ck(lang_of("zhuang") == LANG_EN, "前缀相同但不是语言标记时不误判");
        ck(lang_of("zhx") == LANG_EN, "zh 后面接非分隔符不误判");

        ck(starts_with_ci("Hello", "he") == 1, "starts_with_ci：忽略大小写");
        ck(starts_with_ci("Hello", "HELLO") == 1, "starts_with_ci：全大写也匹配");
        ck(starts_with_ci("Hi", "hello") == 0, "starts_with_ci：不匹配时为 0");
        ck(starts_with_ci("", "") == 1, "starts_with_ci：空前缀恒真");

        ck(accept_language_lang(
               "GET / HTTP/1.1\r\nHost: x\r\nAccept-Language: zh-CN,zh;q=0.9\r\n\r\n") == LANG_ZH_HANS,
           "从请求头读出简体");
        ck(accept_language_lang(
               "GET / HTTP/1.1\r\nAccept-Language: zh-TW\r\n\r\n") == LANG_ZH_HANT,
           "从请求头读出繁体");
        ck(accept_language_lang(
               "GET / HTTP/1.1\r\nAccept-Language: zh-Hant-HK\r\n\r\n") == LANG_ZH_HANT,
           "zh-Hant-HK 也是繁体");
        ck(accept_language_lang(
               "GET / HTTP/1.1\r\nAccept-Language:en-US\r\n\r\n") == LANG_EN,
           "冒号后无空格、值为英文 -> 英文");
        ck(accept_language_lang(
               "GET / HTTP/1.1\r\naccept-language: zh-CN\r\n\r\n") == LANG_ZH_HANS,
           "头名小写也认");
        ck(accept_language_lang("GET / HTTP/1.1\r\nHost: x\r\n\r\n") == LANG_EN,
           "没有 Accept-Language -> 英文");
    }

    /*
     * 17. 通知语言的环境变量取值。
     *
     * 屏幕通知在任何 HTTP 请求之前发出，那时没有请求头，只能看环境变量。
     * 这里最要紧的一条是"C/POSIX 不算语言偏好，要跳过往下找"——实测
     * Git Bash 默认设着 LC_ALL=C.UTF-8，不跳过的话用户设的
     * LANG=zh_CN.UTF-8 永远读不到，通知一律英文。
     *
     * putenv 会接管传入的内存并可能改写它（glibc 不复制），所以每个用例都用
     * 一块独立的静态缓冲，绝不把字符串字面量交出去。
     */
    printf("== 17. 通知语言的环境变量取值（C / POSIX 不算语言偏好）==\n");
    {
        static char ebuf[32][96];
        int ei = 0;

#define SETENV(k, v) do {                                        \
            if (ei < 32) {                                       \
                snprintf(ebuf[ei], sizeof ebuf[0], "%s=%s", (k), (v)); \
                putenv(ebuf[ei]);                                \
                ei++;                                            \
            }                                                    \
        } while (0)

        SETENV("LC_ALL", "");
        SETENV("LC_MESSAGES", "");
        SETENV("LANG", "");
        ck(env_lang() == LANG_EN, "三个变量都为空 -> 英文");

        SETENV("LANG", "zh_CN.UTF-8");
        ck(env_lang() == LANG_ZH_HANS, "只设 LANG=zh_CN.UTF-8 -> 简体");

        SETENV("LC_ALL", "C.UTF-8");
        ck(env_lang() == LANG_ZH_HANS, "LC_ALL=C.UTF-8 不遮蔽后面的 LANG（真实踩过的坑）");

        SETENV("LC_ALL", "C");
        ck(env_lang() == LANG_ZH_HANS, "LC_ALL=C 也不遮蔽");

        SETENV("LC_ALL", "POSIX");
        ck(env_lang() == LANG_ZH_HANS, "LC_ALL=POSIX 也不遮蔽");

        SETENV("LC_ALL", "en_US.UTF-8");
        ck(env_lang() == LANG_EN, "LC_ALL 明确是英文 -> 英文（不越过它去看 LANG）");

        SETENV("LC_ALL", "zh_CN.UTF-8");
        ck(env_lang() == LANG_ZH_HANS, "LC_ALL 明确是中文 -> 简体");

        SETENV("LC_ALL", "");
        SETENV("LC_MESSAGES", "zh_CN.UTF-8");
        ck(env_lang() == LANG_ZH_HANS, "LC_ALL 为空时看 LC_MESSAGES");

        SETENV("LC_MESSAGES", "");
        SETENV("LANG", "zh_TW.UTF-8");
        ck(env_lang() == LANG_ZH_HANT, "LANG=zh_TW.UTF-8 -> 繁体");

        SETENV("LANG", "zh_HK.UTF-8");
        ck(env_lang() == LANG_ZH_HANT, "LANG=zh_HK.UTF-8 -> 繁体");

        SETENV("LANG", "zh_CN.UTF-8");
        ck(env_lang() == LANG_ZH_HANS, "LANG=zh_CN.UTF-8 -> 简体");

        SETENV("LANG", "ja_JP.UTF-8");
        ck(env_lang() == LANG_EN, "LANG=ja_JP.UTF-8 -> 英文");

        SETENV("LANG", "");
#undef SETENV
    }

    printf("\n%s  失败 %d 项\n", fails ? "==> 有失败 ==" : "==> 全部通过 ==", fails);
    return fails ? 1 : 0;
}
