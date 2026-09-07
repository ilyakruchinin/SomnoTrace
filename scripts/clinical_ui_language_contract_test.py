#!/usr/bin/env python3
"""Structural contract for clinically honest bedside labels and tooltips."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HISTORY = (ROOT / "main/touch_history_ui.c").read_text(encoding="utf-8")
DISPLAY = (ROOT / "main/bsp_display_7b.c").read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"\b{name}\s*\([^)]*\)\s*\{{(.*?)\n\}}",
        source,
        re.DOTALL,
    )
    if not match:
        raise AssertionError(f"missing function {name}")
    return match.group(1)


# A display-bin aggregate must never be presented as an exact instantaneous
# sample. Each aggregation shape is named, including both Flow envelope bounds.
for copy in ("Bin min/max", "Bin min", "Bin max", "Bin mean"):
    assert copy in HISTORY, f"missing aggregate-aware cursor copy: {copy}"
assert "upper_x100[nearest]" in HISTORY
assert "TOUCH_HISTORY_POINT_UPPER_VALID" in HISTORY

# Only an event whose recorded interval contains the cursor may appear in the
# same tooltip. The old broad nearest-event tolerance created false proximity.
event_body = function_body(HISTORY, "history_ui_cursor_event")
assert "cursor_ms >= event->start_ms" in event_body
assert "cursor_ms <= event->end_ms" in event_body
assert "nearest_distance" not in event_body
assert "tolerance" not in event_body

# The shared graph axis can include O2 lead/lag, so it is a review window rather
# than a claim about the AirSense session boundary.
assert '"Review window %s–%s"' in HISTORY

# Four hours is identified as an adherence threshold, never as adequate therapy.
assert '"≥4 h adherence"' in HISTORY
assert '"<4 h adherence"' in HISTORY
assert '"on target"' not in HISTORY
assert '"below target"' not in HISTORY

# Respiratory rate cannot be confused with heart rate, and the zero-event copy
# retains generic apnea as a distinct type used by ST AHI.
assert '"RESP RATE", "br/min"' in DISPLAY
assert "No OA/CA/H/A/RERA events recorded" in HISTORY

print("clinical UI language contract passed")
