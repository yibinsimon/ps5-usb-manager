# 第三方组件与许可 · Third-Party Notices

本文件记录本项目使用、链接或依赖的第三方组件及其许可，并说明它们对**本项目自身许可**的影响。

结论先写：**本项目采用 GPL-3.0-or-later**。理由见第 1 节，那是唯一需要认真看的一节。

---

## 1. ps5-payload-sdk —— 决定了本项目的许可

| 项 | 内容 |
|---|---|
| 项目 | `ps5-payload-sdk`（ps5-payload-dev / John Törnblom） |
| 用途 | **编译期 + 链接期依赖**。提供 PS5 (Prospero) 的交叉编译 sysroot、启动代码、libc 实现与系统库 stub |
| 许可 | **GPL-3.0-or-later**（`include/freebsd` 目录下的文件为 BSD 许可） |
| 是否随本项目分发 | 否。本仓库**不包含** SDK 的任何文件；但**构建产物包含其编译结果** |

官方 README 的 License 一节原文：

> Files in the folder include/freebsd are licenced under BSD licences.
> Unless otherwhise explicitly stated inside a file, the rest are licensed under the GPLv3+.

来源：<https://github.com/ps5-payload-dev/sdk> → `README.md` → *License*。

> SDK 源码中不带文件级许可声明的示例一律沿用 GPLv3+，例如
> `samples/hello_world/main.c` 头部即为
> `Copyright (C) 2023 John Törnblom ... GNU General Public License ... version 3, or (at your option) any later version`。

### 1.1 为什么本项目必须是 GPLv3+

**因为它不是"使用"，而是"把 SDK 的编译结果逐字复制进了产物"。**

本项目构建出的 `usbmanage.elf` **静态链接**了 SDK 提供的预编译目标文件。
这不是推测，以下为实测证据（`llvm-nm` 直接读符号归属）：

**(a) `target/lib/crt1.o`（125,248 字节的预编译目标文件）——被链入**

该文件本身定义了下列符号，而它们在 `usbmanage.elf` 中**同样以已定义符号（`T`）出现**：

```
__crt_start              __crt_syscall           __crt_syscall_init
__kernel_init            __klog_init             __patch_init
__dlopen   __dlsym   __dlclose   __dlerror   __dladdr
__rtld_init __rtld_find_file __rtld_lib_open __rtld_lib_close
__rtld_lib_addr2lib __rtld_sprx_init __rtld_payload_init ...（整套运行时链接器）
SHA1Transform
KERNEL_ADDRESS_* / KERNEL_OFFSET_*（内核偏移表，共 29 项）
```

即：SDK 的**启动代码、运行时链接器（rtld）、dlopen/dlsym 实现与内核偏移表**，
原封不动地进了我们的二进制。

**(b) `target/lib/libc.a` —— 被链入**

该归档含 **71 个真实实现目标文件**（非空壳），如 `gmtime.o`、`dlfcn.o`、`arc4random.o`、
`call_once.o`、`emutls.o` 等。实证：

```
$ llvm-nm -A --defined-only target/lib/libc.a | grep __secs_to_tm
.../libc.a:gmtime.o: 0000000000000000 t __secs_to_tm
```

而 `__secs_to_tm` 同时以已定义符号存在于 `usbmanage.elf` 中。

**(c) 为什么不适用 GPLv3 的例外条款**

GPLv3 第 1 节中「系统库（System Libraries）」的例外，限于
**构成"主要组件（Major Component）"正常打包形式的组成部分**（如随操作系统内核、
随编译器分发的运行时）。`crt1.o` 与 `libc.a` 属于 **ps5-payload-sdk** 这一独立项目，
既不是 clang 的一部分（本项目用的编译器是 llvm-mingw 的 clang，Apache-2.0 with LLVM
Exception，与此无关），也不是主机操作系统的组成部分。SDK **未提供任何链接例外
（linking exception）**，因此该例外不适用。

**综上所述**：`usbmanage.elf` 是 GPLv3+ 代码的衍生作品，分发该二进制即须以 GPLv3+ 授权。
本项目据此采用 **GPL-3.0-or-later**。

> 若确实需要 MIT 或其它宽松许可：唯一干净的做法是**不再静态链接 SDK 提供的启动代码与 libc**，
> 即自行提供 `_start` / syscall 封装并改链系统的 `libSceLibcInternal`。
> 这是一项独立的重构工作，不是改个 LICENSE 文件就能解决的事。

### 1.2 分发构建产物时的对应源（GPLv3 §6）

GPLv3 要求：分发二进制时，接收者必须能拿到它的**完整对应源代码**
（Corresponding Source）。这一条在本文档里写清楚，而不是含糊带过：

| 组成部分 | 对应源在哪 |
|---|---|
| 本项目自己的代码 | **本仓库**。`device/ps5-usbmanage/usbmanage.c`、`web/index.html`、`web/icon.png`、`build.sh` 就是生成 `usbmanage.elf` 的全部输入，没有未列出的私有文件 |
| SDK 的启动代码与 libc | **上游 `ps5-payload-sdk`**。本仓库不含其源码，也**未做任何修改**——构建脚本直接用 SDK 目录里预编译好的 `crt1.o` / `libc.a` |

