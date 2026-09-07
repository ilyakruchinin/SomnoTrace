#!/usr/bin/env python3
"""Contracts for the independent native-touchscreen retained log feed."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/log_stream.c").read_text(encoding="utf-8")
HEADER = (ROOT / "main/log_stream.h").read_text(encoding="utf-8")
HOST_TEST = (ROOT / "scripts/test-host.sh").read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if not match:
        raise AssertionError(f"missing function: {name}")
    depth = 1
    cursor = match.end()
    while cursor < len(source) and depth:
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
        cursor += 1
    if depth:
        raise AssertionError(f"unterminated function: {name}")
    return source[match.end(): cursor - 1]


hook = function_body(SOURCE, "log_vprintf_hook")
append = function_body(SOURCE, "retained_append_line")
capture = function_body(SOURCE, "retained_capture_text")
initialise = function_body(SOURCE, "retained_init")
copy_slot = function_body(SOURCE, "retained_copy_slot")
matching = function_body(SOURCE, "retained_filter_matches")
snapshot = function_body(SOURCE, "log_stream_retained_snapshot")
snapshot_page = function_body(SOURCE, "log_stream_retained_snapshot_page")
snapshot_core = function_body(SOURCE, "retained_snapshot_from_bounds")
clear = function_body(SOURCE, "log_stream_retained_clear")
retry = function_body(SOURCE, "log_stream_retained_retry")
save = function_body(SOURCE, "log_stream_retained_save_to_sd")
publish = function_body(SOURCE, "retained_publish_snapshot")

# The public API is caller-owned and reports both monotonic change tokens and
# degradation. A zero-initialised filter is useful without special setup.
for symbol in (
    "log_stream_retained_get_info",
    "log_stream_retained_snapshot",
    "log_stream_retained_snapshot_page",
    "log_stream_retained_retry",
    "log_stream_retained_clear",
    "log_stream_retained_save_to_sd",
    "generation",
    "total_count",
    "retained_span_ms",
    "dropped_count",
    "last_error",
    "available",
    "in_psram",
    "log_stream_retained_progress_fn",
    "after_sequence",
    "before_sequence",
    "matching_count",
    "match_offset",
    "level_mask",
    "query",
):
    assert symbol in HEADER, f"retained API is missing {symbol}"
assert "LOG_STREAM_RETAINED_NEWEST_FIRST = 0" in HEADER
assert "LOG_STREAM_RETAINED_TEXT_MAX 192" in HEADER

# The feed is bounded, PSRAM-preferred, and still has useful degradation paths
# for a fragmented PSRAM heap or a board with no external RAM.
assert re.search(r"RETAINED_CAPACITY_PSRAM\s+2048u", SOURCE)
assert re.search(r"RETAINED_CAPACITY_PSRAM_FALLBACK\s+512u", SOURCE)
assert re.search(r"RETAINED_CAPACITY_INTERNAL\s+32u", SOURCE)
full_at = initialise.find("RETAINED_CAPACITY_PSRAM * sizeof")
smaller_at = initialise.find("RETAINED_CAPACITY_PSRAM_FALLBACK * sizeof")
internal_at = initialise.find("RETAINED_CAPACITY_INTERNAL * sizeof")
assert -1 not in (full_at, smaller_at, internal_at)
assert full_at < smaller_at < internal_at
assert initialise.count("MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT") == 2
assert "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT" in initialise
assert "ESP_ERR_NO_MEM" in initialise

# Capture is best-effort and never waits behind a UI snapshot. It preserves
# logical lines, marks a deterministic 191-byte prefix, and advances monotonic
# counters only for accepted lines.
assert "portTRY_ENTER_CRITICAL(&s_retained_lock, 0)" in append
assert "__atomic_fetch_add(&s_retained_dropped_count" in append
assert "copy_length = LOG_STREAM_RETAINED_TEXT_MAX - 1" in append
assert "slot->text[copy_length] = '\\0'" in append
assert "slot->sequence = ++s_retained_total_count" in append
assert "slot->captured_ms = (uint32_t)(esp_timer_get_time() / 1000)" in append
assert "s_retained_generation++" in append
assert "text[end] != '\\n'" in capture
assert "text[line_end - 1] == '\\r'" in capture
assert "retained_append_line" in capture
assert "rendered_len >= (int)sizeof(buf)" in hook
capture_at = hook.find("retained_capture_text")
websocket_at = hook.find("xRingbufferSend")
assert -1 not in (capture_at, websocket_at) and capture_at < websocket_at

# Snapshots copy one bounded slot under the feed lock and into caller storage.
# This avoids torn PSRAM reads without PSRAM atomics (which ESP-IDF emulates
# with a hidden global spinlock). They never drain the WebSocket ring.
assert "portENTER_CRITICAL(&s_retained_lock)" in copy_slot
assert "portEXIT_CRITICAL(&s_retained_lock)" in copy_slot
assert "__atomic_" not in copy_slot
assert "slot->version" not in SOURCE
assert "RETAINED_SNAPSHOT_RETRIES" not in SOURCE
assert "filter->level_mask" in matching
assert "line->sequence <= filter->after_sequence" in matching
assert "line->sequence >= filter->before_sequence" in matching
assert "retained_contains_case_insensitive" in matching
assert "LOG_STREAM_RETAINED_OLDEST_FIRST" in snapshot_core
for body in (snapshot, snapshot_page, snapshot_core, copy_slot, matching):
    for forbidden in ("xRingbufferReceive", "vRingbufferReturnItem"):
        assert forbidden not in body, f"native snapshot consumes {forbidden}"
for forbidden in ("malloc(", "calloc(", "heap_caps_", "free("):
    assert forbidden not in snapshot, f"snapshot allocates via {forbidden}"
    assert forbidden not in snapshot_page, f"paged snapshot allocates via {forbidden}"

# Explicit paging scans the bounded retained window for an exact match count,
# while copying no more than the caller's reusable visible-row buffer. A
# before-sequence anchor prevents new capture from shifting a paused page.
assert "offset < bounds.count" in snapshot_page
assert "page->returned < line_capacity" in snapshot_page
assert "page->matching_count = matched" in snapshot_page
assert "page->has_previous_page" in snapshot_page
assert "page->has_next_page" in snapshot_page
assert "retained_filter_matches" in snapshot_page

# Clear is scoped strictly to the visible RAM view. Lifetime counters, the
# browser ring, persistent write queue, and already-written files all survive.
assert "s_retained_head = 0" in clear
assert "s_retained_count = 0" in clear
assert "s_retained_generation++" in clear
for preserved in (
    "s_retained_total_count =",
    "s_retained_dropped_count =",
    "s_ringbuf",
    "s_writebuf",
    "remove(",
    "unlink(",
):
    assert preserved not in clear, f"Clear mutates preserved state: {preserved}"

# Save is a separate, chronological file in the existing log directory. The
# export lease spans every FAT operation, partial files stay temporary, and
# all post-acquire exits converge on release/error publication.
assert 'RETAINED_SAVE_FILE         "touchscreen-visible.log"' in SOURCE
assert "LOG_FILE_PREFIX" not in save
lease_at = save.find("sd_storage_lease_acquire(SD_LEASE_EXPORT")
mkdir_at = save.find("mkdir(LOG_DIR")
open_at = save.find('fopen(tmp_path, "wb")')
publish_at = save.find("retained_publish_snapshot(tmp_path, final_path, backup_path)")
release_at = save.rfind("sd_storage_lease_release_unchanged(SD_LEASE_EXPORT)")
assert -1 not in (lease_at, mkdir_at, open_at, publish_at, release_at)
assert lease_at < mkdir_at < open_at < publish_at < release_at
assert "chronological.order = LOG_STREAM_RETAINED_OLDEST_FIRST" in save
assert "retained_filter_matches" in save
assert "progress_fn(0, bounds.count, progress_ctx)" in save
assert "progress_fn(offset + 1, bounds.count, progress_ctx)" in save
assert "progress_fn(bounds.count, bounds.count, progress_ctx)" in save
assert "tmp_exists" in save and "remove(tmp_path)" in save
assert "bool close_failed = fflush(file) != 0" in save
assert "if (fclose(file) != 0) close_failed = true" in save
assert "retained_set_last_error(error)" in save
assert "remove(final_path)" not in save
assert "rename(final_path, backup_path)" in publish
assert "rename(temporary_path, final_path)" in publish
assert "rename(backup_path, final_path)" in publish
for preserved in ("s_retained_head =", "s_retained_count =", "s_ringbuf"):
    assert preserved not in save, f"Save mutates retained/browser state: {preserved}"

assert "log_stream_retained_contract_test.py" in HOST_TEST

# The disconnected screen has an explicit, non-destructive recovery path.
# Retry is a no-op for an already-live feed and publishes a fully zeroed ring
# only after allocation has completed.
assert "if (s_retained_slots && s_retained_capacity > 0)" in retry
assert "s_retained_retrying" in retry
assert retry.find("heap_caps_calloc") < retry.find("s_retained_slots = slots")
assert "s_retained_head = 0" in retry and "s_retained_count = 0" in retry
assert "free(slots)" in retry
assert "MALLOC_CAP_INTERNAL" not in retry, (
    "late Logs reconnect must not consume the display's internal-RAM reserve"
)
assert "s_retained_slots[newest].captured_ms" in initialise or (
    "s_retained_slots[newest].captured_ms" in SOURCE
)

print("retained log stream contract passed")
