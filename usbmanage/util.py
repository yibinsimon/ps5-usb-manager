"""通用工具：命令执行与结果封装。"""

from __future__ import annotations

import codecs
import locale
import shutil
import subprocess
from dataclasses import dataclass, field


@dataclass
class Result:
    """一次外部命令或动作的执行结果。"""

    ok: bool
    code: int = 0
    out: str = ""
    err: str = ""
    steps: list[str] = field(default_factory=list)
    data: list = field(default_factory=list)

    def add(self, text: str) -> None:
        self.steps.append(text)

    def as_dict(self) -> dict:
        return {
            "ok": self.ok,
            "code": self.code,
            "out": self.out,
            "err": self.err,
            "steps": self.steps,
            "data": self.data,
        }


def which(name: str) -> str | None:
    return shutil.which(name)


def _console_encoding() -> str:
    """当前控制台输出代码页对应的 Python 编解码器名。

    简体中文 Windows 上是 cp936。取不到就退回系统首选编码。
    """
    try:
        import ctypes

        k32 = ctypes.windll.kernel32
        cp = k32.GetConsoleOutputCP() or k32.GetOEMCP()
        if cp:
            name = f"cp{cp}"
            codecs.lookup(name)  # 名字不被 Python 认识就抛异常，落到下面的兜底
            return name
    except Exception:
        pass
    return locale.getpreferredencoding(False) or "utf-8"


def _decode(data: bytes) -> str:
    """解码外部命令的输出。

    Windows 上这一步不能想当然：PowerShell 5.1 往管道里写的是**控制台代码页**
    （简体中文系统上是 GBK），不是 UTF-8。原先写死 encoding="utf-8"，磁盘卷标、
    mountvol 的报错这类中文会被解成乱码。

    先按 UTF-8 严格解，失败再按控制台代码页解。GBK 汉字的尾字节落在 0x40-0x7E，
    不是合法的 UTF-8 续字节，所以真中文几乎必然在第一步就失败——这个先后顺序足够稳。
    """
    if not data:
        return ""
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return data.decode(_console_encoding(), "replace")


def run(cmd: list[str], timeout: int = 30) -> tuple[int, str, str]:
    """执行命令，返回 (returncode, stdout, stderr)。不抛异常。"""
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout)
        return p.returncode, _decode(p.stdout or b""), _decode(p.stderr or b"")
    except subprocess.TimeoutExpired as e:
        raw_out = e.stdout if isinstance(e.stdout, bytes) else (e.stdout or b"")
        raw_err = e.stderr if isinstance(e.stderr, bytes) else (e.stderr or b"")
        out = _decode(raw_out) if isinstance(raw_out, bytes) else raw_out
        err = _decode(raw_err) if isinstance(raw_err, bytes) else raw_err
        return 124, out, err + f"\n[命令超时 {timeout}s]"
    except FileNotFoundError as e:
        return 127, "", f"未找到可执行文件：{e}"
    except OSError as e:
        return 126, "", f"执行失败：{e}"


def run_ps(script: str, timeout: int = 60) -> tuple[int, str, str]:
    """在 Windows 上执行一段 PowerShell 脚本。"""
    exe = which("powershell") or which("pwsh") or "powershell"
    return run(
        [exe, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script],
        timeout=timeout,
    )
