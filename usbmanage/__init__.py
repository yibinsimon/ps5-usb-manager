"""usbmanage - USB 存储安全卸载工具。

范围：
  - **PS5**（越狱固件，通过 ELF payload 注入）—— 本项目的主体
  - Windows 本机可移动磁盘 —— PC 控制台附带的本地弹出

设计原则：卸载前先 sync 落盘，再按卷卸载，并回读校验。
"""

# 与 payload（device/ps5-usbmanage/usbmanage.c 的 VERSION）同号发布。
# 两者一起迭代、一起发版，不做独立版本号——避免"工具说 0.1.0、主机说 1.0.0"。
__version__ = "1.0.0"
