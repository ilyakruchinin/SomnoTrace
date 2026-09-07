#!/usr/bin/env python3
"""Structural contracts for the bounded 768x450 native Logs pane."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "main/touch_logs_ui.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "main/touch_logs_ui.c").read_text(encoding="utf-8")


def require(text: str, pattern: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(f"missing native Logs contract: {description}")


# The pane fits the Manage detail card, and every explicit action is at least
# the firmware-wide 44-pixel touch target.
for name, value in (
    ("TOUCH_LOGS_UI_WIDTH", "768U"),
    ("TOUCH_LOGS_UI_HEIGHT", "450U"),
    ("TOUCH_LOGS_UI_VISIBLE_ROWS", "10U"),
):
    require(HEADER, rf"#define\s+{name}\s+{value}\b", name)
require(SOURCE, r"#define\s+UI_TOUCH_MIN\s+44\b", "44-pixel touch minimum")
require(SOURCE, r"if\s*\(height\s*<\s*UI_TOUCH_MIN\)",
        "enforced touch minimum")
for geometry in (
    r"UI_HEADER_HEIGHT\s+64\b",
    r"UI_FILTER_Y\s+64\b",
    r"UI_VIEW_X\s+16\b",
    r"UI_VIEW_Y\s+118\b",
    r"UI_VIEW_WIDTH\s+736\b",
    r"UI_VIEW_HEIGHT\s+286\b",
    r"UI_FOOTER_Y\s+404\b",
):
    require(SOURCE, geometry, f"handoff geometry {geometry}")

# Snapshot input is the exact bounded result/filter/page/info produced around
# log_stream_retained_snapshot_page(), without coupling presentation to it.
for member in (
    r"esp_err_t\s+snapshot_result",
    r"const\s+log_stream_retained_line_t\s*\*\s*lines",
    r"log_stream_retained_filter_t\s+filter",
    r"log_stream_retained_page_t\s+page",
    r"log_stream_retained_info_t\s+info",
    r"uint64_t\s+pause_anchor_total_count",
):
    require(HEADER, member, f"retained snapshot field {member}")
require(SOURCE,
        r"filter\.order\s*!=\s*LOG_STREAM_RETAINED_NEWEST_FIRST",
        "newest-first invariant")
require(SOURCE,
        r"page\.returned\s*!=\s*snapshot->line_count",
        "page/visible-row consistency check")
assert "log_stream_retained_snapshot_page(" not in SOURCE, (
    "the presentation module must not perform retained scans"
)
assert "log_stream_retained_save_to_sd(" not in SOURCE, (
    "the LVGL module must not write the card"
)

# Every external intent has a controller callback and is reached only through
# the shared touch-down event handler.
callbacks = (
    "set_paused", "begin_search", "search_query", "toggle_level",
    "page_older", "page_newer", "jump_newest", "clear_ram_only",
    "save_card_snapshot", "retry_connection",
)
for callback in callbacks:
    require(HEADER, rf"\(\*{callback}\)\s*\(", f"{callback} callback")
    require(SOURCE, rf"controller\.{callback}", f"{callback} intent")
require(SOURCE,
        r"action_cb\(lv_event_t\s*\*event\).*?"
        r"lv_event_get_code\(event\)\s*!=\s*LV_EVENT_PRESSED",
        "PRESSED-only controller dispatch")
event_registrations = re.findall(
    r"lv_obj_add_event_cb\([^;]+;", SOURCE, re.DOTALL
)
assert event_registrations, "no LVGL interactions registered"
gesture_registrations = [
    call for call in event_registrations if "LV_EVENT_GESTURE" in call
]
assert len(gesture_registrations) == 1 and "viewport_gesture_cb" in gesture_registrations[0], (
    "the fixed viewport should own the one vertical paging gesture"
)
assert all(
    "LV_EVENT_PRESSED" in call or "viewport_gesture_cb" in call or
    ("s_ui->keyboard, keyboard_input_cb" in call and any(
        event in call for event in ("LV_EVENT_VALUE_CHANGED", "LV_EVENT_READY", "LV_EVENT_CANCEL")))
    for call in event_registrations
), "only paging gestures and keyboard input may bypass physical PRESSED registration"
require(SOURCE, r'keyboard_input_cb.*?strcmp\(text, "space"\).*?lv_textarea_add_char',
        "shared keyboard space legend inserts a space")
require(SOURCE, r'keyboard_input_cb.*?LV_SYMBOL_UP.*?LV_KEYBOARD_MODE_TEXT_UPPER',
        "shared keyboard Shift legend changes case")
require(SOURCE, r"lv_obj_clear_flag\(s_ui->viewport, LV_OBJ_FLAG_GESTURE_BUBBLE\)",
        "gesture routing terminates at the retained viewport")
require(SOURCE,
        r"viewport_gesture_cb.*?LV_DIR_TOP.*?"
        r"lv_event_send\(s_ui->older_button,\s*LV_EVENT_PRESSED.*?"
        r"LV_DIR_BOTTOM.*?"
        r"lv_event_send\(s_ui->newer_button,\s*LV_EVENT_PRESSED",
        "vertical swipes translated to PRESSED older/newer intents")

# There are exactly ten reusable row records. Steady updates never construct,
# delete, clean, or allocate an object per retained ring line.
require(SOURCE,
        r"log_row_ui_t\s+rows\[TOUCH_LOGS_UI_VISIBLE_ROWS\]",
        "ten reusable row handles")
require(SOURCE,
        r"for\s*\(size_t\s+i\s*=\s*0;\s*"
        r"i\s*<\s*TOUCH_LOGS_UI_VISIBLE_ROWS;\s*\+\+i\)",
        "bounded row construction/update")
assert "2048" not in SOURCE and "2 048" not in SOURCE, (
    "ring capacity must never become an LVGL object count"
)
assert "lv_obj_clean" not in SOURCE, "updates must reuse the object tree"
update_body = re.search(
    r"esp_err_t\s+touch_logs_ui_update\([^)]*\)\s*\{(.*?)\n\}",
    SOURCE, re.DOTALL,
)
assert update_body and "_create(" not in update_body.group(1), (
    "steady page updates must not create LVGL objects"
)

# Context allocation is lazy, PSRAM-only and fully releasable.
require(SOURCE, r"static\s+touch_logs_ui_t\s*\*\s*s_ui",
        "lazy singleton pointer")
require(SOURCE,
        r"touch_logs_ui_create.*?heap_caps_calloc\(.*?"
        r"MALLOC_CAP_SPIRAM\s*\|\s*MALLOC_CAP_8BIT",
        "PSRAM-only lazy context")
assert SOURCE.count("heap_caps_calloc(") == 1, (
    "internal-RAM allocation fallback is forbidden"
)
require(SOURCE,
        r"touch_logs_ui_destroy.*?lv_obj_del.*?heap_caps_free",
        "complete release path")
require(SOURCE,
        r"touch_logs_ui_create.*?LV_OBJ_FLAG_HIDDEN.*?render_all\(\)",
        "complete hidden first render")
require(SOURCE,
        r"touch_logs_ui_show.*?render_all\(\).*?"
        r"lv_obj_clear_flag\(s_ui->root,\s*LV_OBJ_FLAG_HIDDEN\)",
        "complete first visible frame")

# Four stable columns use the compiled data face. Fixed x positions ensure tag
# length and live movement never shift the message column.
for x in ("21", "125", "183", "285"):
    require(SOURCE, rf"make_label\(row->surface,.*?\n\s*{x},",
            f"fixed log column at x={x}")
assert "somnotrace_space_grotesk" in SOURCE, "Space Grotesk UI font missing"
assert "somnotrace_ibm_plex_mono" in SOURCE, "IBM Plex Mono row font missing"
for cue in ("accent", "COLOR_ERROR_ROW", "COLOR_WARN_ROW", "level_color"):
    assert cue in SOURCE, f"missing redundant severity cue {cue}"

# State copy and invariants: ring capture continues while the viewport is
# paused; search dismissal and jump-to-newest never resume implicitly.
for copy in (
    "Live · %u-line ring buffer",
    "Paused · %",
    "Search paused · %",
    "No lines match “%s”",
    "Active levels:",
    "Debug is off — enable Debug",
    "Log stream disconnected",
    "This does not stop therapy or recording",
    '"Clear"',
    "Clear affects RAM only",
    "Save to card",
    "Jump to newest",
):
    assert copy in SOURCE, f"missing required state copy: {copy}"
assert '"Filter by tag or message"' in SOURCE, "handoff search placeholder drifted"
require(SOURCE,
        r"ACTION_SEARCH_FOCUS:.*?view_forced_pause\s*=\s*true",
        "search focus permanently pauses until explicit Resume")
search_done = re.search(
    r"case\s+ACTION_SEARCH_DONE:(.*?)case\s+ACTION_SEARCH_CLEAR:",
    SOURCE, re.DOTALL,
)
assert search_done and "view_forced_pause = false" not in search_done.group(1), (
    "search dismissal must not resume"
)
jump = re.search(
    r"case\s+ACTION_JUMP_NEWEST:(.*?)case\s+ACTION_CLEAR_RAM:",
    SOURCE, re.DOTALL,
)
assert jump and "paused = false" not in jump.group(1), (
    "jump-to-newest must remain paused"
)
require(SOURCE,
        r"info\.total_count\s*-\s*s_ui->pause_anchor_total_count",
        "exact new-line count while the monotonic ring continues")

# Card save is independent of stream connectivity and reports determinate
# progress plus a truthful success/failure receipt.
for field in (
    "save_processed_lines", "save_total_lines", "saved_line_count",
    "save_result", "saved_path", "save_error",
):
    assert field in HEADER, f"missing save status field {field}"
require(SOURCE, r"lv_bar_set_range\(s_ui->save_progress,\s*0,\s*100\)",
        "determinate 0-100 progress")
require(SOURCE,
        r"save_processed_lines.*?100U.*?save_total_lines",
        "progress derived from processed/total")
for copy in ("Saving retained snapshot… %d%%", "Saved %u lines", "Save failed · %s"):
    assert copy in SOURCE, f"missing save receipt: {copy}"

print("native 768x450 Logs UI contract passed")