因此，拿到本项目二进制的人若认为本仓库的源码不足以重建它，可以：
对 SDK 那部分到 <https://github.com/ps5-payload-dev/sdk> 取**同一版本**的源码，
或联系本项目作者索取。用下面的指纹确认"是哪一份 SDK"：

```
sha256  e13ce68fcd8525df4b480a37b6239827e30998284781f4dd0cf37ec06717371a  target/lib/crt1.o
sha256  17b22ab7baec2107ee4ce4189257abbd773a9bb9ce854203e08e0581e10b72b3  target/lib/libc.a
```

（`crt1.o` 125,248 字节；`libc.a` 305,114 字节，内含 71 个目标文件。）

> 这里用指纹而不是版本号：SDK 的发布包里不带版本文件、也不带 LICENSE 文本，
> 写在文档里的"版本号"无从核对；指纹是能实打实比对的东西。
> 重新构建 payload 时，`build.sh` 的产物自校验会报出 ELF 里的 build-id——
> 那是**每次链接都变的**随机值，与 SDK 版本无关，不要拿它当版本凭据。

---

## 2. 编译与测试期依赖（不随本项目分发）

下列组件只在**构建或测试**时使用，其代码**不以任何形式进入** `usbmanage.elf` 或本仓库的产物。

| 组件 | 用途 | 许可 |
|---|---|---|
| **LLVM / Clang**（经 [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 分发） | 交叉编译、汇编、链接（`clang` / `ld.lld` / `llvm-readelf` / `llvm-nm` / `llvm-ar`） | Apache License 2.0 with LLVM Exceptions |
| **llvm-mingw** | Windows 上的 LLVM 工具链打包 | Apache License 2.0 with LLVM Exceptions（`LICENSE.TXT` 已随包分发） |
| **GNU Make / Bash / xxd / curl** | 构建脚本与宿主机测试 | 各自的上游许可 |
| **Node.js** | 仅 `hosttest/web/run.sh` 需要（在极简 DOM 上跑页面脚本） | MIT |
| **Python 3** | PC 侧工具（`usbmanage.py` / `server.py`） | PSF License。**仅使用标准库**，无第三方包 |
| **FreeBSD 头文件**（经 SDK 的 `include/` 提供） | 编译期头文件（`<sys/mount.h>` 等） | BSD（2-clause / 3-clause），见 SDK 声明 |

---

## 3. 本项目自身的许可

| | |
|---|---|
| 许可 | **GNU General Public License, version 3 or later** |
| SPDX | `GPL-3.0-or-later` |
| 全文 | [`LICENSE`](LICENSE) |
| 版权 | `Copyright (C) 2026 YibinSimon` |

```
This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
```

### 3.1 覆盖范围，以及两处值得知道的取舍

**覆盖范围**：`LICENSE` 是 GNU GPLv3 的**逐字全文**，适用于本仓库的全部文件——
payload（C 侧）、内置网页、PC 侧 Python 工具、文档与图标。整个仓库一个许可，
最简单，也不会有"这个文件算哪一档"的争议。

两处取舍：

1. **PC 侧 Python 工具本来可以不跟 GPL。** `usbmanage.py` / `server.py` / `usbmanage/*.py`
   不链接任何 GPL 代码，法律上完全可以单独给 MIT/BSD。没拆开的原因只有一个：
   一仓库一许可，读者不必去翻逐文件声明。若希望 PC 侧更宽松（比如公司内部分发更省事），
   给那几个文件加 `SPDX-License-Identifier: MIT` 头即可，与 payload 的 GPLv3+ 并不冲突。
2. **GPLv3 §6 的"安装信息（Installation Information）"条款只针对 User Product**——
   即为消费用途出售、且受技术措施限制的设备。本项目不分发任何硬件，这条不适用。

> 逐文件加 `SPDX-License-Identifier: GPL-3.0-or-later` 与 `Copyright (C) 2026 YibinSimon`
> 是 GPL 项目的常规做法，好处是文件被单独拿走时授权信息仍跟着它走。本仓库目前只在
> README 与本文档里集中声明；要逐个补上是机械改动，不影响任何行为。

---

## 4. 商标

「PlayStation」「PS5」「PlayStation 5」是 **Sony Interactive Entertainment Inc.** 的商标或注册商标。
本项目**非官方**，与 Sony Interactive Entertainment Inc. 无任何关联，未获其授权、赞助或认可。
名称中提及 PS5 仅为**说明兼容性**（nominative / descriptive use），不表示任何形式的背书。

实际做法上还有两条：界面、页面与应用图标里**不使用** Sony 的任何标志、字体或配色，
图标是本项目自己画的（见 `assets/icon/README.md`）。

> 名称里带商标，比自己写一段免责声明风险更高一档：它是"在产品名里使用他人商标"。
> 本项目是非商业的个人工具，按说明性使用处理足够；**若要商用**，建议把名称改成
> 纯描述性的写法（例如 `USB Storage Manager for PS5`），或者事先取得授权。

其余商标归各自所有者所有。

---

*本文件中的许可判定基于对本机实际安装的 `ps5-payload-sdk` 二进制的检查（2026-10-08）。
若上游 SDK 更改了许可或提供了链接例外，本结论应重新评估。*
