"""后端：PS5（越狱固件）USB 卷枚举与安全卸载。

本后端与主机通信走**两条独立通道**，都不依赖 9021 端口的 elfldr：

1) 部署通道（越狱工具箱，默认 7788）
   用户主机上运行的是 "DB's 越狱工具箱"（底子为 pldmgr），提供：
     - GET  /list_payloads                    列出已装 payload 的完整路径
     - POST /manage:upload?filename=<name>    上传 elf（body 为原始字节）
     - GET  /loadpayload:<payload完整路径>      执行该 payload
   用它把自研的 `usbmanage.elf` 传到主机并启动。

2) 运行通道（usbmanage 服务，默认 9100）
   `usbmanage.elf` 在主机侧常驻，提供：
     - GET /                                  内置选择页（HTML）
     - GET /list                              枚举所有 USB/外接挂载卷
     - GET /eject?mount=/mnt/usb0             sync + unmount 指定卷（失败时内联返回占用者）
     - GET /holders?mount=/mnt/usb0           谁在占用该卷
     - GET /release?mount=/mnt/usb0&confirm=1 解除占用并重试卸载（需 confirm）
     - GET /openui                            让主机把界面弹到它自己的屏幕上
     - GET /diag                              诊断：UI 自启状态与系统调用返回码
   上位机据此列出卷、由用户选择、再执行卸载。
   主机**加载 payload 时会自己**拉起内置浏览器并弹通知，界面直接
   出现在电视上；/openui 用于事后重新唤起。

为什么不用 9021：
  社区常见 autoloader 内置的 elfldr 是"仅接受 localhost 连接"的 fork 版，
  局域网 PC 推不进 payload；走工具箱的 HTTP 接口才是这台机器上可用的通道。
"""

from __future__ import annotations

import json
import os
import re
import socket
import time
import urllib.error
import urllib.parse
import urllib.request

from ..util import Result


_USB_MOUNT_PREFIX = "/mnt/usb"

# 本地编译产物里带的自述标记：USBMANAGE_BUILD=<x.y.z>;
# 用于判定"上传的 elf 是哪个版本"，进而在部署后核对主机实际运行的版本。
_BUILD_ID_RE = re.compile(rb"USBMANAGE_BUILD=(\d+\.\d+\.\d+)")


def read_build_version(elf_path: str) -> str | None:
    """从本地 elf 里提取构建版本号；取不到返回 None。"""
    try:
        with open(elf_path, "rb") as f:
            blob = f.read()
    except OSError:
        return None
    m = _BUILD_ID_RE.search(blob)
    return m.group(1).decode("ascii") if m else None


def _is_usb_mount(path: str) -> bool:
    """仅 `/mnt/usb` + 一串数字，例如 /mnt/usb0。/mnt/ext* 一律拒绝。"""
    if not path or not path.startswith(_USB_MOUNT_PREFIX):
        return False
    tail = path[len(_USB_MOUNT_PREFIX):]
    return bool(tail) and tail.isdigit()


def normalize_mount(path: str) -> str:
    """还原被 Git Bash（MSYS）误转换的挂载点。

    MSYS 会把命令行里的 `/mnt/usb0` 当成 POSIX 路径，自动改写成
    `D:/Program Files/Git/mnt/usb0` 这类 Windows 路径，导致挂载点参数
    永远对不上。这里按后缀把它还原成 `/mnt/usbN`。

    只认 `/mnt/usb<数字>` 结尾的形态，其余原样返回——不会把无关路径
    变成白名单内的值。
    """
    p = (path or "").replace("\\", "/")
    m = re.search(r"/mnt/(usb\d+)$", p)
    return "/mnt/" + m.group(1) if m else p


