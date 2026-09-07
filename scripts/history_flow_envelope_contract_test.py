#!/usr/bin/env python3
"""Contracts preventing ranged raw Flow from averaging breaths flat."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SERVICE = (ROOT / "main/touch_history.c").read_text(encoding="utf-8")
HEADER = (ROOT / "main/touch_history.h").read_text(encoding="utf-8")
UI = (ROOT / "main/touch_history_ui.c").read_text(encoding="utf-8")


def require(source: str, pattern: str, label: str) -> None:
    if not re.search(pattern, source, re.DOTALL):
        raise AssertionError(f"missing {label}")


require(
    HEADER,
    r"touch_history_flow_bins_need_envelope\s*\(\s*uint64_t\s+duration_ms"
    r".*?uint16_t\s+point_count.*?uint32_t\s+sample_hz_x10",
    "source-density envelope decision contract",
)
require(
    SERVICE,
    r"touch_history_flow_bins_need_envelope\s*\(.*?"
    r"duration_ms\s*\*\s*sample_hz_x10\s*>\s*"
    r"\(uint64_t\)point_count\s*\*\s*10000U",
    "more raw samples than display bins selects an envelope",
)
require(
    SERVICE,
    r"if\s*\(raw_flow\s*&&\s*!touch_history_flow_bins_need_envelope\("
    r".*?span_ms.*?point_count.*?250U\)\)\s*"
    r"overview->aggregation\s*=\s*TOUCH_HISTORY_AGGREGATION_MEAN",
    "mean is restricted to point-for-point raw Flow windows",
)
require(
    SERVICE,
    r"candidate_raw\s*&&.*?TOUCH_HISTORY_AGGREGATION_MEAN.*?"
    r"history_aggregate_value.*?else\s*\{.*?aggregate->minimum.*?"
    r"aggregate->maximum",
    "raw downsampling retains both signed extrema",
)
require(
    UI,
    r"const bool envelope\s*=\s*"
    r"ui->signal\s*==\s*TOUCH_HISTORY_SIGNAL_FLOW\s*&&\s*"
    r"ui->overview\.aggregation\s*==\s*TOUCH_HISTORY_AGGREGATION_ENVELOPE\s*;",
    "raw and sidecar envelopes share min-max drawing",
)
assert "!ui->overview.source_raw" not in UI, (
    "raw source identity must not suppress its downsampled envelope"
)

print("history Flow envelope contracts passed")
