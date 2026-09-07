#!/usr/bin/env python3
"""Regression contract for user-visible microSD storage notices."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
STORAGE = (ROOT / "main/sd_storage.c").read_text(encoding="utf-8")
WRITER = (ROOT / "main/session_writer.c").read_text(encoding="utf-8")
MAIN = (ROOT / "main/main.c").read_text(encoding="utf-8")


def require(source: str, pattern: str, description: str) -> None:
    assert re.search(pattern, source), f"missing storage notice contract: {description}"


# Near-full storage still permits recording, so it must remain transient. A
# hard floor or an actual write failure means recording is unavailable/lossy
# and must remain persistent until the user addresses it.
require(
    STORAGE,
    r'bsp_display_set_notice\("microSD nearly full"\)',
    "near-full is an ordinary notice",
)
require(
    STORAGE,
    r'bsp_display_set_critical_notice\("microSD full"\)',
    "full storage is critical",
)
require(
    WRITER,
    r'bsp_display_set_critical_notice\("microSD write error"\)',
    "write failure is critical",
)
assert 'set_critical_notice("microSD nearly full")' not in STORAGE

# User-facing boot/status copy uses the same hardware name. Technical symbols
# such as SDCARD/ intentionally retain their ResMed-compatible names.
for legacy_copy in (
    "Insert SD Card",
    "SD Card Error",
    "Card mount failed",
    "Power off, insert card",
):
    assert legacy_copy not in MAIN, f"legacy user-visible storage copy remains: {legacy_copy}"
for copy in (
    "Insert microSD",
    "Power off, insert microSD",
    "microSD error",
    "microSD mount failed",
):
    assert copy in MAIN, f"missing microSD boot/status copy: {copy}"

print("storage notice severity and copy contract passed")
