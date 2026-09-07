#!/usr/bin/env python3
"""Resolve the exact LVGL 8.4.0 input source used by the release-cancellation test."""
import hashlib
from pathlib import Path
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
EXPECTED_SHA256 = "ce2d4915b4e478c02a6f86474e4a241056128f674c60edfd1d40ad5e7fd1b599"
URL = "https://raw.githubusercontent.com/lvgl/lvgl/v8.4.0/src/core/lv_indev.c"


def prepare_lvgl():
    managed = ROOT / "managed_components/lvgl__lvgl/src/core/lv_indev.c"
    cached = ROOT / ".host-deps/lvgl-8.4.0/lv_indev.c"
    path = managed if managed.exists() else cached
    if not path.exists():
        with urlopen(URL, timeout=30) as response:
            content = response.read()
        if hashlib.sha256(content).hexdigest() != EXPECTED_SHA256:
            raise RuntimeError("LVGL 8.4.0 source digest mismatch")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    if hashlib.sha256(path.read_bytes()).hexdigest() != EXPECTED_SHA256:
        raise RuntimeError(f"LVGL 8.4.0 source digest mismatch: {path}")
    return path


if __name__ == "__main__":
    print(prepare_lvgl())
