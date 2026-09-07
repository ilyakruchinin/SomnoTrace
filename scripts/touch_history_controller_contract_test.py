#!/usr/bin/env python3
"""Structural safety/behavior contract for async rich History wiring."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "main/touch_history_controller.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "main/touch_history_controller.c").read_text(encoding="utf-8")
SERVICE_H = (ROOT / "main/touch_history.h").read_text(encoding="utf-8")
SERVICE = (ROOT / "main/touch_history.c").read_text(encoding="utf-8")
UI = (ROOT / "main/touch_history_ui.c").read_text(encoding="utf-8")


def require(text: str, pattern: str, label: str) -> None:
    if not re.search(pattern, text, re.DOTALL):
        raise AssertionError(f"missing {label}")


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


def braced_block(text: str, marker: str) -> str:
    match = re.search(marker, text, re.DOTALL)
    if not match:
        raise AssertionError(f"missing block marker {marker}")
    brace = text.find("{", match.start(), match.end())
    if brace < 0:
        raise AssertionError(f"marker has no opening brace {marker}")
    depth = 1
    index = brace + 1
    while index < len(text) and depth:
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
        index += 1
    if depth:
        raise AssertionError(f"unterminated block {marker}")
    return text[brace + 1:index - 1]


for api in (
    "touch_history_controller_create",
    "touch_history_controller_destroy",
    "touch_history_controller_set_active",
    "touch_history_controller_refresh",
    "touch_history_controller_handle_intent",
    "touch_history_controller_apply",
    "touch_history_controller_revision",
):
    require(HEADER, rf"\b{api}\b", f"public {api}")
    require(SOURCE, rf"\b{api}\b", f"implementation {api}")

# One long-lived serialized mailbox and one external-stack worker. Rapid
# intents replace pending work, while the in-flight generation is immutable.
require(SOURCE, r"HISTORY_CONTROLLER_QUEUE_LENGTH\s+1U", "one-slot mailbox")
require(SOURCE, r"xQueueCreate\(", "internal serialized queue")
require(SOURCE, r"xQueueOverwrite", "newest pending job wins")
require(SOURCE, r"psram_task_create\(.*?history_controller_worker",
        "PSRAM worker stack")
assert SOURCE.count("psram_task_create(") == 1
require(SOURCE, r"heap_caps_calloc\(.*?sizeof\(\*controller\).*?"
                r"MALLOC_CAP_SPIRAM", "PSRAM controller/model")
require(SOURCE, r"heap_caps_malloc\(.*?sizeof\(\*result\).*?"
                r"MALLOC_CAP_SPIRAM", "PSRAM inactive publication model")
require(SOURCE, r"vQueueDelete\(controller->queue\).*?"
                r"vSemaphoreDelete\(controller->mutex\).*?"
                r"heap_caps_free\(controller\)",
        "kernel object teardown before PSRAM context free")
assert SOURCE.count("history_model_t model;") == 1, (
    "only the PSRAM controller field may own the 480-bin publication model"
)

require(SOURCE, r"controller->generation\+\+.*?job->generation\s*=.*?"
                r"controller->generation", "generation assignment")
require(SOURCE, r"controller->generation\s*==\s*job->generation.*?"
                r"controller->model\s*=\s*\*result", "stale publish rejection")
require(SOURCE, r"history_controller_should_cancel.*?"
                r"history_controller_generation_current",
        "cancellable initial operation")
require(SOURCE, r"job->non_cancellable.*?operation\.progress\s*=\s*NULL.*?"
                r"interruptible\s*=\s*&operation",
        "hidden-cancel ranged read remains generation-interruptible")
require(SOURCE, r"ZOOM_LOADING", "explicit zoom-loading state")

publish = function_body(SOURCE, "history_controller_publish")
reconcile_calendar = function_body(
    SOURCE, "history_controller_reconcile_calendar_locked"
)
selection_job = function_body(SOURCE, "history_controller_job_selects_night")
publish_month = braced_block(
    publish, r"if\s*\(job->kind\s*==\s*HISTORY_JOB_MONTH\)\s*\{"
)
for forbidden in (
    "controller->model.state", "progress_per_mille", "retry_job",
    "ever_loaded", "controller->model = *result",
):
    assert forbidden not in publish_month, (
        f"month publication must not own foreground field {forbidden}"
    )
for required in ("model.month", "model.has_month", "calendar_loading",
                 "calendar_read_error"):
    assert required in publish_month, f"month publication omits {required}"

publish_error = function_body(SOURCE, "history_controller_publish_error")
publish_month_error = braced_block(
    publish_error, r"if\s*\(job->kind\s*==\s*HISTORY_JOB_MONTH\)\s*\{"
)
for forbidden in ("model.state", "model.error", "retry_job"):
    assert forbidden not in publish_month_error, (
        f"month error must not own foreground field {forbidden}"
    )
for required in ("calendar_loading", "calendar_read_error"):
    assert required in publish_month_error, f"month error omits {required}"

enqueue = function_body(SOURCE, "history_controller_enqueue_locked")
enqueue_month = braced_block(enqueue, r"if\s*\(calendar_job\)\s*\{")
enqueue_wrapper = function_body(SOURCE, "history_controller_enqueue")
take_job = function_body(SOURCE, "history_controller_take_job")
for forbidden in ("controller->generation++", "retry_job", "xQueueOverwrite"):
    assert forbidden not in enqueue_month, (
        f"calendar enqueue must not mutate foreground field {forbidden}"
    )

# Complete navigation surface: initial newest selection, seven-row paging,
# calendar months, cross-page destination selection, fixed zoom tier and cursor.
require(SOURCE, r"HISTORY_JOB_INITIAL.*?result->days\[0\]\.day.*?"
                r"global_index\s*=\s*0", "newest auto-selection")
require(SOURCE, r"TOUCH_HISTORY_UI_LIST_ROWS", "seven-row page boundary")
require(SOURCE, r"HISTORY_JOB_MONTH.*?touch_history_load_month",
        "calendar month worker")
require(
    SOURCE,
    r"bool calendar;.*?\}\s*history_operation_context_t;.*?"
    r"calendar \? controller->calendar_generation == generation.*?"
    r"touch_history_load_month_ex\(.*?&operation\)",
    "month scan cancellation uses its independent calendar generation",
)
require(
    SOURCE,
    r"history_controller_begin_job.*?job->kind == HISTORY_JOB_MONTH.*?return;",
    "month reads never replace the right detail with a loading state",
)
require(
    publish,
    r"touch_history_ui_rail_mode_t rail_mode\s*=\s*"
    r"controller->model\.rail_mode;.*?controller->model\s*=\s*\*result;.*?"
    r"controller->model\.rail_mode\s*=\s*rail_mode",
    "worker publications preserve synchronous left-rail selection",
)
for selection_kind in ("HISTORY_JOB_INITIAL", "HISTORY_JOB_DAY",
                       "HISTORY_JOB_PAGE"):
    assert selection_kind in selection_job, (
        f"calendar reconciliation omits {selection_kind}"
    )
assert "history_controller_job_selects_night(job)" in reconcile_calendar
require(
    reconcile_calendar,
    r"TOUCH_HISTORY_UI_RAIL_CALENDAR.*?model\.has_night.*?"
    r"pending_matches.*?published_matches.*?calendar_loading.*?"
    r"history_controller_enqueue_locked",
    "selection publication coalesces the selected night's month",
)
assert "HISTORY_JOB_VIEW" not in selection_job, (
    "channel and zoom publications must not snap an intentionally browsed month"
)
require(
    publish,
    r"controller->retry_job = \*job;.*?"
    r"history_controller_reconcile_calendar_locked\(controller, job\)",
    "calendar alignment is part of the foreground commit",
)
require(
    SOURCE,
    r"TOUCH_HISTORY_UI_INTENT_OPEN_CALENDAR.*?"
    r"xSemaphoreTake\(controller->mutex.*?"
    r"target_day = live->has_night.*?history_controller_month_from_day\("
    r"\s*target_day.*?"
    r"TOUCH_HISTORY_UI_RAIL_CALENDAR.*?"
    r"history_controller_enqueue_locked",
    "Calendar atomically opens on the selected night's month",
)
require(
    SOURCE,
    r"TOUCH_HISTORY_UI_INTENT_OPEN_CALENDAR.*?"
    r"retry_job\.kind == HISTORY_JOB_DAY.*?"
    r"retry_job\.generation ==.*?controller->generation.*?"
    r"target_day = controller->retry_job\.day",
    "Calendar anticipates a queued row selection without waiting for publish",
)
require(
    SOURCE,
    r"TOUCH_HISTORY_UI_INTENT_CLOSE_CALENDAR.*?"
    r"controller->model\.rail_mode\s*=\s*TOUCH_HISTORY_UI_RAIL_LIST",
    "List restores the left rail without rewriting detail state",
)
require(
    publish_error,
    r"job->kind == HISTORY_JOB_MONTH.*?calendar_read_error = true",
    "calendar read failures remain scoped to the left rail",
)
require(
    publish_error,
    r"if \(job->kind != HISTORY_JOB_MONTH\)\s*"
    r"controller->retry_job = \*job",
    "calendar reads never replace the selected-night retry target",
)
require(
    take_job,
    r"xSemaphoreTake\(controller->mutex.*?xQueueReceive\(.*?"
    r"controller->month_pending.*?xSemaphoreGive\(controller->mutex\)",
    "foreground mailbox is drained before coalesced calendar work",
)
require(
    enqueue,
    r"calendar_generation\+\+.*?pending_month = \*job;.*?"
    r"month_pending = true",
    "month requests coalesce without cancelling foreground work",
)
require(
    enqueue,
    r"xQueueOverwrite\(controller->queue, job\).*?"
    r"xTaskNotifyGive\(worker\)",
    "locked enqueue publishes before waking the worker",
)
require(
    enqueue_wrapper,
    r"xSemaphoreTake\(controller->mutex.*?"
    r"history_controller_enqueue_locked\(.*?"
    r"xSemaphoreGive\(controller->mutex\)",
    "ordinary enqueue holds the destroy lifecycle lock through publication",
)
require(
    enqueue,
    r"controller->month_running.*?controller->model\.calendar_loading.*?"
    r"controller->calendar_generation\+\+.*?"
    r"controller->pending_month\s*=\s*resume.*?"
    r"controller->month_pending\s*=\s*true.*?xQueueOverwrite",
    "foreground work cancels and re-pends an in-flight calendar target",
)
require(
    SOURCE,
    r"HISTORY_JOB_INITIAL.*?month_result.*?calendar_read_error = true",
    "initial detail success retains a scoped calendar-index failure",
)
require(
    SOURCE,
    r"TOUCH_HISTORY_UI_INTENT_SELECT_CALENDAR_DAY.*?"
    r"xSemaphoreTake\(controller->mutex.*?"
    r"TOUCH_HISTORY_UI_RAIL_CALENDAR.*?!live->calendar_loading.*?"
    r"selected_year == live->month.year.*?"
    r"selected_month == live->month.month.*?"
    r"history_controller_enqueue_locked",
    "calendar day selection is live-validated and published atomically",
)
require(
    SOURCE,
    r"status == TOUCH_HISTORY_ERR_CANCELLED.*?ESP_ERR_INVALID_STATE.*?"
    r"history_controller_publish_error",
    "recording-priority cancellation cannot leave a current job loading",
)
assert "TOUCH_HISTORY_UI_STATE_CALENDAR" not in SOURCE
require(SOURCE, r"\.rail_mode\s*=\s*model->rail_mode",
        "controller forwards rail selection to UI snapshot")
require(UI, r"ui->rail_mode\s*=\s*snapshot->rail_mode",
        "UI retains controller rail selection")
require(UI, r"calendar_overlay.*?rail_mode\s*!=\s*"
            r"TOUCH_HISTORY_UI_RAIL_CALENDAR",
        "calendar visibility depends on rail mode")
require(SOURCE, r"job->global_index.*?result->page\.offset.*?"
                r"history_controller_load_view", "cross-page prev/next")
require(SOURCE, r"global_index\s*==\s*SIZE_MAX.*?"
                r"touch_history_find_day_index_ex.*?TOUCH_HISTORY_UI_LIST_ROWS",
        "calendar selection resolves its global page")
require(SOURCE, r"history_controller_load_page.*?touch_history_load_page_ex",
        "foreground index paging is generation-cancellable")
require(SOURCE, r"HISTORY_JOB_INITIAL.*?touch_history_load_month_ex.*?"
                r"month_result == TOUCH_HISTORY_ERR_CANCELLED.*?"
                r"history_controller_generation_current.*?"
                r"return month_result.*?calendar_read_error = true",
    "initial month scan separates stale cancellation from rail failure")
can_advance_month = function_body(
    SOURCE, "history_controller_can_advance_month"
)
require(
    can_advance_month,
    r"now >= 1700000000.*?model->has_newest_month.*?"
    r"model->newest_year.*?model->newest_month.*?upper_year",
    "newest indexed night is the offline forward-navigation bound",
)
require(
    SOURCE,
    r"history_controller_remember_newest_month.*?page\.offset != 0.*?"
    r"model->days\[0\]\.day.*?has_newest_month = true.*?"
    r"history_controller_load_page.*?history_controller_remember_newest_month",
    "newest month bound is retained from the newest index page",
)
for lifecycle_function in (
    "touch_history_controller_destroy",
    "touch_history_controller_set_active",
    "touch_history_controller_refresh",
):
    lifecycle = function_body(SOURCE, lifecycle_function)
    assert "calendar_generation++" in lifecycle, (
        f"{lifecycle_function} does not invalidate calendar work"
    )
    assert "month_pending = false" in lifecycle, (
        f"{lifecycle_function} does not discard pending calendar work"
    )
for producer_function in (
    "touch_history_controller_set_active",
    "touch_history_controller_refresh",
):
    producer = function_body(SOURCE, producer_function)
    require(
        producer,
        r"xSemaphoreTake\(controller->mutex.*?"
        r"history_controller_enqueue_locked\(.*?"
        r"xSemaphoreGive\(controller->mutex\)",
        f"{producer_function} decides and publishes its load atomically",
    )
require(SOURCE, r"HISTORY_CONTROLLER_ZOOM_22_MIN_MS\s*"
                r"\(22LL\s*\*\s*60LL\s*\*\s*1000LL\)", "22-minute zoom")
require(SOURCE, r"cursor_ms.*?TOUCH_HISTORY_UI_INTENT_SELECT_CHANNEL.*?"
                r"window_start_ms", "cursor retained across channel reload")

# Graph, exact stats, and event markers all use the selected visible window.
require(SOURCE, r"touch_history_load_view_ex\(.*?window_start_ms.*?"
                r"window_end_ms", "therapy-aware overview/range load")
require(SOURCE, r"touch_history_load_stats_ex\(.*?window_start_ms.*?"
                r"window_end_ms", "visible-window source stats")
require(SOURCE, r"touch_history_load_window_events_ex.*?MAX_VISIBLE_EVENTS",
        "one bounded visible-event request per window")
require(SERVICE, r"event->end_ms\s*<\s*start_ms.*?"
                r"event->start_ms\s*>=\s*end_ms",
        "visible event filtering before bounded publication")
event_loader = function_body(SOURCE, "history_controller_load_events")
assert "if (result == ESP_ERR_NOT_FOUND) return ESP_OK;" not in event_loader, (
    "missing event sources must not collapse into a zero-event success"
)
require(
    event_loader,
    r"event_total_count\s*=\s*page\.total_count.*?"
    r"page\.totals\.complete.*?EVENT_STATE_COMPLETE.*?EVENT_STATE_INCOMPLETE",
    "complete-zero versus incomplete event provenance",
)
require(
    SOURCE,
    r"events_result\s*!=\s*ESP_OK.*?event_count\s*=\s*0.*?"
    r"event_total_count\s*=\s*0.*?EVENT_STATE_UNAVAILABLE",
    "unavailable event publication",
)
require(
    SOURCE,
    r"\.event_total_count\s*=\s*model->event_total_count.*?"
    r"\.event_state\s*=\s*model->event_state.*?"
    r"\.events_truncated\s*=\s*model->events_truncated",
    "event provenance forwarded to the UI",
)
assert "Event markers are unavailable for this night." not in SOURCE, (
    "event-only availability belongs in the non-overlay marker status"
)

for signal in ("PRESSURE", "LEAK", "FLOW_LIMIT", "SNORE"):
    require(SERVICE, rf"TOUCH_HISTORY_SIGNAL_{signal}", f"{signal} stats source")
for stat in (
    "ABSOLUTE_P50", "ABSOLUTE_P95", "ABSOLUTE_P995",
    "P50", "P95", "P995", "MINIMUM", "P5", "P05",
    "TIME_BELOW_88", "MEDIAN", "MAXIMUM",
):
    assert f"TOUCH_HISTORY_STAT_{stat}" in SERVICE_H
require(SERVICE, r"HISTORY_STATS_BIN_COUNT\s+32768U.*?"
                 r"heap_caps_calloc\(.*?MALLOC_CAP_SPIRAM",
        "bounded PSRAM source histogram")
require(SERVICE, r"history_flow_raw_candidate.*?"
                 r"history_stats_accumulate_as11", "raw Flow stats")
require(SERVICE, r"scaled\s*<\s*8800.*?below_88_ms",
        "SpO2 time below 88 percent")
require(SOURCE, r"TIME_BELOW_88.*?\?\s*\"min\"", "independent duration unit")
require(SERVICE, r"signal\s*==\s*TOUCH_HISTORY_SIGNAL_MOTION.*?"
                 r"stats->loaded\s*=\s*true", "truthful unavailable Motion")

# Accepted rich graph copy and corruption hardening.
assert '"Breathing / Flow"' in UI and '"L/s"' in UI
require(SERVICE, r"Rich Flow remains source-native hundredths L/s",
        "Flow L/s scale")
require(SERVICE, r"maximum_drift_ms.*?INT64_MAX\s*-\s*drift_ms.*?"
                 r"INT64_MIN\s*-\s*drift_ms", "checked drift addition")
require(SERVICE, r"clock_drift_usable\s*&&\s*"
                 r"touch_history_apply_clock_drift\(",
        "checked drift helper used by event publication")
require(SERVICE, r"event_dropped.*?touch_history_deduplicate_events.*?"
                 r"totals->complete\s*=\s*false.*?"
                 r"totals->has_indices\s*=\s*false",
        "dropped/replayed event truthfulness")
require(SERVICE_H, r"has_session_errors.*?has_oximetry_error.*?has_summary_error.*?has_event_loss",
        "night degradation provenance")
require(SOURCE, r"has_event_loss.*?ST AHI is unavailable",
        "event-loss degraded UI copy")
require(SOURCE, r"TOUCH_HISTORY_UI_INTENT_CLEAR_CURSOR.*?cursor_valid\s*=\s*false",
        "dismissible opt-in graph cursor")
require(SOURCE, r"stats_warning.*?Percentiles unavailable.*?"
                r"model->state\s*=\s*TOUCH_HISTORY_UI_STATE_READY",
        "statistics warning does not replace the usable graph")
require(SERVICE, r"retain_overlap_owners.*?has_selected_data.*?"
                 r"qsort\(candidates", "missing-aware O2 overlap ownership")

print("touch history controller contract passed")
