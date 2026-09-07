#!/usr/bin/env python3
"""Structural contracts for the Rev B native History service foundation."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "main/touch_history.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "main/touch_history.c").read_text(encoding="utf-8")


def require(source: str, pattern: str, label: str) -> None:
    if not re.search(pattern, source, re.DOTALL):
        raise AssertionError(f"missing {label}")


signals = (
    "FLOW", "PRESSURE", "LEAK", "FLOW_LIMIT", "SNORE", "SPO2", "PULSE",
    "MOTION",
)
signal_enum = HEADER[
    HEADER.index("TOUCH_HISTORY_SIGNAL_FLOW"):
    HEADER.index("TOUCH_HISTORY_SIGNAL_COUNT")
]
for signal in signals:
    assert f"TOUCH_HISTORY_SIGNAL_{signal}" in signal_enum
assert signal_enum.count("TOUCH_HISTORY_SIGNAL_") == 8
require(HEADER, r"TOUCH_HISTORY_OVERVIEW_POINTS\s+480\b", "480-bin overview")
for array in ("timestamp_ms", "value_x100", "upper_x100", "companion_x100",
              "sample_count", "flags"):
    require(HEADER, rf"{array}\[TOUCH_HISTORY_OVERVIEW_POINTS\]",
            f"per-point {array}")
require(HEADER, r"TOUCH_HISTORY_POINT_VALID.*?TOUCH_HISTORY_POINT_THERAPY",
        "explicit point validity and therapy overlap")
require(HEADER, r"TOUCH_HISTORY_SIGNAL_PRESSURE.*?optional EPR companion",
        "Pressure/EPR model")

# The current three-button/48-point LVGL widget remains a compatibility API;
# its count must not be reused as the eight-signal service count.
require(HEADER, r"TOUCH_HISTORY_CHANNEL_COUNT", "legacy channel count")
require(HEADER, r"TOUCH_HISTORY_SIGNAL_COUNT", "full service channel count")
require(HEADER, r"TOUCH_HISTORY_TRACE_POINTS\s+48\b", "legacy compact trace")

require(SOURCE, r"Leak source is hundredths L/s.*?value \*= 60.*?"
                r"Flow remains source-native hundredths L/s",
        "distinct rich Flow L/s and Leak L/min scaling")
require(SOURCE, r"TOUCH_HISTORY_AGGREGATION_ENVELOPE.*?"
                r"minimum\[i\].*?maximum\[i\]",
        "parallel Flow min/max envelope")
require(SOURCE, r"history_pld_channel.*?PRESSURE:\s*return 1.*?"
                r"LEAK:\s*return 3.*?FLOW_LIMIT:\s*return 10.*?"
                r"SNORE:\s*return 9", "canonical PLD channel mapping")
require(SOURCE, r"VITALS_SPO2.*?VITALS_PULSE.*?VITALS_MOTION_FLAGS",
        "canonical O2 Ring signal mapping")
require(SOURCE, r"history_load_sessions_uncached.*?qsort\(sessions.*?"
                r"history_axis.*?history_accumulate_as11",
        "chronological multi-session shared-axis aggregation")
require(SOURCE, r"history_bin_overlaps_session.*?TOUCH_HISTORY_POINT_THERAPY",
        "session-gap/overlap metadata")
assert "Pick the longest terminal session" in SOURCE, (
    "legacy trace behavior should stay visibly documented until UI migration"
)

for api in ("touch_history_load_page", "touch_history_find_day_index",
            "touch_history_load_month",
            "touch_history_load_night", "touch_history_load_overview",
            "touch_history_load_range", "touch_history_load_events"):
    require(HEADER, rf"esp_err_t\s+{api}\(", f"{api} declaration")
    require(SOURCE, rf"esp_err_t\s+{api}\(", f"{api} implementation")
assert "capacity < TOUCH_HISTORY_MAX_DAYS" not in SOURCE
require(SOURCE, r"page->total_days\s*=\s*total.*?page->has_more",
        "full-index pagination metadata")
require(SOURCE, r"therapy_days", "therapy calendar metadata")
require(SOURCE, r"oximetry_days", "oximetry calendar metadata")

require(HEADER, r"TOUCH_HISTORY_EVENT_GENERIC_APNEA",
        "distinct generic-apnea type")
require(SOURCE, r'"ApneaEnd".*?TOUCH_HISTORY_EVENT_GENERIC_APNEA',
        "generic-apnea source mapping")
require(SOURCE, r"eligible_therapy_ms.*?3600000\.0.*?"
                r"totals->ahi\s*=\s*totals->oai\s*\+\s*totals->cai\s*\+\s*"
                r"totals->hi\s*\+\s*totals->generic_ai",
        "combined-duration ST AHI including generic apnea")
require(HEADER, r"float\s+device_ahi;.*?float\s+st_ahi;.*?"
                r"bool\s+has_device_ahi;.*?bool\s+has_st_ahi;",
        "separate Device/ST AHI provenance")
require(SOURCE, r"history_fill_day_summary.*?has_device_ahi.*?"
                r"history_collect_events_leased.*?night->st_ahi",
        "night detail AHI population")
require(SOURCE, r"device summary unavailable.*?has_summary_error\s*=\s*true.*?"
                r"history_collect_events_leased",
        "nonfatal optional Summary metadata failure")

# Every public SD reader takes one upload/read lease. Compatibility APIs
# delegate through their _ex implementation so controller cancellation applies
# while waiting for the lease and during long directory scans.
for api in ("touch_history_load_page", "touch_history_find_day_index",
            "touch_history_load_month"):
    wrapper = SOURCE[
        SOURCE.index(f"esp_err_t {api}("):
        SOURCE.index(f"esp_err_t {api}_ex(")
    ]
    assert f"{api}_ex(" in wrapper, (
        f"{api} compatibility API must delegate to its cancellable reader"
    )
for api in ("touch_history_load_page_ex", "touch_history_find_day_index_ex",
            "touch_history_load_month_ex", "touch_history_load_night_ex",
            "touch_history_load_overview_ex", "touch_history_load_range_ex",
            "touch_history_load_events_ex"):
    start = SOURCE.index(f"esp_err_t {api}(")
    body = SOURCE[start:SOURCE.find("\n}", start) + 2]
    assert "history_lease_acquire_operation(operation)" in body, (
        f"{api} must acquire a cancellable SD lease"
    )
    assert "sd_storage_lease_release(SD_LEASE_UPLOAD)" in body, (
        f"{api} must release SD lease"
    )

require(HEADER, r"touch_history_operation_t.*?touch_history_load_page_ex.*?"
                r"touch_history_find_day_index_ex.*?"
                r"touch_history_load_month_ex.*?"
                r"touch_history_load_night_ex.*?"
                r"touch_history_load_overview_ex.*?touch_history_load_range_ex.*?"
                r"touch_history_load_events_ex",
        "cancellable detail read APIs")
require(SOURCE, r"history_collect_days_leased.*?history_operation_cancelled.*?"
                r"month_prefix.*?history_load_sessions_leased\(.*?operation.*?"
                r"history_operation_cancelled.*?"
                r"month_prefix.*?history_ox_metadata\(.*?operation\)",
        "cancellable calendar/index directory scan")
require(SOURCE, r"touch_history_load_month_ex.*?snprintf\(prefix.*?"
                r"history_collect_days_leased\(.*?operation, prefix\)",
        "month reads filter days before opening session or O2 files")
require(SOURCE, r"touch_history_find_day_index_ex.*?"
                r"history_operation_cancelled.*?TOUCH_HISTORY_ERR_CANCELLED.*?"
                r"result == ESP_OK && \*index_out == SIZE_MAX",
        "day-index cancellation is not collapsed into not-found")
require(HEADER, r"half-open wall-clock window \[start_ms, end_ms\).*?"
                r"touch_history_load_range",
        "documented native range reread contract")
require(SOURCE, r"history_flow_raw_candidate.*?\"flow\"\s*:\s*\"brp\".*?"
                r"inspect_trace_candidate\(path, 0, channels, 250",
        "25 Hz L0 Flow zoom source")
require(SOURCE, r"minimum_zoom_ms\s*=\s*22ULL\s*\*\s*60ULL\s*\*\s*1000ULL.*?"
                r"duration_ms\s*<=\s*minimum_zoom_ms\s*\|\|.*?"
                r"duration_ms\s*<=\s*night_duration_ms\s*/\s*4U",
        "22-minute and quarter-night raw Flow policy")
require(SOURCE, r"history_select_flow_range_source.*?"
                r"history_flow_raw_candidate.*?history_session_candidate.*?"
                r"used_fallback",
        "truthful raw-absent sidecar fallback")
require(HEADER, r"bool\s+source_raw;.*?bool\s+source_fallback;",
        "range source fidelity metadata")
require(SOURCE, r"history_as11_record_at_or_after.*?first_record.*?"
                r"fseek\(file, \(long\)byte_offset",
        "bounded SD range seek")
require(SOURCE, r"history_collect_ox_candidates.*?qsort\(candidates.*?"
                r"for \(size_t c = 0; c < candidate_count; \+\+c\)",
        "all ready O2 recordings combined deterministically")
require(SOURCE, r"history_unified_axis.*?history_ox_metadata",
        "shared therapy and O2 wall-clock axis")
require(SOURCE, r"history_collect_eligible_intervals_leased.*?"
                r"history_coverage_milliseconds",
        "O2 coverage over eligible therapy time")
require(SOURCE, r"touch_history_decode_summary_record.*?"
                r"HISTORY_SUMMARY_MAX_BYTES.*?history_alloc",
        "bounded PSRAM-backed direct Summary decoder")
assert "edf_gen_summary_json" not in SOURCE, (
    "native History must not allocate the web JSON summary pipeline"
)

require(SOURCE, r"heap_caps_calloc.*?MALLOC_CAP_SPIRAM.*?"
                r"heap_caps_malloc.*?MALLOC_CAP_SPIRAM",
        "PSRAM-first transient allocations")
require(SOURCE, r"HISTORY_READ_VALUES\s+512U", "bounded read slab")
assert "HISTORY_TRACE_MAX_RECORDS" not in SOURCE, (
    "trace validation must bound elapsed duration, not sample count"
)
require(SOURCE, r"touch_history_sample_span_within\(.*?10000000U.*?"
                r"header\.sample_hz_x10.*?HISTORY_AXIS_MAX_MS",
        "rate-aware raw Flow duration validation")

print("touch history service contract passed")