class Ps5:
    def __init__(
        self,
        ip: str = "",
        toolbox_port: int = 7788,
        usbmanage_port: int = 9100,
        loader_port: int = 9021,
        payload: str = "",
        timeout: int = 20,
    ) -> None:
        self.ip = ip
        self.toolbox_port = toolbox_port
        self.usbmanage_port = usbmanage_port
        self.loader_port = loader_port
        self.payload = payload
        self.timeout = timeout

    # ------------------------------------------------------------------
    # 基础
    # ------------------------------------------------------------------
    def _url(self, port: int, path: str) -> str:
        return f"http://{self.ip}:{port}{path}"

    def _http(
        self,
        url: str,
        data: bytes | None = None,
        timeout: int | None = None,
    ) -> tuple[int, bytes]:
        req = urllib.request.Request(url, data=data, method="POST" if data is not None else "GET")
        if data is not None:
            req.add_header("Content-Type", "application/octet-stream")
        with urllib.request.urlopen(req, timeout=timeout or self.timeout) as resp:
            return resp.status, resp.read()

    def _tcp_open(self, port: int, timeout: int = 4) -> bool:
        try:
            with socket.create_connection((self.ip, port), timeout=timeout):
                return True
        except OSError:
            return False

    # ------------------------------------------------------------------
    # 通道 1：越狱工具箱（7788）—— 部署 usbmanage.elf
    # ------------------------------------------------------------------
    def probe_toolbox(self) -> Result:
        res = Result(ok=False)
        if not self.ip:
            res.err = "未配置 PS5 IP（config.json 的 ps5.ip，或 --ip）"
            return res
        if not self._tcp_open(self.toolbox_port):
            res.err = (
                f"连不上越狱工具箱 {self.ip}:{self.toolbox_port}。"
                "请确认主机已越狱、且工具箱页面能打开。"
            )
            return res
        try:
            _, raw = self._http(self._url(self.toolbox_port, "/version"), timeout=6)
            ver = raw.decode("utf-8", "replace").strip()
            res.ok = True
            res.add(f"越狱工具箱 {self.ip}:{self.toolbox_port} 可达，版本 {ver}")
        except (urllib.error.URLError, OSError) as e:
            res.err = f"工具箱接口异常：{e}"
        return res

    def list_toolbox_payloads(self) -> Result:
        res = Result(ok=False)
        try:
            _, raw = self._http(self._url(self.toolbox_port, "/list_payloads"))
            data = json.loads(raw.decode("utf-8", "replace"))
            paths = list(data.get("payloads") or [])
            res.ok = True
            res.data = paths  # type: ignore[attr-defined]
            res.add(f"主机已装 payload {len(paths)} 个")
        except (urllib.error.URLError, OSError, ValueError) as e:
            res.err = f"读取 payload 列表失败：{e}"
        return res

    def upload_payload(self, local_path: str, filename: str | None = None) -> Result:
        res = Result(ok=False)
        if not os.path.exists(local_path):
            res.err = f"本地文件不存在：{local_path}"
            return res
        name = filename or os.path.basename(local_path)
        with open(local_path, "rb") as f:
            blob = f.read()
        q = urllib.parse.urlencode({"filename": name})
        url = self._url(self.toolbox_port, f"/manage:upload?{q}")
        try:
            status, _ = self._http(url, data=blob, timeout=max(self.timeout, 60))
            if status != 200:
                res.err = f"上传失败，HTTP {status}"
                return res
            res.ok = True
            res.add(f"已上传 {name}（{len(blob)} 字节）")
        except (urllib.error.URLError, OSError) as e:
            res.err = f"上传失败：{e}"
        return res

    def load_payload(self, remote_path: str) -> Result:
        res = Result(ok=False)
        enc = urllib.parse.quote(remote_path, safe="")
        url = self._url(self.toolbox_port, f"/loadpayload:{enc}")
        try:
            status, _ = self._http(url)
            if status != 200:
                res.err = f"加载失败，HTTP {status}"
                return res
            res.ok = True
            res.add(f"已请求主机加载：{remote_path}")
        except (urllib.error.URLError, OSError) as e:
            res.err = f"加载失败：{e}"
        return res

    def deploy(self, local_elf: str | None = None, start: bool = True) -> Result:
        """把自研 usbmanage.elf 上传到主机并启动（一次性操作）。"""
        res = self.probe_toolbox()
        if not res.ok:
            return res
        elf = local_elf or self.payload
        if not elf or not os.path.exists(elf):
            res.ok = False
            res.err = (
                f"未找到本地 payload：{elf or '(未配置)'}。"
                "请先编译 device/ps5-usbmanage/usbmanage.c 得到 usbmanage.elf"
            )
            return res

        up = self.upload_payload(elf, "usbmanage.elf")
        res.steps.extend(up.steps)
        if not up.ok:
            res.ok = False
            res.err = up.err
            return res

        ls = self.list_toolbox_payloads()
        if not ls.ok:
            res.ok = False
            res.err = ls.err
            return res
        remote = next(
            (p for p in ls.data if p.replace("\\", "/").endswith("/usbmanage.elf")),
            None,
        )
        if not remote:
            res.ok = False
            res.err = "上传后未在主机 payload 列表中找到 usbmanage.elf"
            return res
        res.add(f"主机侧路径：{remote}")

        if start:
            ld = self.load_payload(remote)
            res.steps.extend(ld.steps)
            if not ld.ok:
                res.ok = False
                res.err = ld.err
                return res

            # 校验接管结果：主机自报的版本必须与本次上传的 elf 一致。
            # 工具箱加载 payload 时不会杀掉旧实例；若旧版不支持 /shutdown，
            # 新实例会 bind 失败退出，端口仍由旧版服务——表现为"部署返回成功
            # 但版本没换"。这里把它变成一次显式失败，不留静默坑。
            #
            # 新实例需先请旧实例退出再接管端口，因此轮询等待，最多 6 秒。
            expect = read_build_version(elf)
            running, last_err = "", ""
            for _ in range(12):
                time.sleep(0.5)
                live = self.version()
                if live.ok:
                    running = str(getattr(live, "data", "") or "")
                    last_err = "" if (not expect or running == expect) else (
                        f"主机仍在运行 v{running}（本次上传 v{expect}）"
                    )
                    if not last_err:
                        break
                else:
                    running, last_err = "", live.err

            if expect and running != expect:
                res.ok = False
                res.err = (
                    f"部署未生效：{last_err or '主机未报告版本'}。"
                    f"旧实例占着 {self.usbmanage_port}，且不响应自动接管。"
                    "请在越狱工具箱的「进程管理」里结束 payload.elf，再重新部署。"
                )
                return res
            res.add(f"主机运行版本 v{running}，与本次上传的 elf 一致")
        res.ok = True
        return res

    # ------------------------------------------------------------------
    # 通道 2：usbmanage 服务（9100）—— 枚举与卸载
    # ------------------------------------------------------------------
    def probe_usbmanage(self) -> Result:
        res = Result(ok=False)
        if not self.ip:
            res.err = "未配置 PS5 IP"
            return res
        if not self._tcp_open(self.usbmanage_port):
            res.err = (
                f"usbmanage 服务未就绪（{self.ip}:{self.usbmanage_port} 不可达）。"
                "先部署一次：python usbmanage.py --ip <PS5_IP> ps5-deploy"
            )
            return res
        try:
            _, raw = self._http(self._url(self.usbmanage_port, "/ping"), timeout=6)
            if raw.strip() == b"pong":
                res.ok = True
                res.add(f"usbmanage 服务在线（{self.ip}:{self.usbmanage_port}）")
            else:
                res.err = f"usbmanage 响应异常：{raw[:80]!r}"
        except (urllib.error.URLError, OSError) as e:
            res.err = f"usbmanage 探测失败：{e}"
        return res

    def version(self) -> Result:
        """读取主机侧 usbmanage 自报版本。"""
        res = Result(ok=False)
        try:
            _, raw = self._http(self._url(self.usbmanage_port, "/version"), timeout=8)
        except (urllib.error.URLError, OSError) as e:
            res.err = f"读取版本失败：{e}"
            return res
        try:
            data = json.loads(raw.decode("utf-8", "replace"))
        except ValueError:
            res.err = f"版本响应无法解析：{raw[:120]!r}"
            return res
        ver = data.get("version")
        if ver:
            res.ok = True
            res.data = ver
            res.add(f"主机 usbmanage 版本 {ver}")
        else:
            res.err = "主机未返回版本号——对端没有 /version 端点，恐怕不是 usbmanage 服务"
        return res

    def list_volumes(self) -> Result:
        """枚举主机上所有 USB/外接挂载卷。"""
        res = Result(ok=False)
        entry = self.probe_usbmanage()
        if not entry.ok:
            return entry
        try:
            _, raw = self._http(self._url(self.usbmanage_port, "/list"))
            data = json.loads(raw.decode("utf-8", "replace"))
            vols = list(data.get("volumes") or [])
            res.ok = True
            res.data = vols  # type: ignore[attr-defined]
            res.add(f"发现 {len(vols)} 个外接卷")
        except (urllib.error.URLError, OSError, ValueError) as e:
            res.err = f"枚举失败：{e}"
        return res

    def eject(self, mount: str) -> Result:
        """sync + unmount 指定挂载点。

        客户端侧护栏：只放行 `/mnt/usb<数字>`。
        主机上 `/mnt/ext0`(ufs, es0.crypt) 与 `/mnt/ext1`(bfs, ssd1.user) 是
        内部存储分区，一旦误卸后果严重——即便对手是旧版 payload 也不能松这道口子。
        """
        res = Result(ok=False)
        if not mount:
            res.err = "未指定挂载点"
            return res
        # 先还原 Git Bash 误转换的路径，再上白名单
        mount = normalize_mount(mount)
        if not _is_usb_mount(mount):
            res.err = (
                f"拒绝卸载 {mount}：只允许 /mnt/usb<数字>。"
                "/mnt/ext* 为主机内部存储（扩展存储 / 内置 SSD）。"
            )
            return res
        # safe="/" 让斜杠保持原样：主机侧是按字面量匹配 /mnt/usbN 的，
        # 若编码成 %2Fmnt%2Fusb0 会被白名单直接拒绝（主机端也做了 URL 解码兜底）。
        q = urllib.parse.urlencode({"mount": mount}, safe="/")
        try:
            _, raw = self._http(self._url(self.usbmanage_port, f"/eject?{q}"))
            data = json.loads(raw.decode("utf-8", "replace"))
            res.data = data  # type: ignore[attr-defined]
            if data.get("ok"):
                res.ok = True
                res.add(f"{mount} 已卸载，可安全拔出")
            else:
                res.err = data.get("msg") or f"卸载失败（code={data.get('code')}）"
                # 主机卸载失败时会内联返回占用者，把它转成人话
                holders = data.get("holders") or []
                if holders:
                    names = "、".join(
                        f"pid {h.get('pid')}({h.get('comm', '?')})" for h in holders
                    )
                    res.err += f"；占用者：{names}"
                    res.add("提示：可用 ps5-holders 查看、ps5-release 解除占用后重试")
                elif data.get("holder_count") == 0 and not data.get("holders_supported", True):
                    res.add("提示：主机未放行进程扫描，无法确认占用，可改用强制卸载")
        except (urllib.error.URLError, OSError, ValueError) as e:
            res.err = f"卸载请求失败：{e}"
        return res

    def holders(self, mount: str) -> Result:
        """查询谁在占用指定挂载点。

        返回打开该挂载点下任何文件的进程清单（pid / 进程名 / 打开路径 / 能否安全终止）。
        """
        res = Result(ok=False)
        if not mount:
            res.err = "未指定挂载点"
            return res
        mount = normalize_mount(mount)
        if not _is_usb_mount(mount):
            res.err = f"拒绝查询 {mount}：只允许 /mnt/usb<数字>"
            return res
        q = urllib.parse.urlencode({"mount": mount}, safe="/")
        try:
            _, raw = self._http(self._url(self.usbmanage_port, f"/holders?{q}"))
            data = json.loads(raw.decode("utf-8", "replace"))
            if "unknown endpoint" in str(data.get("msg", "")):
                res.err = ("主机上没有 /holders 端点，对端恐怕不是 usbmanage 服务。"
                           "请确认 payload 已加载，并重新部署与 PC 端配套的 usbmanage.elf。")
                return res
            res.ok = True
            res.data = data  # type: ignore[attr-defined]
            holders = data.get("holders") or []
            if data.get("supported") is False:
                res.add(f"主机未放行进程扫描（无法确认占用），仍列出 {len(holders)} 条")
            else:
                res.add(f"发现 {len(holders)} 个占用进程")
        except (urllib.error.URLError, OSError, ValueError) as e:
            res.err = f"查询占用进程失败：{e}"
        return res

    def release(self, mount: str, confirm: bool = False) -> Result:
        """解除占用并重试卸载。

        会终止占用该卷的进程（先 TERM 后 KILL），因此必须显式 confirm=True
        才真动手；否则只预览会终止谁、不动任何进程。
        """
        res = Result(ok=False)
        if not mount:
            res.err = "未指定挂载点"
            return res
        mount = normalize_mount(mount)
        if not _is_usb_mount(mount):
            res.err = f"拒绝解除 {mount}：只允许 /mnt/usb<数字>"
            return res
        params = {"mount": mount}
        if confirm:
            params["confirm"] = "1"
        q = urllib.parse.urlencode(params, safe="/")
        try:
            _, raw = self._http(self._url(self.usbmanage_port, f"/release?{q}"))
            data = json.loads(raw.decode("utf-8", "replace"))
            if "unknown endpoint" in str(data.get("msg", "")):
                res.err = ("主机上没有 /release 端点，对端恐怕不是 usbmanage 服务。"
                           "请确认 payload 已加载，并重新部署与 PC 端配套的 usbmanage.elf。")
                return res
            res.data = data  # type: ignore[attr-defined]
            if data.get("unmounted"):
                res.ok = True
                res.add(f"{mount} 已解除占用并卸载，可安全拔出")
            elif not confirm and data.get("ok"):
                res.ok = True
                res.add(f"预览：发现 {data.get('before', 0)} 个占用进程（未执行，需 confirm=1）")
            else:
                res.err = data.get("msg") or "解除占用后仍未卸载成功"
        except (urllib.error.URLError, OSError, ValueError) as e:
            res.err = f"解除占用请求失败：{e}"
        return res

    def eject_all(self) -> Result:
        res = Result(ok=False)
        ls = self.list_volumes()
        if not ls.ok:
            return ls
        vols = getattr(ls, "data", []) or []
        # 双重过滤：payload 若带 ejectable 字段则以它为准，再叠一层前缀护栏
        vols = [v for v in vols
                if v.get("ejectable", True) and _is_usb_mount(v.get("mount", ""))]
        if not vols:
            res.add("没有可卸载的外接卷")
            res.ok = True
            return res
        done, failed = 0, []
        for v in vols:
            r = self.eject(v.get("mount", ""))
            res.steps.extend(r.steps)
            if r.ok:
                done += 1
            else:
                failed.append(f"{v.get('mount')}: {r.err}")
        res.ok = failed == []
        if failed:
            res.err = "；".join(failed)
        res.add(f"共卸载 {done}/{len(vols)} 个卷")
        return res

    # ------------------------------------------------------------------
    # UI 自启
    # ------------------------------------------------------------------
    def open_ui(self) -> Result:
        """让主机把选择页弹到它自己的屏幕上（拉内置浏览器 + 弹通知）。

        主机侧对应 usbmanage 的 `/openui`。主机上若没生效，`/diag` 会带回
        三个系统调用的返回码，一并展示出来——省得靠猜。
        """
        res = Result(ok=False)
        entry = self.probe_usbmanage()
        if not entry.ok:
            return entry
        try:
            _, raw = self._http(self._url(self.usbmanage_port, "/openui"), timeout=10)
            data = json.loads(raw.decode("utf-8", "replace"))
            res.data = data  # type: ignore[attr-defined]
            if data.get("ok"):
                res.ok = True
                res.add("已请求主机在电视上打开界面")
                if data.get("browser"):
                    res.add(f"注意：浏览器接口返回 {data.get('browser')}，可能未生效")
                if data.get("notify"):
                    res.add(f"注意：通知接口返回 {data.get('notify')}，可能未生效")
            else:
                msg = data.get("msg") or "主机拒绝了打开界面的请求"
                if "unknown endpoint" in msg:
                    msg = ("主机上没有 /openui 端点，对端恐怕不是 usbmanage 服务。"
                           "请确认 payload 已加载，并部署与 PC 端配套的 usbmanage.elf。")
                res.err = msg
        except (urllib.error.URLError, OSError, ValueError) as e:
            res.err = f"请求失败：{e}"
        return res

    def diag(self) -> Result:
        """读取主机侧诊断信息（版本、端口、局域网地址、UI 自启与系统调用返回码）。

        用于回答"明明加载了却没弹界面"：返回码为 0 说明调用成功但主机没渲染，
        非 0 则是接口被拒绝——两种情况处置方式完全不同，必须能区分。
        """
        res = Result(ok=False)
        try:
            _, raw = self._http(self._url(self.usbmanage_port, "/diag"), timeout=8)
            data = json.loads(raw.decode("utf-8", "replace"))
            if not data.get("version"):
                # 对端若压根没有 /diag，会落到 "unknown endpoint" 分支返回 200 + 一段提示。
                # 别把它当成"诊断结果全是 None"，那样看起来像坏了。
                res.err = ("主机上没有 /diag 端点，对端恐怕不是 usbmanage 服务。"
                           "请确认 payload 已加载，并重新部署与 PC 端配套的 usbmanage.elf。")
                return res
            res.ok = True
            res.data = data  # type: ignore[attr-defined]
            res.add(f"usbmanage {data.get('version')} / 端口 {data.get('port')}"
                    f" / 局域网地址 {data.get('lan_ip')}")
            rc = data.get("rc") or {}
            res.add(f"UI 自启 {'开' if data.get('ui_autolaunch') else '关'}"
                    f"，尝试 {data.get('ui_attempts')} 次")
            res.add(f"系统调用返回码 user_init={rc.get('user_init')}"
                    f" browser={rc.get('browser')} notify={rc.get('notify')}"
                    "（-999 表示未尝试）")
        except (urllib.error.URLError, OSError, ValueError) as e:
            res.err = f"读取诊断信息失败：{e}"
        return res

    # ------------------------------------------------------------------
    # 兼容：直接向 elfldr 端口推送 elf（本机局域网通常不可用，保留备用）
    # ------------------------------------------------------------------
    def probe_loader(self) -> Result:
        res = Result(ok=False)
        if not self.ip:
            res.err = "未配置 PS5 IP"
            return res
        if self._tcp_open(self.loader_port):
            res.ok = True
            res.add(f"ELF loader {self.ip}:{self.loader_port} 可达")
        else:
            res.err = (
                f"无法连接 {self.ip}:{self.loader_port}。"
                "注意：社区 autoloader 内置的 elfldr 多为 localhost-only，"
                "局域网推入需替换为标准 elfldr。"
            )
        return res

    def send_payload(self, path: str | None = None) -> Result:
        res = Result(ok=False)
        payload = path or self.payload
        if not self.ip:
            res.err = "未配置 PS5 IP"
            return res
        if not payload or not os.path.exists(payload):
            res.err = f"未找到 payload 文件：{payload or '(未配置)'}"
            return res
        with open(payload, "rb") as f:
            data = f.read()
        res.add(f"payload：{payload}（{len(data)} 字节）")
        try:
            with socket.create_connection((self.ip, self.loader_port), timeout=self.timeout) as s:
                s.sendall(data)
                try:
                    s.shutdown(socket.SHUT_WR)
                except OSError:
                    pass
                s.settimeout(2)
                try:
                    resp = s.recv(4096)
                except (socket.timeout, OSError):
                    resp = b""
            res.ok = True
            res.add("payload 已推送至 loader")
            if resp:
                res.out = resp.decode("utf-8", "replace")
        except OSError as e:
            res.err = f"推送失败：{e}"
        return res
