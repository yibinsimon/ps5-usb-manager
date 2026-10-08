"""卸载后端集合。"""

from .local_win import eject as eject_local, list_removable
from .ps5 import Ps5

__all__ = ["Ps5", "eject_local", "list_removable"]
