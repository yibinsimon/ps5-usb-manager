"""配置加载。默认值 + 可选 config.json 覆盖。"""

from __future__ import annotations

import copy
import json
import os

DEFAULTS: dict = {
    "ps5": {
        "ip": "",
        "toolbox_port": 7788,
        "usbmanage_port": 9100,
        "loader_port": 9021,
        "payload": "device/ps5-usbmanage/usbmanage.elf",
    },
    "local": {
        "drives": [],
    },
}


def _merge(base: dict, override: dict) -> dict:
    for k, v in override.items():
        if isinstance(v, dict) and isinstance(base.get(k), dict):
            _merge(base[k], v)
        else:
            base[k] = v
    return base


def load(path: str | None = None) -> dict:
    cfg = copy.deepcopy(DEFAULTS)
    candidate = path or os.path.join(os.getcwd(), "config.json")
    if os.path.exists(candidate):
        with open(candidate, "r", encoding="utf-8") as f:
            user = json.load(f)
        if isinstance(user, dict):
            _merge(cfg, user)
    return cfg


def save_example(path: str) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(DEFAULTS, f, ensure_ascii=False, indent=2)
