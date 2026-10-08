# 变更记录 · Changelog

本文件记录值得写下来的变更。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循[语义化版本](https://semver.org/lang/zh-CN/)。

---

## [1.0.0] — 2026-10-08

首个发布版。

### 功能

- **列出可卸载的外接存储**。`GET /list` 只放行 `/mnt/usb<数字>`；`GET /list?all=1`
  另给一份全部挂载点的只读诊断视图。
- **安全卸载**。`GET /eject?mount=/mnt/usb0`，固定走 `sync` 落盘 → `unmount` → 回读校验。
  若因占用失败，响应里直接带上"谁在占用"。
- **占用查询与解除**。`/holders` 列出占用该卷的进程；`/release` 终止占用进程、
  卸掉应用沙箱的 nullfs 挂接后重试卸载，常规卸不掉自动补 `MNT_FORCE`。
- **内置网页界面**。`GET /`，页面与图标都编进 elf，不依赖外部文件、不依赖联网。
  加载后由主机自己打开界面，并弹一条带局域网地址的系统通知。
- **界面三语**。简体中文 / 繁体中文 / 英文。系统语言是中文就出对应中文（简体走简体、
  繁体走繁体），其余语言一律出英文；`?lang=` 可显式指定。
- **单实例**。重复加载不会堆积进程：端口空闲则启动，同版本只把界面叫出来，
  版本不同才接管，端口上是别的程序则明确报错退出。
- **关闭入口**。页面上的「关闭服务」（`/shutdown`）让进程从主机上彻底退出。
- **卷标显示**。内核 `statfs` 不带卷标，所以直读引导扇区解析 exFAT / FAT32 / FAT12-16 的卷标。
- **诊断**。`/diag` 报出端口、局域网地址、单实例判定结果与各系统调用的返回码。

### 文档

- 使用者文档：`README.md` / `README.en.md`——快速开始、界面（含截图）、特性、由来、
  使用注意、安全边界、免责与许可。
- 开发文档：`docs/DEVELOPMENT.md` / `docs/DEVELOPMENT.en.md`——构建、测试、上机验收、
  部署到主机、接口一览、单实例、卷标解析、设计要点与目录结构。
- 第三方组件与许可依据：`THIRD-PARTY-NOTICES.md`。

### 许可

GPL-3.0-or-later。本 payload 静态链接了 `ps5-payload-sdk` 的启动代码与 libc 实现
（`crt1.o`、`libc.a`），该 SDK 为 GPLv3+ 且不带链接例外，因此构建产物必须是 GPLv3+。
判定依据与实证见 [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md)。

### 测试

宿主机上三层回归，共 471 项断言，全部通过：

| 层 | 断言数 |
|---|---|
| `hosttest/run.sh`（纯逻辑） | 139 |
| `hosttest/web/run.sh`（页面 JS） | 73 |
| `hosttest/http/run.sh`（HTTP 端到端） | 259 |

[1.0.0]: https://github.com/yibinsimon/ps5-usb-manager/releases/tag/v1.0.0
