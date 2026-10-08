#!/usr/bin/env python3
"""usbmanage 命令行入口。

用法示例：
  python usbmanage.py doctor
  python usbmanage.py list
  python usbmanage.py --ip 192.168.1.100 ps5-deploy      # 首次：部署 usbmanage 到 PS5
  python usbmanage.py --ip 192.168.1.100 eject ps5 --mount /mnt/usb0   # 卸载指定卷
  python usbmanage.py --ip 192.168.1.100 eject ps5       # 卸载全部外接卷
  python usbmanage.py eject local --drive E:
  python usbmanage.py eject all
  python usbmanage.py serve --port 8848

注：`--ip` 既可写在子命令之前（全局位置），也可写在子命令之后。
"""

from __future__ import annotations

import argparse
import sys

from usbmanage import __version__, config as cfgmod
from usbmanage.backends import Ps5, eject_local, list_removable
from usbmanage.util import Result


def _print_steps(res: Result) -> None:
    for s in res.steps:
        print(f"  - {s}")
    if res.err:
        print(f"  ! {res.err}")


def _human(n) -> str:
    try:
        v = float(n)
    except (TypeError, ValueError):
        return str(n)
    for unit in ("B", "KB", "MB", "GB", "TB", "PB"):
        if v < 1024 or unit == "PB":
            return f"{v:.1f} {unit}"
        v /= 1024
    return f"{v:.1f} PB"


def _make_ps5(cfg: dict, args, ip: str) -> Ps5:
    ps = cfg["ps5"]
    return Ps5(
        ip=ip,
        toolbox_port=ps.get("toolbox_port", 7788),
        usbmanage_port=ps.get("usbmanage_port", 9100),
        loader_port=ps.get("loader_port", 9021),
        payload=getattr(args, "payload", None) or getattr(args, "elf", None) or ps.get("payload", ""),
    )


def cmd_doctor(args, cfg) -> int:
    print("== 环境自检 ==")
    ps5_ip = args.ip or cfg["ps5"].get("ip")
    if ps5_ip:
        print(f"检测 PS5 {ps5_ip} ...")
        p = _make_ps5(cfg, args, ps5_ip)
        _print_steps(p.probe_toolbox())
        _print_steps(p.probe_usbmanage())
    else:
        print("未配置 PS5 IP，跳过")
    return 0


def cmd_list(args, cfg) -> int:
    print("== 本机可移动磁盘 ==")
    r = list_removable()
    if r.ok and r.data:
        for d in r.data:
            size = d["size"]
            try:
                size = f"{int(size) / 1e9:.1f} GB" if size and size.isdigit() else size
            except Exception:
                pass
            print(f"  {d['drive']:<4} {d['label']:<12} {d['fs']:<8} {size}")
    elif r.ok:
        print("  （没有可移动磁盘）")
    else:
        print(f"  （读取失败：{r.err or '未知原因'}）")

    ps5_ip = args.ip or cfg["ps5"].get("ip")
    print(f"== PS5 {ps5_ip or '(未配置 IP)'} 外接卷 ==")
    if ps5_ip:
        p = _make_ps5(cfg, args, ps5_ip)
        r = p.list_volumes()
        vols = getattr(r, "data", None) or []
        if r.ok and vols:
            for v in vols:
                print(f"  {v.get('mount', ''):<14} {v.get('fstype', ''):<10} "
                      f"{v.get('device', ''):<18} {_human(v.get('free'))} / {_human(v.get('total'))}")
        else:
            _print_steps(r)
    return 0


def cmd_ps5_deploy(args, cfg) -> int:
    ip = args.ip or cfg["ps5"].get("ip")
    if not ip:
        print("未配置 PS5 IP（config.json 的 ps5.ip，或 --ip）")
        return 1
    p = _make_ps5(cfg, args, ip)
    r = p.deploy(local_elf=args.elf or None)
    print("== 部署 usbmanage 到 PS5 ==")
    _print_steps(r)
    if r.ok:
        print("== 部署后探测 ==")
        _print_steps(p.probe_usbmanage())
    return 0 if r.ok else 1


def cmd_eject(args, cfg) -> int:
    targets = [args.target] if args.target != "all" else ["local", "ps5"]
    overall_ok = True

    if "local" in targets:
        drive = args.drive or (cfg["local"].get("drives") or [None])[0]
        print("== 本机弹出 ==")
        if not drive:
            print("  跳过：未指定 --drive 且 config.json 未配置 local.drives")
        else:
            r = eject_local(drive)
            _print_steps(r)
            overall_ok = overall_ok and r.ok

    if "ps5" in targets:
        ip = args.ip or cfg["ps5"].get("ip")
        print("== PS5 安全卸载（usbmanage）==")
        if not ip:
            print("  跳过：未配置 ps5.ip")
        else:
            p = _make_ps5(cfg, args, ip)
            r = p.eject(args.mount) if args.mount else p.eject_all()
            _print_steps(r)
            overall_ok = overall_ok and r.ok

    return 0 if overall_ok else 1


def cmd_ps5_ui(args, cfg) -> int:
    """让 PS5 在自己的屏幕上弹出选择界面，并顺手打印主机侧诊断信息。

    典型场景：payload 已经在主机上跑着，但电视上没出现界面——先跑这条命令，
    比走到电视前手动敲地址快；诊断里的系统调用返回码能直接区分
    "接口被主机拒绝" 与 "根本没尝试"。
    """
    ip = args.ip or cfg["ps5"].get("ip")
    if not ip:
        print("未配置 PS5 IP（config.json 的 ps5.ip，或 --ip）")
        return 1
    p = _make_ps5(cfg, args, ip)
    print("== 请求主机打开界面 ==")
    r = p.open_ui()
    _print_steps(r)
    print("== 主机诊断 ==")
    _print_steps(p.diag())
    return 0 if r.ok else 1


