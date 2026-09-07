#!/usr/bin/env python3
"""Regression contracts for AirSense live-flow display units and scale."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
WRITER = (ROOT / "main/session_writer.c").read_text(encoding="utf-8")
DISPLAY_7B = (ROOT / "main/bsp_display_7b.c").read_text(encoding="utf-8")
DISPLAY_SMALL = (ROOT / "main/bsp_display.c").read_text(encoding="utf-8")
DISPLAY_HEADER = (ROOT / "main/bsp_display.h").read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if not match:
        raise AssertionError(f"function not found: {name}")
    start = match.end()
    depth = 1
    pos = start
    while pos < len(source) and depth:
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
        pos += 1
    if depth:
        raise AssertionError(f"unterminated function: {name}")
    return source[start : pos - 1]


stream = function_body(WRITER, "session_writer_try_stream_data_raw")
live = function_body(WRITER, "publish_live_stream")
push_7b = function_body(DISPLAY_7B, "bsp_display_push_flow")
push_small = function_body(DISPLAY_SMALL, "bsp_display_push_flow")
draw_7b = function_body(DISPLAY_7B, "flow_plot_draw_cb")

# AirSense PatientFlow is parsed into signed hundredths of L/s. Convert it at
# the display API boundary to L/min (raw / 100 * 60 == raw * 0.6).
assert re.search(
    r"bsp_display_push_flow\(flow_vals\[j\] == SNT_MISSING \? NAN : flow_vals\[j\] \* 0\.6f\)",
    live,
), "PatientFlow converts valid values to L/min and retains missing positions"
assert "bsp_display_push_flow_gap(missing)" in stream
assert stream.index("bsp_display_push_flow_gap(missing)") < stream.rindex("publish_live_stream(")
assert "!isfinite(flow_lpm)" in push_7b and "bsp_display_push_flow_gap(1)" in push_7b
assert "litres per minute" in DISPLAY_HEADER

# Both panel implementations now consume the documented L/min unit. The 7B
# retains one decimal place in its integer ring; the small panel stores L/min
# directly and must not apply the legacy second x60 conversion.
assert re.search(r"lrintf\s*\(\s*flow_lpm\s*\*\s*10\.0f\s*\)", push_7b)
assert "flow_lpm * 60.0f" not in push_small

# Guard the concrete overnight failure: a normal raw sample of 0.50 L/s is
# 30 L/min, becomes 300 deci-L/min, and must produce a clearly visible
# deflection in the 232 px chart. The former path stored 5 and rounded to a
# zero-pixel offset.
raw_centi_lps = 50
flow_lpm = raw_centi_lps * 0.6
stored_deci_lpm = round(flow_lpm * 10.0)
chart_height = 232
pixel_offset = stored_deci_lpm * (chart_height - 18) // 2000
assert stored_deci_lpm == 300
assert pixel_offset >= 24, f"normal breathing is still subpixel: {pixel_offset}px"
assert re.search(
    r"offset\s*=\s*value\s*\*\s*\(height\s*-\s*18\)\s*/\s*2000",
    draw_7b,
), "test model no longer matches the physical chart scale"

print("Live flow unit/scale contracts passed")
