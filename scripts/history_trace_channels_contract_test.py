#!/usr/bin/env python3
"""Regression contracts for the native History overnight channel picker."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HISTORY = (ROOT / "main/touch_history.c").read_text(encoding="utf-8")
HEADER = (ROOT / "main/touch_history.h").read_text(encoding="utf-8")
DISPLAY = (ROOT / "main/bsp_display_7b.c").read_text(encoding="utf-8")


def require(source: str, pattern: str, label: str) -> None:
    if not re.search(pattern, source, re.DOTALL):
        raise AssertionError(f"missing {label}")


# The retained graph is one selected 480-bin view, never eight graphs per day.
UI = (ROOT / "main/touch_history_ui.c").read_text(encoding="utf-8")
CONTROLLER = (ROOT / "main/touch_history_controller.c").read_text(encoding="utf-8")
day_model = HEADER[HEADER.index("typedef struct {", HEADER.index("touch_history_trace_t")):
                   HEADER.index("} touch_history_day_t;")]
assert "points[" not in day_model, "every History day must not own samples"
for signal in (
    "FLOW", "PRESSURE", "LEAK", "FLOW_LIMIT", "SNORE",
    "SPO2", "PULSE", "MOTION",
):
    assert f"TOUCH_HISTORY_SIGNAL_{signal}" in HEADER
require(HEADER, r"TOUCH_HISTORY_OVERVIEW_POINTS\s+480", "480-bin retained view")
require(HEADER, r"value_x100\[TOUCH_HISTORY_OVERVIEW_POINTS\].*?"
                r"upper_x100\[TOUCH_HISTORY_OVERVIEW_POINTS\].*?"
                r"companion_x100\[TOUCH_HISTORY_OVERVIEW_POINTS\]",
        "value, Flow envelope, and Pressure EPR arrays")

# Rich Flow stays source-native L/s. Accepted 22-minute zooms prefer raw 25 Hz
# and the min/max sidecar remains an honest fallback.
require(HISTORY, r"history_flow_raw_candidate.*?history_accumulate_as11",
        "raw Flow candidate")
require(HISTORY, r"touch_history_flow_range_prefers_raw.*?"
                 r"22ULL\s*\*\s*60ULL\s*\*\s*1000ULL.*?"
                 r"night_duration_ms\s*/\s*4U",
        "raw Flow preference includes 22-minute window")
require(HISTORY, r"Rich Flow remains source-native hundredths L/s",
        "Flow L/s unit preservation")
require(HISTORY, r"TOUCH_HISTORY_AGGREGATION_ENVELOPE.*?upper_x100",
        "sidecar min/max envelope")
assert '"Breathing / Flow"' in UI and '"L/s"' in UI

# Canonical SpO2 uses status-good samples and deterministic multi-recording
# overlap ownership. Missing spans remain gaps rather than zero-valued data.
require(HISTORY, r"OXIMETRY_CANONICAL_VITALS_SPO2.*?"
                 r"OXIMETRY_CANONICAL_VITALS_STATUS.*?&\s*1U",
        "canonical SpO2 value and quality gate")
require(HISTORY, r"retain_overlap_owners.*?has_selected_data.*?"
                 r"qsort\(candidates", "missing-aware O2 overlap ownership")

# One serialized generation-safe worker publishes only complete current views.
require(CONTROLLER, r"HISTORY_CONTROLLER_QUEUE_LENGTH\s+1U.*?xQueueOverwrite",
        "one-slot latest-intent worker")
require(CONTROLLER, r"controller->generation\s*==\s*job->generation.*?"
                    r"controller->model\s*=\s*\*result",
        "stale result rejection")
require(CONTROLLER, r"job->non_cancellable.*?operation\.progress\s*=\s*NULL.*?"
                    r"interruptible\s*=\s*&operation",
        "hidden-cancel zoom remains generation-interruptible")
require(CONTROLLER, r"touch_history_load_view_ex\(.*?window_start_ms.*?"
                    r"window_end_ms", "selected-window reread")

# Rendering is one bounded custom-draw object with gaps, event markers,
# session captions, cursor, and the Pressure companion.
assert "lv_chart_create" not in UI
require(UI, r"history_ui_graph_draw.*?TOUCH_HISTORY_POINT_VALID.*?"
            r"TOUCH_HISTORY_POINT_COMPANION_VALID.*?"
            r"SESSION %u.*?cursor_x",
        "bounded rich graph trace, session, and cursor layers")
require(UI, r"history_ui_draw_event_lane.*?"
            r"history_ui_event_color\(marker->type\).*?history_ui_draw_rect",
        "bounded source-colored square event lane")
require(UI, r"history_ui_row_pressed.*?LV_EVENT_SHORT_CLICKED",
        "night selection waits for scroll arbitration")

print("history trace channel contract passed")