def cmd_ps5_holders(args, cfg) -> int:
    """查询谁在占用指定 PS5 卷。"""
    ip = args.ip or cfg["ps5"].get("ip")
    if not ip:
        print("未配置 PS5 IP（config.json 的 ps5.ip，或 --ip）")
        return 1
    if not args.mount:
        print("请用 --mount 指定挂载点，如 --mount /mnt/usb0")
        return 1
    p = _make_ps5(cfg, args, ip)
    print(f"== 查询占用：{args.mount} ==")
    r = p.holders(args.mount)
    _print_steps(r)
    if r.ok:
        data = getattr(r, "data", {}) or {}
        holders = data.get("holders") or []
        if not holders:
            print("  没有发现占用进程。")
        for h in holders:
            killable = h.get("killable", True)
            mark = "（不可安全终止）" if killable is False else ""
            print(f"  - pid {h.get('pid')}  {h.get('comm', '?')}  {h.get('path', '')}{mark}")
    return 0 if r.ok else 1


def cmd_ps5_release(args, cfg) -> int:
    """解除占用并重试卸载。"""
    ip = args.ip or cfg["ps5"].get("ip")
    if not ip:
        print("未配置 PS5 IP（config.json 的 ps5.ip，或 --ip）")
        return 1
    if not args.mount:
        print("请用 --mount 指定挂载点，如 --mount /mnt/usb0")
        return 1
    p = _make_ps5(cfg, args, ip)
    if args.preview:
        print(f"== 预览解除占用（不动任何进程）：{args.mount} ==")
        r = p.release(args.mount, confirm=False)
    else:
        print(f"== 解除占用并卸载：{args.mount} ==")
        print("  注意：这会终止占用该卷的进程（先 TERM 后 KILL）。")
        r = p.release(args.mount, confirm=True)
    _print_steps(r)
    return 0 if r.ok else 1


def cmd_serve(args, cfg) -> int:
    from server import serve

    return serve(host=args.host, port=args.port, cfg=cfg)


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="usbmanage", description="跨设备 USB 存储安全卸载工具")
    ap.add_argument("--config", help="配置文件路径（默认 ./config.json）")
    ap.add_argument("--ip", help="覆盖目标设备 IP")
    ap.add_argument("--version", action="version", version=f"usbmanage {__version__}")
    sub = ap.add_subparsers(dest="cmd", required=True)

    # 子命令也各带一个 --ip（dest 另取 ip_sub，避免覆盖全局值），
    # 这样 `--ip X ps5-deploy` 与 `ps5-deploy --ip X` 两种写法都能用。
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--ip", dest="ip_sub",
                        help="覆盖目标设备 IP（等价于写在本命令之前的 --ip）")

    p_doc = sub.add_parser("doctor", parents=[common], help="环境自检")
    p_doc.set_defaults(func=cmd_doctor)

    p_list = sub.add_parser("list", parents=[common], help="列出本机与各设备的存储卷")
    p_list.set_defaults(func=cmd_list)

    p_dep = sub.add_parser("ps5-deploy", parents=[common],
                           help="部署 usbmanage payload 到 PS5（首次/每次越狱后）")
    p_dep.add_argument("--elf", help="本地 usbmanage.elf 路径（默认取 config 的 ps5.payload）")
    p_dep.set_defaults(func=cmd_ps5_deploy)

    p_ej = sub.add_parser("eject", parents=[common], help="执行安全卸载")
    p_ej.add_argument("target", choices=["local", "ps5", "all"])
    p_ej.add_argument("--drive", help="本机盘符，如 E:")
    p_ej.add_argument("--mount", help="PS5 挂载点，如 /mnt/usb0（不指定则卸载全部外接卷）")
    p_ej.add_argument("--payload", help="PS5 payload 路径")
    p_ej.set_defaults(func=cmd_eject)

    p_ui = sub.add_parser("ps5-ui", parents=[common],
                          help="让 PS5 在自己的屏幕上弹出选择界面，并打印主机诊断")
    p_ui.set_defaults(func=cmd_ps5_ui)

    p_h = sub.add_parser("ps5-holders", parents=[common],
                         help="查询谁在占用指定 PS5 卷")
    p_h.add_argument("--mount", required=True, help="挂载点，如 /mnt/usb0")
    p_h.set_defaults(func=cmd_ps5_holders)

    p_r = sub.add_parser("ps5-release", parents=[common],
                         help="解除占用并重试卸载（默认只预览，--confirm 才动手）")
    p_r.add_argument("--mount", required=True, help="挂载点，如 /mnt/usb0")
    p_r.add_argument("--confirm", dest="preview", action="store_false",
                     help="真正解除占用并卸载（默认只预览，不动任何进程）")
    p_r.set_defaults(func=cmd_ps5_release, preview=True)

    p_srv = sub.add_parser("serve", help="启动本地 Web 控制台")
    p_srv.add_argument("--host", default="127.0.0.1")
    p_srv.add_argument("--port", type=int, default=8848)
    p_srv.set_defaults(func=cmd_serve)
    return ap


def main(argv: list[str] | None = None) -> int:
    ap = build_parser()
    args = ap.parse_args(argv)
    # 子命令上的 --ip 作为兜底：全局那份没给时才生效
    if not args.ip and getattr(args, "ip_sub", None):
        args.ip = args.ip_sub
    cfg = cfgmod.load(args.config)
    try:
        return args.func(args, cfg)
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
