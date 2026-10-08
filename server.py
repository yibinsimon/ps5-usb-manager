#!/usr/bin/env python3
"""本地 Web 控制台后端（仅标准库）。

启动：
  python server.py --port 8848

接口：
  GET  /                控制台页面
  GET  /icon.png        页面图标（/favicon.ico 返回同一份；即 web/icon.png）
  GET  /api/status      目标与卷状态
  POST /api/eject       {"target":"local|ps5","drive":"E:","ip":"..."}
  POST /api/ps5/openui  让 PS5 在自己的屏幕上弹出选择界面，并返回主机诊断
"""

from __future__ import annotations

import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from usbmanage import config as cfgmod
from usbmanage.backends import Ps5, eject_local, list_removable

WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
_CFG: dict = {}


def _ps5(ip: str) -> "Ps5":
    """按配置构造 PS5 后端（工具箱部署通道 + usbmanage 运行通道）。"""
    c = _CFG["ps5"]
    return Ps5(
        ip=ip,
        toolbox_port=c.get("toolbox_port", 7788),
        usbmanage_port=c.get("usbmanage_port", 9100),
        loader_port=c.get("loader_port", 9021),
        payload=c.get("payload", ""),
    )


def _status() -> dict:
    local = list_removable()
    ps5_ip = _CFG["ps5"].get("ip", "")

    ps5 = {
        "ip": ps5_ip,
        "online": False,
        "port": _CFG["ps5"].get("usbmanage_port", 9100),
        "toolbox_port": _CFG["ps5"].get("toolbox_port", 7788),
        "version": "",
        "volumes": [],
        "error": "",
    }
    if ps5_ip:
        p = _ps5(ps5_ip)
        c = p.probe_usbmanage()
        ps5["online"] = c.ok
        if not c.ok:
            ps5["error"] = c.err
        else:
            v = p.version()
            if v.ok:
                ps5["version"] = str(v.data or "")
            lv = p.list_volumes()
            if lv.ok:
                ps5["volumes"] = list(lv.data or [])
            else:
                ps5["error"] = lv.err

    return {
        "local": local.data if local.ok else [],
        "local_error": "" if local.ok else local.err,
        "ps5": ps5,
    }


def _do_eject(body: dict) -> dict:
    target = body.get("target", "")
    ip = body.get("ip") or ""
    if target == "local":
        drive = body.get("drive") or (_CFG["local"].get("drives") or [None])[0]
        if not drive:
            return {"ok": False, "err": "未指定盘符"}
        return eject_local(drive).as_dict()
    if target == "ps5":
        ip = ip or _CFG["ps5"].get("ip")
        if not ip:
            return {"ok": False, "err": "未配置 PS5 IP"}
        p = _ps5(ip)
        # 给了具体挂载点就卸那一个；否则卸掉全部外接卷（仅 /mnt/usb*，
        # 客户端侧 _is_usb_mount 会再挡一道，内部卷 /mnt/ext* 进不来）。
        mount = (body.get("mount") or "").strip()
        return (p.eject(mount) if mount else p.eject_all()).as_dict()
    return {"ok": False, "err": f"未知目标：{target}"}


def _do_ps5_openui(body: dict) -> dict:
    """让 PS5 把界面弹到它自己屏幕上，并附上主机诊断。

    诊断一并返回是有意的：若电视上没出现界面，返回码能直接区分
    "系统调用被主机拒绝"（rc 非 0）与 "根本没尝试"（rc = -999）。
    """
    ip = body.get("ip") or _CFG["ps5"].get("ip")
    if not ip:
        return {"ok": False, "err": "未配置 PS5 IP"}
    p = _ps5(ip)
    r = p.open_ui()
    out = r.as_dict()
    d = p.diag()
    if d.ok:
        out["diag"] = getattr(d, "data", None)
    # 页面在 diag 缺失时用 diag_log 兜底逐行显示，所以这里必须给数组：
    # 诊断步骤 + 失败原因。diag() 从不写 res.out，原先取 d.out 恒为空串，
    # 等于把这条降级路径写死了。
    out["diag_log"] = list(d.steps) + ([d.err] if d.err else [])
    return out


