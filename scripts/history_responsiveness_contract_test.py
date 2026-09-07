#!/usr/bin/env python3
"""Structural contracts for bounded native History touch and redraw work."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
UI = (ROOT / "main/touch_history_ui.c").read_text(encoding="utf-8")
CONTROLLER = (ROOT / "main/touch_history_controller.c").read_text(
    encoding="utf-8"
)


def function_body(text: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", text, re.DOTALL)
    if not match:
        raise AssertionError(f"missing function body {name}")
    depth = 1
    index = match.end()
    while index < len(text) and depth:
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
        index += 1
    if depth:
        raise AssertionError(f"unterminated function body {name}")
    return text[match.start():index]


graph_touch = function_body(UI, "history_ui_graph_touch")
finish_pan = function_body(UI, "history_ui_finish_graph_pan")
load_view = function_body(CONTROLLER, "history_controller_load_view")
progress = function_body(CONTROLLER, "history_controller_progress")
apply_snapshot = function_body(UI, "touch_history_ui_apply")
handle_intent = function_body(CONTROLLER, "touch_history_controller_handle_intent")

# A drag accumulates framebuffer-local displacement and dispatches exactly one
# storage-backed pan only when the gesture terminates.
assert "graph_drag_delta_x" in graph_touch
assert "history_ui_finish_graph_pan(ui)" in graph_touch
assert "TOUCH_HISTORY_UI_INTENT_PAN_RELATIVE" not in graph_touch
assert finish_pan.count("TOUCH_HISTORY_UI_INTENT_PAN_RELATIVE") == 1
assert "LV_EVENT_RELEASED" in UI
assert "LV_EVENT_PRESS_LOST" in UI
assert "ui->graph_dragged = false" not in finish_pan
assert re.search(
    r"LV_EVENT_SHORT_CLICKED.*?if\s*\(ui->graph_dragged\)\s*\{.*?"
    r"ui->graph_dragged\s*=\s*false;.*?return;",
    graph_touch,
    re.DOTALL,
)
assert re.search(
    r"TOUCH_HISTORY_UI_INTENT_PAN_RELATIVE.*?"
    r"start\s*!=\s*model\.window_start_ms\s*\|\|\s*"
    r"end\s*!=\s*model\.window_end_ms.*?history_controller_queue_view",
    handle_intent,
    re.DOTALL,
)

# Hiding Cancel is a presentation decision, not permission for stale reads to
# continue after a newer intent or after History is deactivated.
assert "operation.progress = NULL" in load_view
assert "const touch_history_operation_t *interruptible = &operation" in load_view
assert "? NULL : &operation" not in load_view
for loader in (
    "touch_history_load_night_ex",
    "touch_history_load_view_ex",
    "touch_history_load_stats_ex",
    "history_controller_load_events",
):
    assert loader in load_view and "interruptible" in load_view

# Visible initial-load progress is coalesced, hidden overlay progress is muted,
# and unchanged graph content is not explicitly invalidated a second time.
assert "HISTORY_CONTROLLER_PROGRESS_STEP" in progress
assert "meaningful_step" in progress
assert "history_ui_graph_content_changed" in apply_snapshot
assert re.search(
    r"if\s*\(graph_content_changed\)\s*lv_obj_invalidate\(ui->graph\)",
    apply_snapshot,
)

print("history responsiveness contract passed")
