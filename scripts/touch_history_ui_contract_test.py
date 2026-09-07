#!/usr/bin/env python3
"""Structural contracts for the bounded Rev B native History surface."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "main/touch_history_ui.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "main/touch_history_ui.c").read_text(encoding="utf-8")


def require(text: str, pattern: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(f"missing History UI contract: {description}")


# The surface consumes the exact persistent-content geometry.
for pattern, description in (
    (r"TOUCH_HISTORY_UI_WIDTH\s+992\b", "992-pixel content width"),
    (r"TOUCH_HISTORY_UI_HEIGHT\s+450\b", "450-pixel content height"),
    (r"TOUCH_HISTORY_UI_LIST_WIDTH\s+288\b", "288-pixel list"),
    (r"TOUCH_HISTORY_UI_COLUMN_GAP\s+12\b", "12-pixel gutter"),
    (r"TOUCH_HISTORY_UI_DETAIL_X\s+300\b", "detail x coordinate"),
    (r"TOUCH_HISTORY_UI_DETAIL_WIDTH\s+692\b", "692-pixel detail"),
    (r"HISTORY_UI_HIT\s+44\b", "44-pixel minimum controls"),
    (r"HISTORY_UI_SUMMARY_H\s+44\b", "44-pixel summary strip"),
    (r"HISTORY_UI_LIST_ROW_H\s+60\b", "60-pixel recycled rows"),
):
    require(HEADER + SOURCE, pattern, description)

# Object count stays bounded on an ESP32: seven recycled rows, eight recycled
# channel controls, one graph draw object, and no chart/canvas point objects.
require(HEADER, r"TOUCH_HISTORY_UI_LIST_ROWS\s+7\b", "seven list rows")
require(
    HEADER,
    r"TOUCH_HISTORY_UI_CHANNEL_CONTROLS\s+TOUCH_HISTORY_SIGNAL_COUNT",
    "all eight channel controls",
)
require(
    SOURCE,
    r"rows\[TOUCH_HISTORY_UI_LIST_ROWS\].*?"
    r"channels\[TOUCH_HISTORY_UI_CHANNEL_CONTROLS\]",
    "fixed object arrays",
)
require(
    SOURCE,
    r"for\s*\(size_t i = 0; i < TOUCH_HISTORY_UI_LIST_ROWS; \+\+i\)",
    "reused row creation loop",
)
require(
    SOURCE,
    r"for\s*\(size_t i = 0; i < TOUCH_HISTORY_UI_CHANNEL_CONTROLS; \+\+i\)",
    "reused channel creation loop",
)
assert "lv_chart_create" not in SOURCE, "History must not allocate an LVGL chart"
assert "lv_canvas_create" not in SOURCE, "History must not allocate a framebuffer canvas"
assert SOURCE.count("history_ui_graph_draw,") == 1, "exactly one graph draw object"

# The handoff's left rail is a List/Calendar segmented control over a clipped,
# scroll-recycled list. It never regresses to a numeric pager footer.
for copy in ("List", "Calendar", "RECORDED\\nNIGHTS", "Jump to date"):
    assert copy in SOURCE, f"missing left-rail copy {copy}"
require(
    SOURCE,
    r"list_viewport.*?LV_OBJ_FLAG_SCROLLABLE.*?"
    r"LV_EVENT_SCROLL_END",
    "scroll-driven page recycling",
)
assert "page_previous" not in SOURCE
assert "page_next" not in SOURCE
assert "page_label" not in SOURCE
require(
    SOURCE,
    r"channels\[i\]\.pill = history_ui_container\(.*?,\s*31,",
    "31-pixel visual channel pill inside its 44-pixel target",
)
assert "≥4 h adherence" in SOURCE and "usage_target_known" in HEADER
require(
    SOURCE,
    r"history_ui_format_day_compact.*?"
    r'"Jan", "Feb", "Mar".*?"Sun", "Mon", "Tue".*?'
    r'snprintf\(output, capacity, "%s %u %s %u"',
    "compact weekday/date/month/year formatter",
)
require(
    SOURCE,
    r"lv_txt_get_size\(&title_size, title.*?"
    r"title_size\.x > lv_obj_get_content_width\(ui->night_title\).*?"
    r"history_ui_format_day_compact",
    "full date falls back responsively to Tue 1 Sep 2026",
)
require(
    SOURCE,
    r"ui->night_title = history_ui_label\(.*?"
    r"lv_label_set_long_mode\(ui->night_title, LV_LABEL_LONG_CLIP\)",
    "selected-night title never renders an ellipsis",
)

# Calendar replaces only the left-rail content below the persistent segmented
# control. The selected-night detail remains mounted and visible on the right.
require(
    SOURCE,
    r"if\s*\(ui->calendar_overlay\)\s*return ESP_OK",
    "lazy calendar allocation guard",
)
require(
    SOURCE,
    r"history_ui_discard_calendar.*?lv_obj_del\(ui->calendar_overlay\).*?"
    r"ui->calendar_grid = NULL.*?ui->calendar_status = NULL",
    "partial calendar allocation is discarded transactionally",
)
require(
    SOURCE,
    r"history_ui_ensure_calendar.*?goto no_memory;.*?no_memory:\s*"
    r"history_ui_discard_calendar\(ui\);\s*return ESP_ERR_NO_MEM",
    "calendar allocation failures leave no partial subtree",
)
require(
    SOURCE,
    r"rail_mode == TOUCH_HISTORY_UI_RAIL_CALENDAR.*?"
    r"history_ui_ensure_calendar",
    "calendar allocation only in calendar rail mode",
)
require(
    SOURCE,
    r"ui->calendar_overlay = history_ui_container\(\s*"
    r"ui->left,\s*0,\s*HISTORY_UI_CALENDAR_Y,\s*"
    r"TOUCH_HISTORY_UI_LIST_WIDTH,\s*HISTORY_UI_CALENDAR_H",
    "calendar content belongs to the 288-pixel left rail",
)
require(
    SOURCE,
    r"bool calendar = ui->rail_mode == TOUCH_HISTORY_UI_RAIL_CALENDAR;.*?"
    r"history_ui_set_hidden\(ui->list_title, calendar\);.*?"
    r"history_ui_set_hidden\(ui->jump_to_date, calendar\);.*?"
    r"history_ui_set_hidden\(ui->list_viewport, calendar\);.*?"
    r"if \(calendar\)\s*return;.*?"
    r"for \(size_t i = 0; i < TOUCH_HISTORY_UI_LIST_ROWS; \+\+i\)",
    "calendar swaps only the list content beneath the segment",
)
assert "calendar_close" not in SOURCE, "List is the calendar's only close control"
assert "lv_obj_move_foreground(ui->calendar_overlay)" not in SOURCE
assert '{"M", "T", "W", "T", "F", "S", "S"}' in SOURCE
require(
    SOURCE,
    r"selected_date_valid.*?selected_year == ui->month.year.*?"
    r"selected_month == ui->month.month.*?selected_day == \(unsigned\)day",
    "selected calendar date remains highlighted",
)
require(
    SOURCE,
    r"history_ui_calendar_grid_pressed.*?ui->calendar_loading.*?return;",
    "calendar ignores stale grid coordinates while a month is loading",
)
assert SOURCE.count("ui->calendar_grid = history_ui_container(") == 1
assert "42 calendar cells from becoming 42 objects" in SOURCE
require(
    SOURCE,
    r"history_ui_calendar_grid_content_changed.*?"
    r"month\.therapy_days != snapshot->month->therapy_days.*?"
    r"month\.oximetry_days != snapshot->month->oximetry_days",
    "calendar redraws only when semantic month content changes",
)
require(
    SOURCE,
    r"history_ui_update_calendar\(.*?bool grid_content_changed\).*?"
    r"if \(grid_content_changed\)\s*lv_obj_invalidate\(ui->calendar_grid\)",
    "unrelated snapshots do not redraw the calendar grid",
)
require(
    SOURCE,
    r"history_ui_set_hidden.*?lv_obj_has_flag\(object, LV_OBJ_FLAG_HIDDEN\)"
    r" == hidden.*?return;",
    "unchanged visibility does not dirty the LVGL tree",
)
require(
    SOURCE,
    r"history_ui_set_enabled.*?currently_enabled == enabled.*?return;",
    "unchanged enabled state does not dirty the LVGL tree",
)

# The retained 480-point model and all copied arrays live in PSRAM only.
require(
    SOURCE,
    r"heap_caps_calloc\(\s*1,\s*sizeof\(\*ui\),\s*"
    r"MALLOC_CAP_SPIRAM\s*\|\s*MALLOC_CAP_8BIT\)",
    "PSRAM-only context allocation",
)
assert SOURCE.count("heap_caps_calloc(") == 1, "internal-RAM fallback is forbidden"
assert re.search(r"touch_history_overview_t\s+overview;", SOURCE)
require(
    SOURCE,
    r"ui->overview\s*=\s*\*snapshot->overview",
    "deep-copied graph snapshot",
)
require(SOURCE, r"heap_caps_free\(ui\)", "PSRAM release")

# All required worker/BSP states and touch intents are explicit and typed.
for state in (
    "EMPTY",
    "AUTO_LOADING",
    "READY",
    "ZOOM_LOADING",
    "READ_ERROR",
    "DEGRADED_UNKNOWN",
):
    assert f"TOUCH_HISTORY_UI_STATE_{state}" in HEADER, f"missing state {state}"

for rail_mode in ("LIST", "CALENDAR"):
    assert f"TOUCH_HISTORY_UI_RAIL_{rail_mode}" in HEADER, (
        f"missing History rail mode {rail_mode}"
    )

for event_state in ("UNAVAILABLE", "COMPLETE", "INCOMPLETE"):
    assert f"TOUCH_HISTORY_UI_EVENT_STATE_{event_state}" in HEADER, (
        f"missing event provenance state {event_state}"
    )
for field in ("event_total_count", "event_state", "events_truncated"):
    assert field in HEADER, f"missing event-lane snapshot field {field}"

for intent in (
    "SELECT_DAY",
    "PAGE_RELATIVE",
    "OPEN_CALENDAR",
    "CLOSE_CALENDAR",
    "MONTH_RELATIVE",
    "SELECT_CALENDAR_DAY",
    "SELECT_CHANNEL",
    "PREVIOUS_NIGHT",
    "NEXT_NIGHT",
    "CANCEL_AUTO_LOAD",
    "RETRY_READ",
    "OPEN_CARD",
    "FIT_NIGHT",
    "ZOOM_RELATIVE",
    "PAN_RELATIVE",
    "SET_CURSOR",
    "CLEAR_CURSOR",
    "TOGGLE_THERAPY_ONLY",
):
    assert f"TOUCH_HISTORY_UI_INTENT_{intent}" in HEADER, f"missing intent {intent}"

require(
    SOURCE,
    r"history_ui_cancel_pressed.*?"
    r"ui->state != TOUCH_HISTORY_UI_STATE_AUTO_LOADING",
    "cancel is restricted to initial night load",
)
require(
    SOURCE,
    r"LV_EVENT_SHORT_CLICKED.*?TOUCH_HISTORY_UI_INTENT_CLEAR_CURSOR",
    "repeat short tap dismisses the graph cursor without stealing pan",
)
require(
    SOURCE,
    r"stats_warning_text.*?stats_warning.*?"
    r"history_ui_set_hidden\(ui->stat_labels",
    "stats-only warning is compact in the header rather than graph overlay",
)
require(
    SOURCE,
    r"TOUCH_HISTORY_UI_STATE_ZOOM_LOADING.*?"
    r"Intentionally no cancel control exists in this overlay",
    "updating status has no blocking cancellation panel",
)
for callback in (
    "history_ui_channel_pressed",
    "history_ui_open_calendar_pressed",
    "history_ui_retry_pressed",
    "history_ui_fit_pressed",
    "history_ui_zoom_in_pressed",
):
    require(
        SOURCE,
        rf"{callback}.*?LV_EVENT_PRESSED",
        f"{callback} emits from touch-down",
    )
require(
    SOURCE,
    r"row->button,\s*history_ui_row_pressed,\s*"
    r"LV_EVENT_SHORT_CLICKED",
    "scrollable rows select only after a short click",
)
assert not re.search(
    r"row->button,\s*history_ui_row_pressed,\s*LV_EVENT_PRESSED",
    SOURCE,
), "row touch-down must not race scroll arbitration"
assert "history_ui_list_scroll_end" in SOURCE

# Truthful graph states: full-night Flow envelope, 22-minute raw zoom source,
# Pressure/EPR companion, therapy-only SpO2, explicit gaps and event taxonomy.
assert '"Breathing / Flow"' in SOURCE, "accepted rich Flow title is required"
assert '"L/s"' in SOURCE, "rich Flow must remain source-native L/s"
assert "source_raw" in SOURCE and "raw 25 Hz" in SOURCE
assert "1 Hz fallback" in SOURCE
assert "TOUCH_HISTORY_AGGREGATION_ENVELOPE" in SOURCE
assert "TOUCH_HISTORY_POINT_UPPER_VALID" in SOURCE
assert "TOUCH_HISTORY_POINT_COMPANION_VALID" in SOURCE
assert "Pressure + EPR" in SOURCE
assert "availability_y" in SOURCE, "O2 channels need a binary availability strip"
require(
    SOURCE,
    r"signal == TOUCH_HISTORY_SIGNAL_SPO2.*?therapy_only.*?"
    r"TOUCH_HISTORY_POINT_THERAPY",
    "SpO2 during-therapy-only filter",
)
require(
    SOURCE,
    r"else\s*\{\s*previous = SIZE_MAX;\s*\}",
    "missing bins break trace segments",
)
assert "TOUCH_HISTORY_EVENT_GENERIC_APNEA" in SOURCE
assert '"A"' in SOURCE, "generic apnea keeps a distinct marker"
assert "cursor_valid" in SOURCE and "history_ui_cursor_event" in SOURCE
for rich_graph_copy in (
    "SESSION %u · %s–%s",
    "Markers: OA · CA · H · A · RERA",
    "No OA/CA/H/A/RERA events recorded",
    "Event data unavailable",
    "Event markers truncated · zoom in",
    "Trend review only. Not a diagnosis or a prescription.",
):
    assert rich_graph_copy in SOURCE, f"missing rich graph detail: {rich_graph_copy}"
require(
    SOURCE,
    r"history_ui_draw_event_lane.*?marker->end_ms.*?"
    r"history_ui_draw_rect\(draw_ctx, &marker_area, color, LV_OPA_COVER",
    "source-timestamped filled-square event markers",
)
require(
    SOURCE,
    r"history_ui_draw_event_lane\(ui, draw_ctx, left, right, lane_y\).*?"
    r"if \(!ui->has_overview \|\| !ui->overview\.loaded",
    "event lane remains visible when the selected trace is empty",
)
require(
    SOURCE,
    r"event_state == TOUCH_HISTORY_UI_EVENT_STATE_COMPLETE.*?"
    r"event_total_count == 0.*?No OA/CA/H/A/RERA events recorded.*?"
    r"event_count == 0.*?No respiratory events in this window",
    "complete-zero and empty-window event copy",
)
require(
    SOURCE,
    r"EVENT_STATE_UNAVAILABLE.*?event_count \|\| snapshot->event_total_count",
    "unavailable event data cannot carry a false count",
)
require(
    SOURCE,
    r"for\s*\(unsigned tick = 0; tick <= 4; \+\+tick\).*?"
    r"history_ui_format_clock",
    "five labelled time ticks",
)
require(
    SOURCE,
    r"minimum = -magnitude;\s*maximum = magnitude;",
    "Flow axis includes an exact zero tick",
)

# Page/month metadata avoids the old 30-night product cap. Unknown stays an
# em dash, and ST AHI never silently replaces the dim Device AHI.
assert "TOUCH_HISTORY_MAX_DAYS" not in HEADER + SOURCE
assert "page.total_days" in SOURCE and "page.has_more" in SOURCE
assert r'"\xE2\x80\x94"' in SOURCE
for label in ("Usage", "ST AHI", "Device AHI", "Recorded", "O₂ coverage"):
    assert f'"{label}"' in SOURCE, f"missing summary field {label}"
require(
    SOURCE,
    r"i == 2 \? HISTORY_UI_COLOR_TERTIARY",
    "Device AHI is visually secondary",
)
assert "stats[TOUCH_HISTORY_UI_STAT_COUNT]" in HEADER
assert "not invent percentiles" in HEADER
require(HEADER, r"TOUCH_HISTORY_UI_STAT_COUNT\s+4\b", "four SpO2 stat slots")
for label in (
    "P50 |Flow|", "P95 |Flow|", "P99.5 |Flow|",
    "P50", "P95", "P99.5", "Minimum", "P5", "P0.5", "Time <88%",
    "Median", "Maximum",
):
    assert f'"{label}"' in SOURCE, f"missing source-stat label {label}"
require(
    SOURCE,
    r"i < 3 \|\| ui->signal == TOUCH_HISTORY_SIGNAL_SPO2",
    "fourth stat is reserved for SpO2",
)
assert "const char *unit;" in HEADER, "time-below-88 needs a duration unit"
assert "history_ui_set_hidden(ui->graph_title" not in SOURCE
assert "history_ui_set_hidden(ui->graph_source" not in SOURCE

# Snapshot bounds are rejected, not truncated, and zoom can retain the last
# resolved plot while the raw SD reread runs.
for bound in (
    "TOUCH_HISTORY_UI_LIST_ROWS",
    "TOUCH_HISTORY_UI_MAX_SESSIONS",
    "TOUCH_HISTORY_UI_MAX_VISIBLE_EVENTS",
):
    require(
        SOURCE,
        rf"snapshot->(?:day_count|session_count|event_count) > {bound}",
        f"reject overflow for {bound}",
    )
require(
    SOURCE,
    r"snapshot->overview.*?state != TOUCH_HISTORY_UI_STATE_ZOOM_LOADING",
    "retain resolved waveform during zoom read",
)

print("Rev B native History UI contract passed")