def _do_ps5_holders(body: dict) -> dict:
    """查询谁在占用指定 PS5 卷。"""
    ip = body.get("ip") or _CFG["ps5"].get("ip")
    if not ip:
        return {"ok": False, "err": "未配置 PS5 IP"}
    mount = (body.get("mount") or "").strip()
    if not mount:
        return {"ok": False, "err": "未指定挂载点"}
    p = _ps5(ip)
    return p.holders(mount).as_dict()


def _do_ps5_release(body: dict) -> dict:
    """解除占用并重试卸载。confirm 才动手，否则只预览。"""
    ip = body.get("ip") or _CFG["ps5"].get("ip")
    if not ip:
        return {"ok": False, "err": "未配置 PS5 IP"}
    mount = (body.get("mount") or "").strip()
    if not mount:
        return {"ok": False, "err": "未指定挂载点"}
    p = _ps5(ip)
    return p.release(mount, confirm=bool(body.get("confirm"))).as_dict()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # 静默
        pass

    def _send(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, code: int = 200) -> None:
        self._send(code, json.dumps(obj, ensure_ascii=False).encode("utf-8"), "application/json; charset=utf-8")

    def do_GET(self) -> None:
        if self.path in ("/", "/index.html"):
            path = os.path.join(WEB_DIR, "index.html")
            if os.path.exists(path):
                with open(path, "rb") as f:
                    self._send(200, f.read(), "text/html; charset=utf-8")
            else:
                self._send(404, b"index.html missing", "text/plain; charset=utf-8")
        elif self.path in ("/icon.png", "/favicon.ico"):
            # 页面图标与 PS5 payload 用的是同一份文件（assets/icon/ 的 96x96）。
            # 这里从磁盘直接读，不需要重新构建任何东西，换图只换 web/icon.png。
            path = os.path.join(WEB_DIR, "icon.png")
            if os.path.exists(path):
                with open(path, "rb") as f:
                    self._send(200, f.read(), "image/png")
            else:
                self._send(404, b"icon.png missing", "text/plain; charset=utf-8")
        elif self.path.startswith("/api/status"):
            self._json(_status())
        else:
            self._send(404, b"not found", "text/plain; charset=utf-8")

    def do_POST(self) -> None:
        if self.path.startswith("/api/ps5/openui"):
            length = int(self.headers.get("Content-Length", "0") or 0)
            raw = self.rfile.read(length) if length else b"{}"
            try:
                body = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                body = {}
            self._json(_do_ps5_openui(body))
            return
        if self.path.startswith("/api/ps5/holders"):
            length = int(self.headers.get("Content-Length", "0") or 0)
            raw = self.rfile.read(length) if length else b"{}"
            try:
                body = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                body = {}
            self._json(_do_ps5_holders(body))
            return
        if self.path.startswith("/api/ps5/release"):
            length = int(self.headers.get("Content-Length", "0") or 0)
            raw = self.rfile.read(length) if length else b"{}"
            try:
                body = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                body = {}
            self._json(_do_ps5_release(body))
            return
        if not self.path.startswith("/api/eject"):
            self._send(404, b"not found", "text/plain; charset=utf-8")
            return
        length = int(self.headers.get("Content-Length", "0") or 0)
        raw = self.rfile.read(length) if length else b"{}"
        try:
            body = json.loads(raw.decode("utf-8") or "{}")
        except json.JSONDecodeError:
            self._json({"ok": False, "err": "请求体不是合法 JSON"}, 400)
            return
        self._json(_do_eject(body))


def serve(host: str = "127.0.0.1", port: int = 8848, cfg: dict | None = None) -> int:
    global _CFG
    _CFG = cfg or cfgmod.load()
    httpd = ThreadingHTTPServer((host, port), Handler)
    url = f"http://{host}:{port}"
    print(f"PS5 USB管理器 控制台已启动：{url}")
    print("按 Ctrl+C 停止。")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止。")
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser(description="usbmanage 本地 Web 控制台")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8848)
    ap.add_argument("--config")
    a = ap.parse_args()
    sys.exit(serve(a.host, a.port, cfgmod.load(a.config)))
