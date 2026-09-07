#!/usr/bin/env python3
"""Contract for resilient SDMMC width and clock fallback."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/sd_storage.c").read_text(encoding="utf-8")


def function(name: str) -> str:
    match = re.search(
        rf"^[\w][\w\s*]*\b{name}\s*\([^;{{}}]*\)\s*\{{",
        SOURCE,
        re.MULTILINE,
    )
    if not match:
        raise AssertionError(f"missing function {name}")
    depth = 1
    cursor = match.end()
    while cursor < len(SOURCE) and depth:
        if SOURCE[cursor] == "{":
            depth += 1
        elif SOURCE[cursor] == "}":
            depth -= 1
        cursor += 1
    if depth:
        raise AssertionError(f"unterminated function {name}")
    return SOURCE[match.end():cursor - 1]


mount = function("sdmmc_mount_with_fallback")
assert mount.count("esp_vfs_fat_sdmmc_mount") == 3
assert "slot->width = 1" in mount
assert "host->max_freq_khz = SDMMC_FREQ_DEFAULT" in mount
assert mount.index("slot->width = 1") < mount.index(
    "host->max_freq_khz = SDMMC_FREQ_DEFAULT"
)

init = function("sd_storage_init")
format_card = function("sd_storage_format")
assert "sdmmc_mount_with_fallback" in init
assert "sdmmc_mount_with_fallback" in format_card

print("SDMMC mount fallback contract passed")
