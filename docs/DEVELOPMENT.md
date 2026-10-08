# PS5 USB管理器 · 开发文档

改代码、编译 payload、跑测试、把 elf 送进主机、上机验收——这些事看这一份。
只想把工具用起来，看 [README](../README.md) 就够了。

> 返回 [README](../README.md) ｜ [English dev doc](DEVELOPMENT.en.md)

## 目录

- [构建 payload](#构建-payload)
- [测试](#测试)
- [上机验收](#上机验收)
- [部署到主机](#部署到主机)
- [接口一览](#接口一览)
- [单实例（重复加载与接管）](#单实例重复加载与接管)
- [卷标解析](#卷标解析)
- [应用图标](#应用图标)
- [安全边界](#安全边界)
- [名称约定](#名称约定)
- [设计要点（择要）](#设计要点择要)
- [目录结构](#目录结构)

---

## 构建 payload

需要 `ps5-payload-sdk` 加宿主机的 `clang` / `ld.lld`。Windows 上用 llvm-mingw 就够，**不需要 WSL，也不需要 Docker**——SDK 发行包自带 Windows 工具链（`win/prospero-lld.exe`、`win/ninja.exe`）。

```bash
cd device/ps5-usbmanage
bash build.sh
```

脚本先读环境变量，再去 `$HOME` / `/opt` / 上级目录里找 SDK；都找不到就直接报错并打印设置方法，不会静默用一个写死的路径。

```bash
export PS5_PAYLOAD_SDK="$HOME/ps5-payload-sdk"        # 解压后的 SDK 根目录
export PS5_CLANG="/c/llvm-mingw/bin/clang.exe"        # 不设则自动从 PATH 找 clang
```

脚本自己生成内嵌页面数组、编译，然后做一轮自校验：ELF 头、`NEEDED`、动态符号、页面与图标有没有真的进二进制、名称有没有编进去。任何一项不过，直接失败退出。

```bash
# Linux / macOS
sudo apt-get install clang-18 lld-18     # Debian 系；或 brew install llvm lld
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make
```

构建会先把 `web/index.html` 用 `xxd` 转成 `web/index_html.h` 再编译，因此需要 `xxd`（随 vim-common 提供）。缺它会直接报错退出，不会静默编出无页面的 elf。

`win\build.cmd` 是同一入口的 cmd 版本：PATH 里有 `bash` 时它直接转调 `build.sh`，否则退化为纯 cmd 调 clang。

### 三个踩过的坑（脚本里已处理，换环境时注意）

1. **不要用 SDK 自带的 `win/prospero-clang.cmd`。** `bin/prospero-clang`（POSIX 版）在 clang 主版本 ≥ 20 时会清空 `crt1.o`，因为 clang 20 起驱动会自动追加 crt 目标文件；而 `.cmd` 版漏了这个判断，仍以绝对路径再传一次 `crt1.o`，链接必然报 `duplicate symbol: payload_exit`。本脚本因此不显式传 `crt1.o`。
2. **Git Bash 下 PATH 条目必须是 POSIX 形态（`/c/...`）。** 放 `C:/...` 会让 MSYS 的 PATH 转换出错，clang 报 `unable to execute command: program not executable`。
3. **PATH 必须同时含 clang 目录与 `SDK/win` 两处。** `prospero-lld.exe` 只是个壳，内部再调 `ld.lld`——前者在 `SDK/win`，后者在 clang 同目录。另建议设 `SCE_PROSPERO_SDK_DIR`，让驱动把 `--sysroot` 指向 SDK 自身。

### 关键参数

与官方 `samples/mntinfo` 一致：

```bash
clang --start-no-unused-arguments \
  -target x86_64-sie-ps5 \
  -fvisibility-nodllstorageclass=default \
  -isysroot "$SDK" -isystem "$SDK/target/include" \
  -L "$SDK/target/lib" -L "$SDK/target/user/homebrew/lib" \
  -fno-stack-protector -fno-plt -femulated-tls \
  -lc -lkernel_sys \
  --end-no-unused-arguments \
  usbmanage.c -o usbmanage.elf \
  --start-no-unused-arguments --sysroot "$SDK" \
  -lSceLibcInternal -lSceNet \
  --end-no-unused-arguments
```

用 `-lkernel_sys`（而不是默认的 `-lkernel_web`）是因为本 payload 要调 `getfsstat()` / `unmount()`——官方枚举挂载点的示例 `samples/mntinfo` 正是这么链的。产物结构：

```
Class: ELF64      OS/ABI: UNIX - FreeBSD     Type: DYN (PIE)     Machine: x86-64
NEEDED: libkernel_sys / libSceLibcInternal / libSceNet / libkernel_web
        + libSceSystemService / libSceUserService / libSceNotification   (弹界面用)
```

交付件在 `dist/`：

| 文件 | 说明 |
|---|---|
| `dist/usbmanage-v1.0.0.elf` | **当前交付版本** |
| `dist/SHA256SUMS.txt` | sha256 校验和，**以文件内的实际值为准** |
| `dist/_legacy/` | 开发期产物归档（不入库），只作本地备查 |

> **交付前必查**：`usbmanage.c` / `web/index.html` / `web/icon.png` 的 mtime 必须**早于** `usbmanage.elf`。曾出现源码改过、却在交付旧产物的情况——产物比源码旧就说明没重新构建。

> **关于 `PT_DYNAMIC` 警告**：`llvm-readelf -l` 在本产物上会报 `invalid PT_DYNAMIC size`。这是链接器把 `.dynamic` / `.dynsym` / `.dynstr` / `.rela.dyn` 归入同一个 `PT_DYNAMIC` 段所致，触发条件只是段大小不是 16 字节的整数倍——**SDK 自带的官方样例 `samples/mntinfo` 同样会报**（实测：本产物 0x1978 / 2 条，`mntinfo-ref` 0x1018 / 2 条，而 `browser-ref`、`notify-ref` 为 0 条）。`.dynamic` 本身以 `DT_NULL` 正常收尾（26 条），运行时加载器按该数组遍历，不受段大小影响。属可忽略的告警。

> **关于 sha256 的含义**：链接器会往 `.note.gnu.build-id` 写 16 字节 build-id，每次链接都不同，所以**同样的源码两次编译 sha256 必然不一致**。实测核对过：剥掉这个 note 段后，两次构建的其余字节完全一致——语义上是可复现的，变的只有那个指纹。因此这里的 sha256 用途是**校验传输/拷贝过程有没有损坏**（`sha256sum -c dist/SHA256SUMS.txt`），不要把它当作"源码 → 字节"的复现凭据。

---

## 测试

三层验证，都在宿主机上跑，不需要 PS5 在线：

```bash
cd device/ps5-usbmanage

bash hosttest/run.sh        # 139 项 —— 纯逻辑（抽函数出来，用宿主机 clang 编）
bash hosttest/web/run.sh    #  73 项 —— 页面 JS 逻辑（极简 DOM，需要 node）
bash hosttest/http/run.sh   # 259 项 —— HTTP 层端到端（C 原文编成可执行 + 真监听端口 + curl）
```

| 层 | 断言数 | 覆盖什么 |
|---|---|---|
| `hosttest/run.sh` | 139 | 白名单判定、卷标解析、语言判定、路径解码这些纯函数 |
| `hosttest/web/run.sh` | 73 | 页面渲染、语言切换（简体 / 繁体 / 英文各一套断言）、`pending` 反馈回填 |
| `hosttest/http/run.sh` | 259 | 全部接口、状态码、CSRF 与接管握手、三语 `msg`、通知文案 |

`hosttest/http` 会在宿主机上真的起一个服务，用 `curl` 打接口，跑完大约 3 分钟。

### 纯逻辑层（139）

`hosttest/run.sh` 用 `awk` 按**函数名**从 `usbmanage.c` 原文抽出与平台无关的纯函数（`path_allowed` / `url_decode` / `json_escape` / `room` / `path_under` / `holder_killable` / `extract_mount` / `fsboot_parse` / `utf16le_to_utf8` / `label_from_83` / `exfat_label_in_dir` / `fat_label_in_dir` / `starts_with_ci` / `lang_of` / `accept_language_lang` / `env_lang` 等）生成 `pure.inc`，保证被测代码就是要编进 elf 的那份，不是副本。语言相关的类型（`LANG_ZH_HANS` / `LANG_ZH_HANT` / `LANG_EN`）与卷标的结构体一样，用显式标记从原文里夹出来，测试与实现共用同一份定义。覆盖：

- 白名单放行：`/mnt/usb0`、`/mnt/usb1`、`/mnt/usb12`…
- 白名单拒绝：`/mnt/ext0`、`/mnt/ext1`（内部存储）、`/mnt/usb`、`/mnt/usb0/`、`/mnt/usb0/../ext0`、`//mnt/usb0`、`/mnt/usbx`、NULL、空串…
- URL 解码：`%2Fmnt%2Fusb0` → `/mnt/usb0`；非法转义 `%ZZ`、`%2` 保持原样；解码后再过白名单（`%2Fmnt%2Fext0` 仍被拒）
- `room()`：剩余空间计算（`off==0` 必须返回全量，否则首写被丢弃）
- `path_under()`：子路径边界（`/mnt/usb00` 不算 `/mnt/usb0` 的子路径）
- `holder_killable()`：三道不杀的护栏（本进程 / pid≤1 / `Sce*` 前缀 / `init` / `kernel`）
- `extract_mount()`：带 `force=1`、`confirm=1` 开关的查询串不再被截断
- 引导扇区识别：`55 AA` 与跳转字节两道门；exFAT 的 `"EXFAT   "` 标记；FAT32 的判据是「FATSz16=0 且 根目录项数=0」；`bps` 非 512/1024/2048/4096、`spc` 非 2 的幂、保留扇区/FAT 个数越界一律拒收
- UTF-16LE → UTF-8：BMP、代理对（U+1F600 要出 4 字节）、控制字符丢弃、`0x0000`/`0xFFFF` 视为结束、`cap` 不足时截断而非越界
- 8.3 短字段：尾部空格是填充要去掉、中间空格是名字的一部分要留、`"NO NAME"` 与全空格按"没有卷标"处理、非 ASCII 一律拒收
- 根目录项：exFAT 的 `0x83`（`0x03` 已删除不算、字符数 >11 判畸形）、FAT 的 `0x0F` 长名项按 `order` 拼回（被删项 `0xE5` 必须清掉攒了一半的长名）
- 端到端几何自洽：同一份合成扇区喂给两层，`data_off + (簇号-2)×spc×bps` 必须正好落在真正的根目录上

> **两个造数据的坑**：① `label_from_83()` 契约上要读满 11 字节，测试里用 8 字节的字面量喂它会读越界、在宿主机上直接段错误——往被测的定长字段里传字面量时长度必须给够。② 造引导扇区时 **8.3 卷标只能往一套偏移写**（FAT12/16 在 `0x2B`、FAT32 在 `0x47`），两处都写会把 FAT32 的根目录首簇（`0x2C`-`0x2F`）连同 FSInfo 覆盖掉，解析出来的簇号就成了名字里的字节（写成 `"OLDDISK"` 会读回 `0x4944404C` = `"LDDI"`）。

这一层的定位是把"与 PS5 无关、却最容易写错"的部分在本地穷举掉。早前 `%2F` 那个缺陷就属于这类——当时只能上机才暴露，代价高。

### HTTP 端到端层（259）

这一套把 `usbmanage.c` **原文**（一个字符都不改）在宿主机上编成可执行文件并真实监听端口，然后用 `curl` 逐个打接口。PS5 专有的调用由 `hosttest/http/stub.c` 顶替：

| 被顶替的 | 桩件行为 |
|---|---|
| `getfsstat` | 返回可控的假挂载表（已插盘 / 无盘 / 只读诊断 / 带沙箱 nullfs 视图 / 带卷标的合成镜像） |
| `unmount` | 记录调用并可模拟成功、`EBUSY` 失败、或"首次 EBUSY 之后成功" |
| `sync` | Windows 无此调用，空实现 |
| `sysctl` | 按环境变量伪造进程表 / fd 表（`USBMANAGE_FAKE_PROCS` / `USBMANAGE_FAKE_FDS`） |
| `kill` | 记录"对谁、发了什么信号"到 `kill.log`，并真的从假进程表里移除（`USBMANAGE_FAKE_KILL_FAIL=1` 时返回 -1） |
| `sceKernelSendNotificationRequest` | 把消息写进 `notify_kernel.txt`（便于断言"第二个通道也发了、内容对不对"） |
| `getpeername` | 按 `USBMANAGE_FAKE_PEER_IP` 返回来源地址（测 `/shutdown` 的回环鉴权） |

为此准备了三处纯适配（**不修改 `usbmanage.c`**）：`prologue.h`（`-include` 注入，补 `sync`/`kill` 声明、修正 winsock `setsockopt` 参数类型，并把 `getpeername` 宏重定向成 `usbmanage_host_getpeername`——Windows 导入库里已有这个名字，桩件用同名会报 `duplicate symbol`）、`shim/`（补 llvm-mingw 砍掉的 BSD 兼容头）、`-Dclose=usbmanage_host_close`。

> **卷标在宿主机上是怎么被真实测到的**：`hosttest/http/mkfixtures.py` 生成 5 张合成镜像（exFAT 的 `0x83` 中文名 / FAT32 的长名项 + 短项 / FAT16 的固定根目录 / FAT32 空根目录退回引导扇区卷标 / 纯垃圾字节）。假挂载表里把这些**文件名当成设备名**，于是 `read_volume_label()` 里真正的 `fopen`/`fread`/簇偏移计算全部被跑到，只是读的是普通文件而不是块设备。用完即删，不进 elf。

覆盖的十四个场景：

| 场景 | 假挂载表 | 验的东西 |
|---|---|---|
| A 已插两块 U 盘 | `/` + ext0 + ext1 + usb0 + usb1 | `/version`（含中英文名）、**`GET /` 的页面与 `web/index.html` 字节完全一致**、**启动时确实调了浏览器接口与通知接口**（参数、顺序、返回码，且指向局域网 IP 而非 127.0.0.1）、**通知 JSON 对齐官方 rawData 结构、无自造 actionUrl、时间戳非写死值**、`/diag` 字段、`/list` 只列外接卷、编码与明文两种 `/mnt/ext0` 均被拒、尾斜杠/路径穿越/`/mnt/ext2`/空参数均被拒、**浏览器导航到 `/list` 或 `/eject` 被 302 到 `/`**、`%2Fmnt%2Fusb0` 真卸载成功、**`/icon.png` 与 `/favicon.ico` 都返回 `image/png` 且与 `web/icon.png` 字节完全一致**、**页面里确实声明了 `<link rel="icon">` 与页头 logo**、未知端点引导、POST 405 |
| B 没插 U 盘 | 只有 ext0 + ext1 | `/list` 返回 `count:0`，内部卷绝不进入可卸载列表 |
| C 卷被占用 | unmount 返回 EBUSY | `ok:false` + errno + 带下一步动作的提示 + `release_url`，占用者名单内联，`force=1` 确实把 `MNT_FORCE` 传下去 |
| D 挂载点极多 | 80 条假沙箱 + usb0/usb1 排在最后 | **U 盘没被上限藏住**、`count` 报真实总数 82、`truncated:true`、截断后仍能卸载 |
| E `--no-ui` | 同 A | 启动时**不**调浏览器接口、`ui_attempts:0`；`/openui` 手动触发仍生效 |
| F 系统调用被拒 | 桩件返回 -1 | `/diag` 如实报出失败码；服务本体与页面/列表不受影响 |
| G 占用发现与解除 | 假进程表含可杀与不可杀进程 | `/holders` 报出占用者（`init`、`SceShellCore`、本服务自身被正确挡在"可终止"之外）、`/release` 预览不动作、`confirm=1` 才真 TERM、解除后重试卸载成功 |
| H fd 表被拒 | `KERN_PROC_FILEDESC` 失败 | 如实报"查不了"而非谎报"没人占用"；`/release` 跳过终止、仍尝试挂接解除与 force 兜底，force 也失败才如实报 `unmounted:false` |
| I 本服务自身占用 | 自己在 fd 表里 | **绝不自杀**：只杀别的占用者，自己进 skipped，服务存活 |
| J 沙箱 nullfs 挂接占用 | usb0 被两条沙箱视图引用、无进程 fd | 卸载失败时 `holder_count:0` + "系统沙箱"提示 + `release_url`；`/release` **逐条卸掉两个挂接**、`child_released_count:2`、**卸载顺序正确（先挂接后目标）**、最终卸载成功 |
| K 卷标 | 5 张合成镜像挂在 usb2…usb7 | 四类镜像各自解出正确卷标；非 FAT 与读不到的**如实报空串而不是报错**；内部卷不去读设备；有卷标不影响卸载 |
| L 重复加载 | 同 A | **同版本**：新进程自己退出、日志说明是"激活"、**日志里没有第二次 `listening`**、原实例 pid 没被顶掉；**不同版本**：接管日志正确、新实例接管端口、旧实例真从进程退出、`ui_reason:"takeover"` |
| M `/shutdown` 鉴权 | 同 A | 非回环来源 + 无标识头 → 拒绝且服务仍在；**地址栏打开 `/shutdown` 被 302**；非回环 + `X-Requested-With: usbmanage` → 放行；回环来源不带标识头也放行 |
| N 界面语言 | 同 A | 服务端按 `Accept-Language` 判定：无头 / `en-US` / `ja-JP` → 英文；`zh-CN` / `zh-Hans-CN` → 简体；`zh-TW` / `zh-HK` / `zh-Hant` → 繁体。三种语言的正文互不残留别档文字（简体档里不出现繁体字，反之亦然）。`?lang=` 四个方向都能覆盖。屏幕通知按进程环境变量 `LANG` 出对应语言 |

> **场景 L 曾经红过一次，暴露的是真问题**：最初按"bind 失败 → 再请旧实例退役"来写，结果在 Windows 上旧实例根本没被请退——因为 `SO_REUSEADDR` 允许两个进程同时监听同一端口（与 BSD 语义相反）。改成 bind 之前主动请退，主机与真机行为才一致。

### 页面 JS 层（73）

前两层打的全是接口，**页面里的 JS 一行都没被执行过**，按钮被 `load()` 整表重建冲掉这类问题就是从那里漏的：服务端老老实实返回了 `holders` / `hint` / `release_url`，接口测试全绿，但页面 `renderHolders()` 把按钮 append 到卡片后紧跟一次 `load()`，而 `load()` 是 `box.innerHTML=html` 整表重建——按钮和占用者名单当场被冲掉。真机表现就是：顶部提示条让你"点解除占用并卸载"，页面上却根本没有那颗按钮。

这一套把 `web/index.html` 的 `<script>` 原文抽出来，喂给一个极简 DOM（`createElement` / `appendChild` / `innerHTML` / `querySelector` + 同步 `XMLHttpRequest` 桩），然后像用户一样点按钮，断言 **DOM 里真的出现了那颗按钮**。覆盖：

- 失败（EBUSY、查不到占用者）后卡片上出现「解除占用并卸载」按钮、挂载点正确、文案正确、卡片标红
- 无占用者时给出"多为沙箱 nullfs 挂接"的说明
- 顶部提示条报出失败原因并指明是哪个盘；提示条出现时给 `body` 让位（不再盖住标题）
- 点解除后确实打了带 `confirm=1` 的 `/release`；成功后该卷从列表消失
- 解除之后仍失败：保留重试按钮 + 列出残留占用者
- **反证**：`renderHolders()` 之后紧跟 `load()`，按钮必丢——证明这套断言真的能抓到该 bug
- 卷标显示：有卷标的卡片出现卷标行；没有卷标时明确写"（这块盘没有卷标）"而不是留空；点卸载时确认框里点名卷标
- 关闭服务：确实打了 `/shutdown`，且请求带 `X-Requested-With: usbmanage` 与 `Accept: application/json`；关闭后列表区换成"服务已关闭"、所有按钮禁用，**再点刷新也不会再发请求**（`dead` 标志短路）

> **版本号不写死在测试里**：`run.sh` 从 `usbmanage.c` 的 `#define VERSION` 现读后传给 `page_test.js`。此前测试里写死了版本，产品已经更新了它照样全绿——版本漂移必须由测试发现，而不是由人眼发现。

---

## 上机验收

宿主机测试证明不了系统调用在主机上的实际行为，必须在主机上做。

**第一步：看界面与启动通知。** 加载 payload 后，电视上应当自动打开浏览器并显示选择页，屏幕角落还会弹出一条系统通知（内容带管理地址 `http://<局域网IP>:9100`）。页脚版本号应显示 `v1.0.0`，页头左侧能看到应用图标、浏览器页签上也是它。若没有：

- 通知没弹但界面弹了 → 看 `curl -s "http://<PS5_IP>:9100/diag"` 的 `rc.notify` 与 `rc.knotify`：两个都非 0 = 都被主机拒；一个 0 一个非 0 = 其中一条通道在你这台机器上不被接受；两个都 0 但屏幕上就是没有 = 请求被接受了但主机没渲染，属主机侧行为，软件层面没有办法
- `rc.browser` 非 0 → 拉浏览器被主机拒绝，返回码即原因
- `ui_attempts: 0` → 根本没走到那一步（用了 `--no-ui`，或 payload 没起来）
- `procscan.supported` 为 false → 内核没放行 `KERN_PROC_FILEDESC`，占用者查不了，「解除占用并卸载」会自动跳过终止、走挂接解除 + 强制卸载兜底
- 页面本身打不开（9100 连不上）→ payload 没跑起来
- 页面能开但版本号不对 → 9100 上挂的是旧实例。工具箱加载新 payload **不会**踢掉旧实例，先点页面上的「关闭服务」把旧的关掉，再加载新的
- 自动弹不生效也不影响可用性：`python usbmanage.py ps5-ui` 或用手机/电脑开 `http://<PS5_IP>:9100/` 一样用

**第一步之二：验重复加载。** 在已经跑起来的情况下，再点一次工具箱的「加载」。期望：① 屏幕上再次弹出界面（这就是"激活"）；② `/diag` 的 `ui_reason` 仍是 `start`（端口上还是原来那个实例）；③ 主机「进程管理」里只有一个 `payload.elf`。若 `ui_reason` 变成 `takeover`，说明你重建过 elf、版本号变了——这是设计行为，`other_version` 会告诉你顶替的是哪个版本。

**第一步之三：验「关闭服务」。** 点页面上的「关闭服务」→ 确认。期望界面变成"服务已关闭"、所有按钮禁用；之后 `curl` 9100 应连不上。再从工具箱加载一次，应能正常起来（`ui_reason` 回到 `start`）。

**第二步：命令行核验。**

```bash
curl -s "http://<PS5_IP>:9100/version"     # 必须等于本地 elf 的版本，并带中英文名
curl -s "http://<PS5_IP>:9100/diag"        # rc 四个值；procscan.supported 决定占用发现可用与否
curl -s "http://<PS5_IP>:9100/list"        # 只列外接卷；看每条的 label 是不是这块盘的真名
curl -s "http://<PS5_IP>:9100/list?all=1"  # 内部卷应为 ejectable=false；看一下 count/shown
# 图标：应为 200 / image/png / 12863
curl -s -o /tmp/icon.png -w '%{http_code} %{content_type} %{size_download}\n' "http://<PS5_IP>:9100/icon.png"
curl -s "http://<PS5_IP>:9100/eject?mount=%2Fmnt%2Fext0"   # 必须被拒（顺带验证解码+白名单）
```

> 第四条若 `label` 是空串，先别急着当 bug——那可能是这块盘本来就没设卷标，用 Windows 在盘上"属性 → 重命名"设一个再插回来复测。第五条能下下来的那份可以直接和仓库里的 `web/icon.png` 比对（`cmp`），字节一致才说明 elf 里烧的是这一版的图。第六条即使被拒也什么都不会发生，可以放心执行。

**第三步：真卸载 + 占用解除。** 在界面上点「安全弹出」卸载一块 **U 盘**（不要点内部存储，界面里它们本来就是禁用状态）：

- 直接成功 → 返回已卸载提示，此时方可拔盘
- 失败（EBUSY）→ 被点的那张卡片上应当出现「谁在占用」区域和一颗「解除占用并卸载」按钮，**无论有没有查到占用进程，按钮都必须出现**，卡片同时标红。点它之后三种结果都是明确反馈：成功 / 部分成功（终止了 N 个进程、卸掉了 M 个挂接但仍失败）/ 失败（原因 + 残留占用者继续列出）
- 如果"没有程序在占用"却仍卸不掉，把 `/holders` 与 `/list?all=1` 的输出发出来——后者能看出是不是沙箱 nullfs 挂接在引用这块盘
- 解除成功时留意回执里的两点：`child_released_count`（卸掉了几个沙箱挂接，非 0 就实锤"没进程占用也 EBUSY"是 nullfs 造成的）、`forced_retry`（是否补了 `MNT_FORCE`）

**第四步：与官方样例交叉比对。** 用完全相同的编译参数编一遍 SDK 自带的 `samples/mntinfo`（官方枚举挂载点的示例，与本 payload 需求最接近），再并排比结构：

```bash
SDK=.../ps5-payload-sdk
clang ... "$SDK/samples/mntinfo/main.c" -o mntinfo-ref.elf   # 参数同 build.sh
llvm-readelf -h/-l/-d usbmanage.elf mntinfo-ref.elf
```

实测两者逐项相同：

| 项 | usbmanage.elf | mntinfo-ref.elf |
|---|---|---|
| Class / ABI / Type / Machine | ELF64 / FreeBSD / DYN / x86-64 | 同 |
| Entry point | `0x0` | `0x0` |
| NEEDED | libkernel_sys, libSceLibcInternal, libSceNet, libkernel_web | 同（顺序也同） |
| 程序头 | 3×LOAD(RWE/RW/RW, align 0x4000) + DYNAMIC | 同 |

也就是说这个产物在加载器看得见的每一个维度上都与官方样例一致；`unmount` / `getfsstat` / `socket` / `sync` 等都在动态符号表里，由主机的 4 个库在运行时解析。

---

## 部署到主机

主机上跑的是 **pldmgr 或 DB's 越狱工具箱**（`<PS5_IP>:7788`，底子为 pldmgr），它提供现成的 HTTP 接口，不需要 9021、不需要 elfldr、不需要 etaHEN。

### 工具箱在哪些位置"发现"elf

pldmgr **不是全盘扫描**，只认固定几处。按可靠度排：

| 位置 | 说明 | 依据 |
|---|---|---|
| `/data/pldmgr/payloads/<名字>/<文件>.elf` | 内部存储。工具箱自己的数据目录，它的上传接口就落在这里 | **本机实测**：`usbmanage.elf` 落在 `/data/pldmgr/payloads/usbmanage/usbmanage.elf` |
| `/mnt/usb0`…`/mnt/usb7` 下的 `pldmgr/` 子目录 | U 盘根目录建 `pldmgr` 文件夹，elf 丢进去，插盘即被发现，**不必复制到内部存储** | PLK 文档 + 社区教程（本机未复现） |
| `/data/ps5_autoloader/` | Y2JB autoloader 目录。配 `autoload.txt` 走开机自动加载 | PLK autoloader 文档 |
| `/data/etaHEN/payloads/` | etaHEN 的目录。本机没装 etaHEN，用不上——列出来只为避免混淆 | 官方教程 |

子目录是按 payload 名建的（`usbmanage.elf` → `usbmanage/`）。**文件名大小写敏感**。

**"发现"和"加载"是两个动作**：文件落到上面任一位置即可被"发现"（出现在列表里），真正执行要走 `GET http://<PS5_IP>:7788/loadpayload:<完整路径>`，界面上就是插件卡片的「加载」按钮。加载后进程名统一显示为 `payload.elf`，所以别靠进程名认自己，靠 9100 端口的 `/version`。

### payload 目录里那个 `.json` 是什么

```
/data/pldmgr/payloads/
├── usbmanage/
│   ├── usbmanage.elf            ← payload 本体
│   └── usbmanage.elf.json       ← 工具箱自动生成的元数据
└── …
```

**这个 `<文件名>.json` 是工具箱自己写的，不需要手工准备**（与 elf 同时间戳）。它的 `install_source` 区分三种来路：`web_upload`（网页界面上传）、`usb`（U 盘导入，另有 `install_source_detail` 记路径）、`repository`（云仓库下载，另有 url / version / checksum / category）。

它**不参与"发现"和"加载"**：`/list_payloads` 的数组是扫目录得到的，`meta` 只是附带的显示信息。文件在、目录对就能被发现；json 丢了顶多少几个字段。

可选美化（非必需，改前先备份）：上传进来的 payload，json 里的 `name` 默认等于文件名，界面上就照它显示。想让显示名干净、顺带带上说明，可以编辑：

```json
{
  "name": "PS5 USB管理器",
  "filename": "usbmanage.elf",
  "description": "PS5 USB管理器（usbmanage）：枚举 /mnt/usb* 并 sync + unmount，HTTP 9100",
  "version": "1.0.0",
  "downloaded_at": "…原值保持不变…",
  "install_source": "web_upload"
}
```

> `checksum` 建议留空——若被自动更新流程拿去校验，填错比不填更麻烦。

### 怎么把 elf 送进去（四种，任选）

| # | 办法 | 适用场景 | 落点 |
|---|---|---|---|
| 1 | **U 盘直放**：U 盘根目录建 `pldmgr/`，把 elf 放进去，插到 PS5 | 不碰 PC、不碰网络，最省事 | `/mnt/usb<N>/pldmgr/`，不用复制 |
| 2 | **工具箱网页界面**：`http://<PS5_IP>:7788` → 插件/管理页，浏览器选文件上传 | 有 PC 或 PS5 自带浏览器 | 工具箱自己决定（实测为 `/data/pldmgr/payloads/usbmanage/`） |
| 3 | **工具箱的 U 盘搬运**：界面里"U 盘文件"入口，或 `GET /usb_move_check?path=` + `GET /usb_move_perform?path=` | U 盘做中转，但要落到内部存储 | `/data/pldmgr/payloads/` |
| 4 | **FTP / 网页文件管理器**：ftpsrv（2121）或 web-file-mgr（8888） | 需要自己指定路径 | 手放到 `/data/pldmgr/payloads/usbmanage/`，文件名 `usbmanage.elf` |

> 办法 1 的前提是 U 盘是 PS5 认的文件系统（exFAT / FAT32）。注意**本工具的功能就是"卸载 U 盘"**，验证时别把放着 payload 的那块盘卸掉。

### web 文件管理器（8888）——另一条上传 + 启动通道

`ps5-web-file-manager` 跑在 8888，是自带上传/下载/编辑/启动的网页文件管理器。实测其后端接口（**路径一律不带前导 `/`**）：

| 接口 | 用途 |
|---|---|
| `GET /api/list?path=` | 列目录 |
| `GET /api/text?path=` | 读文本文件 |
| `GET /fs?path=` | 取文件原始内容 |
| `POST /api/download/prepare` → `GET /api/download?id=` | 下载 |
| `POST /api/upload/prepare` → `POST /api/upload-file` → `POST /api/upload/finish` | 上传 |
| `POST /api/launch-elf`（`path=`） | **直接启动一个 elf** |

也就是说，**不经过工具箱也能注入并启动**：把 elf 传到任意目录，再 `launch-elf`。代价是它不写 pldmgr 的元数据 json，所以"能在工具箱列表里看到"这件事仍要走工具箱上传。该 payload 不是常驻的：关掉或崩溃后 8888 就没人监听，需要重新加载它才回来。

### 主机侧确认（闭环）

```bash
curl -s "http://<PS5_IP>:7788/list_payloads"   # 列表里出现 usbmanage.elf 才算被"发现"

# 1) 上传（body 为 elf 原始字节）
curl -X POST --data-binary @usbmanage.elf \
  "http://<PS5_IP>:7788/manage:upload?filename=usbmanage.elf"

# 2) 确认落盘路径
curl -s "http://<PS5_IP>:7788/list_payloads"

# 3) 启动（路径以上一步返回的实际路径为准）
curl -s "http://<PS5_IP>:7788/loadpayload:/data/pldmgr/payloads/usbmanage/usbmanage.elf"
```

一条命令走完的等价写法：`python usbmanage.py --ip 192.168.1.100 ps5-deploy`。

启动后自检：

```bash
curl -s "http://<PS5_IP>:9100/version"        # 名称 + 版本 + 语言
curl -s "http://<PS5_IP>:9100/diag"           # UI 自启状态 + 系统调用返回码 + procscan
curl -s "http://<PS5_IP>:9100/list"           # 可卸载的外接卷（无 U 盘时应为 count:0）
curl -s "http://<PS5_IP>:9100/list?all=1"     # 全部挂载点，含内部卷与沙箱挂接（只读）
curl -s "http://<PS5_IP>:9100/holders?mount=%2Fmnt%2Fusb0"              # 谁在占用 usb0
curl -s "http://<PS5_IP>:9100/release?mount=%2Fmnt%2Fusb0"              # 预览解除动作（不动手）
curl -s "http://<PS5_IP>:9100/release?mount=%2Fmnt%2Fusb0&confirm=1"    # 真解除并卸载
curl -s "http://<PS5_IP>:9100/eject?mount=/mnt/usb0"                    # 字面量写法
```

> `ps5-deploy` 之后若 `/list` 返回 `count:0`、`/list?all=1` 只有内部卷，说明当前没插 U 盘——这是正常状态，白名单不会把内部卷列进来。

### 开机自动加载（可选）

越狱是 tethered 的，重启后 payload 全没了。除了每次跑 `ps5-deploy`，也可以在工具箱自己的界面上把它设为自动加载：

1. 打开 `http://<PS5_IP>:7788` → 左侧「自动加载」页
2. 从「可用插件」里找到 `usbmanage.elf`，加入列表（该页会把 `DELAY` 之类的等待标记转换成 payload 语法，建议直接用它而不是手改配置文件）
3. 「自动加载总开关」置为开

> 对应配置由 `/get_config` 读出、`/set_config` 写回（同一份 `pldmgr_config.txt`），字段为 `AUTOLOAD_ENABLED` 与 `AUTOLOAD_LIST`。**本项目不代写这两个字段**：`AUTOLOAD_LIST` 里的 `!` 前缀等语法由工具箱前端负责生成，手工拼写若出错会影响开机加载链，代价不对等。
>
> 另外工具箱建议"自动加载项目不要超过 4 项"，加之前先确认你这台机器上已跑的项数。

---

## 接口一览

payload 在主机 **9100** 端口提供 HTTP 服务。

| 方法 | 路径 | 作用 |
|---|---|---|
| GET | `/`、`/index.html` | 内置网页界面（页面与图标都烧在 elf 里） |
| GET | `/ping` | 存活探测，返回 `pong` |
| GET | `/version` | 名称（简体 / 繁体 / 英文）、版本、当前语言（`zh-Hans` / `zh-Hant` / `en`）。**重复加载时新实例靠这个接口认出自述是同版本的** |
| GET | `/list` | 枚举可卸载的 USB 卷：挂载点、容量、卷标 |
| GET | `/list?all=1` | 全部挂载点（只读诊断，`ejectable` 标明能否卸载） |
| GET | `/eject?mount=/mnt/usb0` | 卸载。失败时内联返回占用者与 `release_url`；加 `force=1` 用 `MNT_FORCE` 强卸 |
| GET | `/holders?mount=/mnt/usb0` | 谁在占用这个盘：pid / 进程名 / 打开的路径 / 能否安全终止 |
| GET | `/release?mount=/mnt/usb0&confirm=1` | 解除占用并重试卸载（TERM→KILL → 卸 nullfs 挂接 → sync + unmount）。**必须 `confirm=1` 才动手** |
| GET | `/procs` | 进程清单（只读诊断：pid / ppid / uid / 进程名 / 是否本服务自身） |
| GET | `/openui` | 让主机把界面弹到它自己的屏幕上，可从 PC/手机远程触发 |
| GET | `/diag` | 诊断：版本、端口、局域网地址、UI 自启状态、系统调用返回码、进程扫描是否可用 |
| GET | `/icon.png` | 应用图标（`/favicon.ico` 返回同一份字节） |
| GET | `/shutdown` | 从进程里彻底退出（页面「关闭服务」用它） |

启动参数：`usbmanage.elf [端口] [--no-ui] [--force]`。`--no-ui` 关掉"加载后自动弹界面"，只保留 `/openui` 手动触发。`--force` 跳过"已有同版本实例"的判定，强制起新进程并接管端口——**只给宿主机测试用**，真机上会得到两个抢同一端口的实例。

浏览器直接访问 `/list` 或 `/eject` 会 302 跳回 `/`（判据：请求头 `Accept` 含 `text/html`）。页面自己的 XHR 显式带 `Accept: application/json`。想要 JSON 就用 `curl`。这个副作用的另一面是好的：`/eject` 不可能被"地址栏回车"误触发，只能从界面点。

`/diag` 返回示例：

```json
{"name":"PS5 USB管理器","name_hant":"PS5 USB管理器","name_en":"PS5 USB Manager",
 "version":"1.0.0","lang":"en","port":9100,
 "lan_ip":"192.168.1.100","ui_autolaunch":true,"ui_attempts":1,
 "ui_reason":"start","other_version":"",
 "rc":{"user_init":0,"browser":0,"notify":0,"knotify":0},
 "procscan":{"supported":true,"procs":42}}
```

`rc` 里 `-999` 表示"未尝试"，其它非 0 值即主机给出的失败码。`rc.notify` 是 `sceNotificationSend` 的返回码，`rc.knotify` 是 `sceKernelSendNotificationRequest` 的——**哪个通道真机不显示就看这两个数**。`procscan` 报告进程扫描（`sysctl KERN_PROC_FILEDESC`）是否可用。

`ui_reason` 说明这次界面为什么被拉起来：

| 值 | 含义 |
|---|---|
| `start` | 正常启动 |
| `same-version` | 重复加载：已有同版本实例在跑，本进程只是把界面叫出来然后自己退出 |
| `busy` | 端口上有个连上却不出声的东西，判定为"有实例"，同样只激活 |
| `takeover` | 端口上是**别的版本**，本进程请它退役后接管；`other_version` 是被顶替的那个版本 |

`/list` 返回示例（真机实测值）：

```json
{"count":2,"scope":"usb","volumes":[
  {"mount":"/mnt/usb0","device":"/dev/da2p1","fstype":"exfatfs","ejectable":true,
   "label":"移动硬盘",
   "bsize":131072,"blocks":16776673,"bfree":1022447,
   "total":2198952083456,"free":134014173184},
  {"mount":"/mnt/usb1","device":"/dev/da2p2","fstype":"exfatfs","ejectable":true,
   "label":"",
   "bsize":131072,"blocks":21376583,"bfree":8941511,
   "total":2801871486976,"free":1171981729792}
],"shown":2,"truncated":false}
```

`count` 是**真实匹配总数**，`shown` 是本次实际列出的条数，`truncated` 标出两者是否不等。分开报是因为真机挂载点极多（实测 80+ 条），诊断视图 `/list?all=1` 会被 `MAX_VOLUMES`（64）或响应缓冲截断；如果只报"写入条数"当总数，就会出现"主机明明有 U 盘、接口却说 count 只有几十"的假象。`/list`（只列 `/mnt/usb*`）条数极少，不受截断影响——这一点由测试场景 D 守住：82 条挂载点、U 盘排在最后，仍必须被列出并能卸载。

`/eject` 成功与失败（卷被占用）的返回：

```json
{"ok":true,"mount":"/mnt/usb0","forced":false,"msg":"已落盘并卸载，现在可以安全拔出了"}
```

```json
{"ok":false,"code":16,"mount":"/mnt/usb0","forced":false,"busy":true,
 "msg":"卸载失败：Resource device。谁在占用见列表，点「解除占用并卸载」自动处理",
 "holders_supported":true,
 "holders":[{"pid":99,"comm":"web-file-mgr.elf","fd":12,"kind":"fd",
             "path":"/mnt/usb0/movie.mkv","self":false,"killable":true},
            {"pid":10,"comm":"SceShellCore.elf","fd":5,"kind":"fd",
             "path":"/mnt/usb0/b","self":false,"killable":false,
             "why":"Sony 系统组件（Sce* 前缀，不代为终止）"}],
 "holder_count":2,
 "release_url":"/release?mount=/mnt/usb0&confirm=1"}
```

查不到占用进程时，msg 与 `hint` 会说明原因（fd 表被拒 / 占用来自沙箱挂接），**无论哪种情况「解除占用并卸载」都可用**——`/release` 会跳过终止、直接解除系统挂接并强制卸载。`holder_count:0` 不等于"可以再试一次"。

`/release` 解除成功时：

```json
{"ok":true,"mount":"/mnt/usb0","confirmed":true,"before":2,"scanned":42,
 "holders":[…],"killed":[{"pid":99,"comm":"web-file-mgr.elf","sig":"TERM"}],
 "skipped":1,"after":0,"still":[],
 "child_released":[{"mount":"/mnt/sandbox/CUSA12345_000/mnt/usb0","ok":true,"err":0}],
 "child_released_count":1,"unmounted":true,
 "msg":"已解除占用并卸载（已终止占用进程）（已解除系统内部挂接），现在可以安全拔出了"}
```

`child_released` 是**源指向这块盘的 nullfs 挂接被逐个卸掉**的回执。应用沙箱把 U 盘挂进 `/mnt/sandbox/<TITLEID>_000/...` 这类引用，不属于任何进程（fd 表查不到人），`unmount` 目标卷时却是 EBUSY——真机上"没有程序在用却卸不掉"多半就是它。最后一步常规 `unmount` 失败时自动补 `MNT_FORCE`（回执带 `"forced_retry":true`），每一步成败都如实回报。

> 查询串里的 `%2F` 会先解码再进白名单。Python 的 `urlencode` 默认把 `/` 编码成 `%2F`，早前主机按字面量比对，合法请求被当成非法路径拒掉、表现为"点了卸载没反应"。现在主机端解码后再判，PC 端也用 `safe="/"` 保留斜杠，两层互不依赖。

---

## 单实例（重复加载与接管）

工具箱加载 payload 时有两个既定行为，直接影响升级：**每个 payload 的进程名都叫 `payload.elf`**（靠进程名分辨不出哪个是我们），而且**加载新 payload 不会杀掉上一个实例**（界面里也写着"请勿重复加载插件"）。于是升级时会撞上：新实例启动 → `bind(9100)` 失败 → 旧实例仍在服务 → 部署"看起来成功了"，实际版本根本没换。

现在的判定（`probe_instance()`，超时 600ms）：

| 端口上的情况 | 动作 |
|---|---|
| 没人监听 | 正常启动 |
| `/version` 自述是 usbmanage、**且版本相同** | **只把界面叫到屏幕上，本进程随即退出**（进程数保持 1） |
| `/version` 自述是 usbmanage、但**版本不同** | 向它发 `/shutdown` 请其退役，等 500ms 后接管端口 |
| 连上了但不出声 | 当作"有实例"，同样只激活、不起第二个 |
| 出的是别人的东西 | 报错退出，**不抢别人的端口** |

两处必须靠探测而不是靠进程名：

- 工具箱里所有 payload 的进程名都是 `payload.elf`，"查进程名里有没有自己"这条路走不通，只能靠端口 + `/version` 自述。
- **接管必须在 bind 之前主动做**，不能等 bind 失败再补救：Windows 的 `SO_REUSEADDR` 允许两个进程同时监听同一端口（与 BSD 相反），等 bind 报错时旧实例还活着。主动请退在 FreeBSD 和 Windows 上行为一致。

> 旧实例是更早版本（没有 `/shutdown`）时接管会失败并打印提示，手动把它退掉即可——这种只可能出现在升级路径上。
>
> 另外记一条：**删除主机上的 elf 文件不等于杀掉正在跑的进程**。旧实例只要还活着，就会继续占着 9100。

---

## 卷标解析

`label` 为空字符串表示这块盘**没有卷标**（或卷标读不出来），不是接口出错。这只是"帮人别卸错盘"的辅助信息，**绝不影响能否卸载**。

`/mnt/usb0` → `/dev/da2p1` 这种映射本身不带任何可读含义，而 PS5 上没有弹出入口、拔错盘的代价是文件系统损坏。卷标是唯一一眼能认的标识，所以只能直接读盘的引导扇区——`struct statfs` 里只有 `f_mntonname` / `f_mntfromname` / `f_fstypename`，**没有卷标字段**。

| 文件系统 | 卷标在哪 |
|---|---|
| exFAT | 引导扇区 `"EXFAT   "` 标记 → 簇堆偏移(`0x58`) + 根目录簇(`0x60`)，从根目录里找 `EntryType 0x83` 的项（`bit7`=在用，第 2 字节是字符数），名字是 UTF-16LE |
| FAT32 | FATSz16(`0x16`)=0 且根目录项数(`0x11`)=0 → 数据区 = `(保留扇区 + FAT数×FAT大小 + 根目录扇区) × 每扇区字节数`，根目录首簇在 `0x2C`；先按 `0x0F` 长名项拼回完整名字，再退回 8.3 短项的 `0x47` 字段 |
| FAT12/16 | 根目录是固定区，8.3 卷标在引导扇区 `0x2B` |

PS5 的外接存储只认 exFAT 与 FAT12/16/32，所以只实现了这三种。内部卷（`/system`、`/user`、`/mnt/sandbox/**`）根本不去读设备——它们既不是外接盘、也没有卷标，去读只会白白多一份访问风险，`label` 只对 `/mnt/usb<数字>` 填。

两处细节：**8.3 短字段只接受纯 ASCII**，中文卷标在这段字段里是 GBK 字节，原样输出就是乱码，遇到非 ASCII 一律放弃、转去根目录找长名项（`0x0F`）；`"NO NAME"` 是全空格的 Windows 占位写法，同样按没有处理。读盘用 `fopen`/`fread`/`fclose` 而不是 `open`/`read`——宿主机编译时 `close()` 被换成了 socket 版本，CRT 文件描述符与 socket 句柄不同域。

---

## 应用图标

浏览器页签上不再是默认的问号，页头左侧也是同一张图。

| 接口 | 返回 |
|---|---|
| `GET /icon.png` | 96×96 PNG，12863 字节，`Content-Type: image/png` |
| `GET /favicon.ico` | **同一份字节**。浏览器按内容嗅探，PNG 放在 `.ico` 路径上没问题；老浏览器不认 `<link rel="icon">` 时的回落就是这条路 |

与页面一样，图标是编进 elf 的（不读外部文件、不联网）：`web/icon.png` 经 `xxd` 转成 `web/icon_png.h` 参与编译。页面里引用两处：

```html
<link rel="icon" type="image/png" href="/icon.png">        <!-- head -->
<img class="logo" src="/icon.png" width="40" height="40">  <!-- 页头 -->
```

图标主文件与各尺寸的来源、规格（RGBA、四角透明、约 4% 透明外边距、约 20% 圆角）见 [`assets/icon/README.md`](../assets/icon/README.md)。同一个 96×96 文件同时供 PS5 页面与 PC 控制台使用，是页面里 32–40 px 显示尺寸的 2–3 倍图。页头 logo 直接引 `/icon.png`，不要内联 base64——页面字节会白涨一个数量级。

> **换图标必须重跑 `bash build.sh`**：图标被烧进 elf，不重新构建的话 elf 里还是旧图。`build.sh` 会在结尾自查两件事——生成数组的长度与 `web/icon.png` 一致、elf 里能找到 PNG 的 `IHDR` 标记（防"数组没人引用被链接器丢掉"，只查长度发现不了这种）。

---

## 安全边界

判据只有一条，而且是正向的：只放行 `/mnt/usb` 加数字，其余一律拒绝。

不按名字猜，是因为 PS5 内部存储的挂载名会随主机状态变。2026-10-07 真机 `/list?all=1` 实测（80 条以上挂载点）长这样：

| 类 | 挂载点 / 设备 | 性质 |
|---|---|---|
| 外置 USB | `/mnt/usb0` = `/dev/da2p1`、`/mnt/usb1` = `/dev/da2p2`（exfatfs，2.2 TB / 2.8 TB） | **只有这个前缀能卸载** |
| 内部 | `/` = `md0`(exfatfs)、`/system` = `/dev/ssd0.system`、`/system_ex`、`/system_data`(ufs)、`/user`(bfs)、`/update`、`/preinst` | 主机内部存储 |
| 内部（挂接） | `/data` = nullfs→`/user/data`、`/original_user`、`/original_system_data`、`/devlog/*` | 别名/挂接，不是独立卷 |
| 系统 | `/dev`(devfs)、`/system_tmp`(tmpfs)、`/mnt`(tmpfs) | 伪文件系统 |
| 沙箱 | `/mnt/sandbox/<TITLEID>_000/...`（一大票 nullfs） | VSH 应用的沙箱视图 |

而更早某次会话里枚举到的却是 `/mnt/ext0`(ufs)、`/mnt/ext1`(bfs)，**`/mnt/ext*` 在今天这台机器上一条都没有**。也就是说内部存储的挂载名随主机状态变化，靠枚举名字判断迟早出事——`/mnt` 本身是 tmpfs，`/mnt/sandbox/**` 是沙箱的 `nullfs` 视图，用"以 `/mnt` 开头"这种粗判据会把它们一起放过去。卸载前还会再确认一次挂载点存在。

> 踩过的坑：开发期第一版白名单里写了 `/mnt/ext*`，而无 U 盘时枚举恰好只返回内部卷——一旦触发"全部卸载"就会去卸主机内部存储。现已收紧为 `/mnt/usb<数字>`，全量视图挪到只读的 `/list?all=1`。

其他几条：

- 卸载固定走 `sync` → `unmount` → 回读校验，三步缺一不可。exFAT 只是把出错代价降下来，替代不了卸载
- 「解除占用」只处理 `fstypename == "nullfs"` 且来源是目标挂载点的挂接，不碰别的
- 解除占用会终止别的进程（先 TERM 给它收尾的机会，稍候仍占用才 KILL）。护栏挡在三处，绝不碰：**本服务自身、pid≤1 的内核/init、`Sce*` 前缀的 Sony 系统组件**。宁可留下一个解除不掉的让用户自己处理，也不误杀系统关键进程
- PS5 侧接口没有鉴权（`/shutdown` 除外），只在可控局域网里用，**不要暴露到公网**
- `/shutdown` 只接受两类请求：回环来源（供新实例接管），或带着 `X-Requested-With: usbmanage` 的页面请求。CSRF 和地址栏手滑都挡在外面

---

## 名称约定

| | 名称 | 用在哪 |
|---|---|---|
| 中文显示名 | **PS5 USB管理器** | 简体中文界面标题、屏幕通知文案、`/version` 的 `name` 字段——给人看的地方 |
| 繁体显示名 | **PS5 USB管理器** | 繁体中文界面与通知、`/version` 的 `name_hant` 字段。目前与简体同字（管理器三字简繁一致），单列一个字段是为了将来真要分岔时只改一处 |
| 英文显示名 | **PS5 USB Manager** | 英文界面与通知、`/version` 的 `name_en` 字段——给人看的地方 |
| 机器标识 | **usbmanage** | payload 文件名 `usbmanage.elf`、工具箱里的插件目录名、JSON 的 `app` 字段、CSRF 头 `X-Requested-With: usbmanage`——给机器看的地方 |

**显示名与机器标识不要混改。** 改显示名（界面文案）是安全的；改 `usbmanage` 这个标识会打断工具箱的扫描路径、部署脚本、CSRF 头与既有文档，纯风险零收益。还有一个更隐蔽的后果：`probe_instance()` 靠 `/version` 里的机器标识认自己，标识一改，**重复加载会把自己的旧实例当成"别人的程序"而拒绝启动**（改名时踩过）。

---

## 设计要点（择要）

- 显示名和机器标识是两回事。界面与通知用 `PS5 USB管理器` / `PS5 USB Manager`；payload 文件名、JSON 的 `app` 字段、CSRF 头固定用 `usbmanage`。改显示名时别顺手把标识也改了，工具箱的扫描路径和部署脚本都认这个串。
- 单实例靠"端口 + `/version` 自述"判断，不靠进程名：工具箱里所有 payload 的进程名都叫 `payload.elf`，按名字分不出来。接管必须在 `bind` 之前主动做，因为 Windows 的 `SO_REUSEADDR` 允许两个进程同时监听同一端口（跟 BSD 相反），等 `bind` 报错的时候旧实例还活着。
- 界面语言在服务端定。读请求头里的 `Accept-Language`，页面里另留一份判定兜底。三种语言（简 / 繁 / 英）的词表分别放在 `usbmanage.c` 的 `T()` 调用与 `web/index.html` 的 `L` 表里，**两边都要改**，漏一边就会出现"标题换了、正文没换"。屏幕通知发出时还没有任何 HTTP 请求可读，只能退回读环境变量 `LC_ALL` / `LC_MESSAGES` / `LANG`。
- 卷标从引导扇区直接读。过两道门：`55 AA` 签名，以及头字节是一条跳转指令。FAT32 靠 `FATSz16 == 0 && 根目录项数 == 0` 认。8.3 字段里出现非 ASCII（中文在那是 GBK）就放弃，转去找长名项。
- `sha256` 不可复现。链接器每次会写一个随机 `build-id`，所以它只能当传输完整性校验，不是可复现构建的凭据。
- 自动弹界面走的是 Orbis 官方接口，不是工具箱的什么机制——工具箱本身没有"弹插件窗口"这回事。SDK 里带着 `samples/browser` 与 `samples/notify` 两个样例，社区里"加载后自己弹出东西"的 payload 走的都是这条路：主机拉起内置浏览器指向本服务，再弹一条屏幕通知。
- 界面是烧进 elf 的。页面源码在 `web/index.html`，构建时由 `xxd` 转成 C 字节数组（`web/index_html.h`）编进 elf，图标（`web/icon.png` → `web/icon_png.h`）走同一套机制。改页面只需改 `web/index.html` 再 `bash build.sh`；不依赖外部文件、不依赖联网。
- 页面里的 JS 只用 ES5（XHR，无 Promise / 箭头函数 / 模板串），以便在 PS5 自带浏览器这类老旧环境里也能打开。
- 页面的 XHR 要显式声明两个头：`Accept: application/json` —— 否则会被判成"浏览器导航"而 302 回首页；`X-Requested-With: usbmanage` —— `/shutdown` 的放行凭据之一，从 PC 浏览器点「关闭服务」时来源不是回环地址，没有这个头会被主机拒绝。
- 就地反馈要过一道 `pending`：`load()` 是拿 `innerHTML` 重建整张列表的，任何直接 append 到卡片上的节点都会在下次刷新时消失。所以卸载/解除的结果先写进 `pending`，`load()` 每次重绘完由 `applyPending()` 重新挂回对应卡片。改页面逻辑时**不要**在 `renderHolders()` 后面直接跟 `load()`。

---

## 目录结构

```
usbmanage/
├─ LICENSE                         GPL-3.0 全文
├─ README.md / README.en.md        使用者文档（中文 / 英文）
├─ CHANGELOG.md                    版本记录
├─ THIRD-PARTY-NOTICES.md          第三方组件与许可依据
├─ config.example.json             配置模板（config.json 含内网 IP，不入库）
├─ usbmanage.py                    PC 侧 CLI 入口
├─ server.py                       本地 Web 控制台后端（仅标准库）
├─ web/                            PC 控制台页面
├─ assets/icon/                    全平台应用图标定稿
├─ usbmanage/backends/
│  ├─ ps5.py                       PS5 后端（走工具箱 7788 上传 + 9100 控制）
│  └─ local_win.py                 Windows 本机可移动磁盘
├─ device/ps5-usbmanage/           ★ PS5 payload
│  ├─ usbmanage.c                  源码
│  ├─ web/index.html               内置网页界面（构建时 xxd 编进 elf）
│  ├─ web/icon.png                 内置页面图标（同样编进 elf）
│  ├─ build.sh                     编译脚本（Git Bash / Linux / macOS）
│  ├─ hosttest/                    三层宿主机测试
│  └─ dist/                        交付件：usbmanage-v<版本>.elf + SHA256SUMS.txt
└─ docs/
   ├─ DEVELOPMENT.md               开发文档（本文件）
   ├─ DEVELOPMENT.en.md            开发文档，英文
   ├─ images/                      文档用界面截图
   ├─ 根因分析与方案设计.md          根因分析与方案设计（源）
   └─ analysis.html                同上，HTML 版
```

`docs/images/` 里的截图不是手绘示意图，是把 `device/ps5-usbmanage/web/index.html` 用真实浏览器渲染出来的（mock 一份与本文档示例同源的 `/list`、`/eject` 响应，页面代码一行没改）。**改了页面记得重出截图。**

---
