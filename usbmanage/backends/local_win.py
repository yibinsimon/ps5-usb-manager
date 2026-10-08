"""后端：Windows 本机可移动磁盘安全弹出。

两种取径，按顺序尝试：
  1. Shell.Application 的 Eject 动词（等价于资源管理器"弹出"）
  2. mountvol <盘符> /p（移除挂载点，卸载卷）

适用于"先在 PC 写入媒体 → 再拿到电视/主机播放"的工作流收尾。
"""

from __future__ import annotations

from ..util import Result, run_ps

# 脚本里所有花括号都是 PowerShell 的语法，只有 __DRIVE__ 这一个占位符需要替换，
# 所以走 str.replace 而不是 str.format——后者会把 PowerShell 的 {} 当成格式化字段，
# 一遇到 `{ Write-Output ... }` 就抛 KeyError。
_EJECT_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
$drive = '__DRIVE__'
$sh = New-Object -ComObject Shell.Application
$ns = $sh.Namespace(17)            # 17 = 本机驱动器
$item = $ns.ParseName($drive)
if ($null -eq $item) { Write-Output 'NOTFOUND'; exit 2 }
try {
    $item.InvokeVerb('Eject')
    Write-Output 'SHELL_EJECT_OK'
} catch {
    Write-Output ('SHELL_EJECT_FAIL ' + $_.Exception.Message)
}
"""

# mountvol 必须如实回传退出码：早先的写法不管成败都打印 MOUNTVOL_DONE，
# 盘符不存在时也会被判成"弹出成功"——静默谎报比直接报错更坏。
# 2>&1 写在 cmd 的命令行里由 cmd 合并到 stdout，避免 PowerShell 把
# stderr 变成 ErrorRecord（配合 $ErrorActionPreference='Stop' 会变成终止性错误）。
_MOUNTVOL_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
$drive = '__DRIVE__'
$t = cmd /c "mountvol $drive /p 2>&1"
$rc = $LASTEXITCODE
if ($t) { Write-Output $t }
if ($rc -ne 0) { Write-Output ("MOUNTVOL_FAIL rc=$rc"); exit $rc }
Write-Output 'MOUNTVOL_DONE'
"""

_LIST_SCRIPT = r"""
$ErrorActionPreference = 'SilentlyContinue'
Get-CimInstance Win32_LogicalDisk |
  Where-Object { $_.DriveType -eq 2 } |
  ForEach-Object {
    $vol = (Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$($_.DeviceID)'")
    '{0}|{1}|{2}|{3}' -f $_.DeviceID, $_.VolumeName, $_.FileSystem, $_.Size
  }
"""


def list_removable() -> Result:
    res = Result(ok=False)
    code, out, err = run_ps(_LIST_SCRIPT, timeout=40)
    res.code, res.err = code, err
    if code != 0:
        return res
    drives = []
    for line in out.splitlines():
        line = line.strip()
        if not line or "|" not in line:
            continue
        parts = line.split("|")
        if len(parts) < 4:
            continue
        # 卷标理论上不含 "|"（Windows 不允许），但从两端定位字段是零成本的保险：
        # 万一中间多出分隔符，也只会并进卷标，不会把文件系统与容量挤错位。
        drives.append(
            {
                "drive": parts[0],
                "label": "|".join(parts[1:-2]),
                "fs": parts[-2],
                "size": parts[-1],
            }
        )
    res.data = drives
    res.ok = True
    return res


def eject(drive: str) -> Result:
    """弹出指定盘符。返回 Result，任何失败都落在 err 里，不外抛异常。"""
    res = Result(ok=False)
    try:
        drive = drive.strip().upper()
        if len(drive) == 1:
            drive = drive + ":"
        if not drive.endswith(":"):
            res.err = f"盘符格式应为 E 或 E:，收到 {drive}"
            return res

        code, out, err = run_ps(_EJECT_SCRIPT.replace("__DRIVE__", drive), timeout=40)
        out = out.strip()
        res.add(f"Shell Eject {drive} -> {out or err.strip()}")
        if "SHELL_EJECT_OK" in out:
            res.ok = True
            return res
        if "NOTFOUND" in out:
            # 盘符不在系统里（没插盘/盘符写错），再试 mountvol 也只会多一条失败，
            # 不如直接给一句能对症的话。
            res.err = f"系统里没有 {drive} 这个盘符：可能是可移动磁盘未插入，或盘符写错。"
            return res

        # 回退：mountvol /p
        code, out2, err2 = run_ps(_MOUNTVOL_SCRIPT.replace("__DRIVE__", drive), timeout=40)
        res.add(f"mountvol {drive} /p -> {(out2 + err2).strip()}")
        if code == 0 and "MOUNTVOL_DONE" in out2:
            res.ok = True
        else:
            res.err = (err2 or out2).strip() or res.err
    except Exception as e:  # noqa: BLE001 —— 本函数是后端边界，兜住一切才是正确行为
        res.ok = False
        res.err = f"弹出 {drive} 时发生意外错误：{type(e).__name__}: {e}"
    return res
