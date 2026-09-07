/* Lightweight SD-backed history model for the native touch UI. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Compatibility page size used by the current native UI.  This is not a
 * product/history retention limit: touch_history_load_page() can address the
 * complete card index with an arbitrary caller-owned page buffer. */
#define TOUCH_HISTORY_MAX_DAYS 30
#define TOUCH_HISTORY_TRACE_POINTS 48
#define TOUCH_HISTORY_TRACE_MISSING INT16_MIN

/* Rev B native History service model.  The large overview is deliberately
 * separate from the legacy trace type so the existing LVGL service snapshot
 * does not permanently consume another ~8 KiB of scarce internal RAM.  A
 * worker should allocate touch_history_overview_t in PSRAM and retain only the
 * selected night/channel. */
#define TOUCH_HISTORY_OVERVIEW_POINTS 480
#define TOUCH_HISTORY_VALUE_MISSING INT16_MIN
#define TOUCH_HISTORY_VALUE_SCALE 100
#define TOUCH_HISTORY_SESSION_ID_LEN 32
#define TOUCH_HISTORY_STATS_MAX_VALUES 4
/* Component-local result used only when a caller-provided operation callback
 * requests cancellation.  Partial output must be discarded. */
#define TOUCH_HISTORY_ERR_CANCELLED ((esp_err_t)0x7101)

typedef bool (*touch_history_should_cancel_fn)(void *context);
typedef void (*touch_history_progress_fn)(void *context,
                                          uint16_t progress_per_mille);

/* Optional worker-operation hooks.  Callbacks run synchronously on the
 * calling worker and must be non-blocking; they must not call back into the
 * History service.  Progress is monotonic from 0 through 1000. */
typedef struct {
    touch_history_should_cancel_fn should_cancel;
    touch_history_progress_fn progress;
    void *context;
} touch_history_operation_t;

typedef enum {
    TOUCH_HISTORY_CHANNEL_FLOW = 0,
    TOUCH_HISTORY_CHANNEL_SPO2,
    TOUCH_HISTORY_CHANNEL_LEAK,
    TOUCH_HISTORY_CHANNEL_COUNT,
} touch_history_channel_t;

/* Full-size History signal set. Physical values are represented as signed
 * hundredths of the unit named below. Rich Flow stays source-native L/s;
 * Leak is converted to its clinical L/min display unit. */
typedef enum {
    TOUCH_HISTORY_SIGNAL_FLOW = 0,       /* L/s */
    TOUCH_HISTORY_SIGNAL_PRESSURE,       /* cmH2O; optional EPR companion */
    TOUCH_HISTORY_SIGNAL_LEAK,           /* L/min */
    TOUCH_HISTORY_SIGNAL_FLOW_LIMIT,     /* dimensionless */
    TOUCH_HISTORY_SIGNAL_SNORE,          /* dimensionless */
    TOUCH_HISTORY_SIGNAL_SPO2,           /* percent */
    TOUCH_HISTORY_SIGNAL_PULSE,          /* bpm */
    TOUCH_HISTORY_SIGNAL_MOTION,         /* device motion index/flags */
    TOUCH_HISTORY_SIGNAL_COUNT,
} touch_history_signal_t;

#define TOUCH_HISTORY_SIGNAL_BIT(signal) ((uint16_t)(1U << (unsigned)(signal)))

typedef enum {
    TOUCH_HISTORY_AGGREGATION_MEAN = 0,
    TOUCH_HISTORY_AGGREGATION_MINIMUM,
    TOUCH_HISTORY_AGGREGATION_MAXIMUM,
    TOUCH_HISTORY_AGGREGATION_ENVELOPE,
} touch_history_aggregation_t;

enum {
    TOUCH_HISTORY_POINT_VALID = 1U << 0,
    TOUCH_HISTORY_POINT_UPPER_VALID = 1U << 1,
    TOUCH_HISTORY_POINT_COMPANION_VALID = 1U << 2,
    /* The bin overlaps at least one terminal AirSense therapy session. */
    TOUCH_HISTORY_POINT_THERAPY = 1U << 3,
};

typedef struct {
    /* Uniform wall-clock bin centres.  Missing bins retain a timestamp so
     * cursors/window selection do not have to infer x coordinates. */
    int64_t timestamp_ms[TOUCH_HISTORY_OVERVIEW_POINTS];
    int16_t value_x100[TOUCH_HISTORY_OVERVIEW_POINTS];
    /* Flow only: parallel maximum for the min/max envelope. */
    int16_t upper_x100[TOUCH_HISTORY_OVERVIEW_POINTS];
    /* Pressure only: EPR-relieved pressure when the source provides it. */
    int16_t companion_x100[TOUCH_HISTORY_OVERVIEW_POINTS];
    uint16_t sample_count[TOUCH_HISTORY_OVERVIEW_POINTS];
    uint8_t flags[TOUCH_HISTORY_OVERVIEW_POINTS];
    int64_t axis_start_ms;
    int64_t axis_end_ms;
    uint32_t bin_width_ms;
    uint32_t source_sample_count;
    uint32_t valid_sample_count;
    /* Number of populated time slots in the fixed-capacity arrays. All-night
     * views use 480; short ranged rereads may use fewer to preserve native
     * sample cadence without fabricating interpolated points. */
    uint16_t point_count;
    uint16_t contributing_sessions;
    /* Valid samples during therapy / combined therapy time, 0..1000.  This is
     * meaningful for O2 Ring channels; zero is not itself proof of absence. */
    uint16_t therapy_coverage_per_mille;
    touch_history_signal_t signal;
    touch_history_aggregation_t aggregation;
    bool has_data;
    bool has_companion;
    bool has_therapy_coverage;
    /* One or more session sources were present but malformed/unreadable. The
     * remaining bins are safe to display, but the view is incomplete. */
    uint16_t unreadable_sessions;
    /* Ranged Flow only: true means the 25 Hz waveform supplied every
     * contributing session. `source_fallback` means raw was requested but a
     * session had only the 1 Hz min/max sidecar, so the whole result honestly
     * remains an envelope. */
    bool source_raw;
    bool source_fallback;
    bool preview; /* Reprojected display bins; never exact source statistics. */
    bool loaded;
} touch_history_overview_t;

typedef struct {
    char id[TOUCH_HISTORY_SESSION_ID_LEN];
    int64_t start_ms;
    int64_t end_ms;
    uint16_t available_signals;
    bool partial;
    bool end_estimated;
    bool has_epr_companion;
} touch_history_session_t;

typedef enum {
    TOUCH_HISTORY_EVENT_OBSTRUCTIVE_APNEA = 0,
    TOUCH_HISTORY_EVENT_CENTRAL_APNEA,
    TOUCH_HISTORY_EVENT_HYPOPNEA,
    /* Generic/unspecified apnea is intentionally not folded into OA or CA. */
    TOUCH_HISTORY_EVENT_GENERIC_APNEA,
    TOUCH_HISTORY_EVENT_RERA,
    TOUCH_HISTORY_EVENT_TYPE_COUNT,
    TOUCH_HISTORY_EVENT_UNKNOWN = -1,
} touch_history_event_type_t;

typedef struct {
    touch_history_event_type_t type;
    int64_t start_ms;
    int64_t end_ms;
    uint16_t session_index;
    bool time_corrected;
} touch_history_event_t;

typedef struct {
    uint32_t count[TOUCH_HISTORY_EVENT_TYPE_COUNT];
    uint32_t total_count;
    uint64_t eligible_therapy_ms;
    float ahi;
    float oai;
    float cai;
    float hi;
    float generic_ai;
    float rera;
    bool complete;
    bool has_indices;
} touch_history_event_totals_t;

typedef struct {
    size_t offset;
    size_t returned;
    size_t total_count;
    bool has_more;
    touch_history_event_totals_t totals;
} touch_history_event_page_t;

typedef struct {
    char day[9];
    int64_t axis_start_ms;
    int64_t axis_end_ms;
    size_t session_count;
    size_t sessions_returned;
    uint16_t available_signals;
    uint16_t o2_coverage_per_mille;
    float device_ahi;
    float st_ahi;
    bool has_device_ahi;
    bool has_st_ahi;
    bool has_o2_coverage;
    bool sessions_truncated;
    uint16_t skipped_sessions;
    uint16_t probe_failed_sessions;
    bool has_session_errors;
    bool has_oximetry_error;
    /* Raw session traces remain usable when optional Device Summary metadata
     * is missing/corrupt.  The controller surfaces this as degraded rather
     * than replacing the entire night with a card-read failure. */
    bool has_summary_error;
    bool has_event_loss;
    esp_err_t events_result;
    touch_history_event_totals_t event_totals;
} touch_history_night_t;

typedef struct {
    size_t offset;
    size_t returned;
    size_t total_days;
    bool has_more;
} touch_history_index_page_t;

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t days_in_month;
    /* Bit 0 is day 1; bit 30 is day 31. */
    uint32_t therapy_days;
    uint32_t oximetry_days;
    uint16_t therapy_night_count;
    uint16_t oximetry_night_count;
} touch_history_month_t;

/* Exact, source-derived statistics for a selected wall-clock window.  These
 * values are deliberately separate from touch_history_overview_t: display
 * bins are lossy and must never be used to calculate clinical percentiles.
 * Values retain the service-wide x100 convention.  TIME_BELOW_88 is x100
 * minutes, rather than the selected signal's physical unit. */
typedef enum {
    TOUCH_HISTORY_STAT_P50 = 0,
    TOUCH_HISTORY_STAT_P95,
    TOUCH_HISTORY_STAT_P995,
    TOUCH_HISTORY_STAT_ABSOLUTE_P50,
    TOUCH_HISTORY_STAT_ABSOLUTE_P95,
    TOUCH_HISTORY_STAT_ABSOLUTE_P995,
    TOUCH_HISTORY_STAT_MINIMUM,
    TOUCH_HISTORY_STAT_P5,
    TOUCH_HISTORY_STAT_P05,
    TOUCH_HISTORY_STAT_TIME_BELOW_88,
    TOUCH_HISTORY_STAT_MEDIAN,
    TOUCH_HISTORY_STAT_MAXIMUM,
} touch_history_stat_kind_t;

typedef struct {
    touch_history_stat_kind_t kind;
    int32_t value_x100;
    bool available;
} touch_history_stat_value_t;

typedef struct {
    touch_history_stat_value_t values[TOUCH_HISTORY_STATS_MAX_VALUES];
    uint64_t sample_count;
    int64_t start_ms;
    int64_t end_ms;
    touch_history_signal_t signal;
    uint8_t value_count;
    bool therapy_only;
    bool source_raw;
    bool exact;
    bool loaded;
} touch_history_stats_t;

/* Only the currently selected night/channel trace is retained by the UI.
 * Keeping this separate from touch_history_day_t avoids multiplying the
 * trace storage by 30 nights and three channels in scarce internal RAM. */
typedef struct {
    int16_t points[TOUCH_HISTORY_TRACE_POINTS];
    /* Flow is rendered as two parallel time series.  upper_points contains
     * the per-bin maximum while points contains the per-bin minimum; other
     * channels leave upper_points missing. */
    int16_t upper_points[TOUCH_HISTORY_TRACE_POINTS];
    int64_t start_ms;
    int64_t end_ms;
    uint8_t count;
    touch_history_channel_t channel;
    bool has_data;
    bool loaded;
} touch_history_trace_t;

typedef struct {
    char day[9];
    int sessions;
    int mask_off_count;
    int usage_min;
    float ahi;
    float oai;
    float cai;
    float hi;
    float rera;
    float pressure_p95;
    float leak_p95;
    /* `ahi` remains the compatibility alias consumed by the current UI.
     * Device AHI and SomnoTrace event-derived AHI are separate provenance. */
    float device_ahi;
    float st_ahi;
    bool has_summary;
    bool has_mask_off_count;
    bool has_usage;
    bool has_ahi;
    bool has_oai;
    bool has_cai;
    bool has_hi;
    bool has_rera;
    bool has_pressure_p95;
    bool has_leak_p95;
    bool has_device_ahi;
    bool has_st_ahi;
    bool has_therapy;
    bool has_oximetry;
    uint16_t skipped_sessions;
    bool has_incomplete_sessions;
} touch_history_day_t;

/* Compatibility first-page loader.  Returns newest days first and fills up to
 * caller capacity; there is no built-in 30-night storage/index cap. */
esp_err_t touch_history_load(touch_history_day_t *days, size_t capacity,
                             size_t *count);

/* Address the complete newest-first day index without retaining it in RAM.
 * `capacity` is the requested page size and `offset` is the zero-based row in
 * that ordering. */
esp_err_t touch_history_load_page(size_t offset, touch_history_day_t *days,
                                  size_t capacity,
                                  touch_history_index_page_t *page);
esp_err_t touch_history_load_page_ex(
    size_t offset, touch_history_day_t *days, size_t capacity,
    touch_history_index_page_t *page,
    const touch_history_operation_t *operation);

/* Resolve one recorded day to its newest-first global row without imposing a
 * retention cap. Used by Calendar so Previous/Next remains meaningful even
 * when the selected date is outside the currently recycled seven-row page. */
esp_err_t touch_history_find_day_index(const char *day, size_t *index_out,
                                       size_t *total_days_out);
esp_err_t touch_history_find_day_index_ex(
    const char *day, size_t *index_out, size_t *total_days_out,
    const touch_history_operation_t *operation);

/* Compact calendar metadata.  This scans ready terminal sessions and ready
 * canonical O2 Ring packages, but never loads trace samples into the result. */
esp_err_t touch_history_load_month(uint16_t year, uint8_t month,
                                   touch_history_month_t *month_out);
esp_err_t touch_history_load_month_ex(
    uint16_t year, uint8_t month, touch_history_month_t *month_out,
    const touch_history_operation_t *operation);

/* Night detail and session-boundary discovery.  `sessions` may be NULL only
 * when session_capacity is zero; total/returned/truncated are always reported. */
esp_err_t touch_history_load_night(const char *day,
                                   touch_history_night_t *night,
                                   touch_history_session_t *sessions,
                                   size_t session_capacity);
esp_err_t touch_history_load_night_ex(
    const char *day, touch_history_night_t *night,
    touch_history_session_t *sessions, size_t session_capacity,
    const touch_history_operation_t *operation);

/* Load one 480-bin, all-terminal-session overview on a shared wall-clock
 * axis.  Gaps have no VALID flag and retain TOUCH_HISTORY_VALUE_MISSING.
 * Overview/window buffers belong to the caller and should normally live in
 * PSRAM. */
esp_err_t touch_history_load_overview(const char *day,
                                      touch_history_signal_t signal,
                                      touch_history_overview_t *overview);
esp_err_t touch_history_load_overview_ex(
    const char *day, touch_history_signal_t signal,
    touch_history_overview_t *overview,
    const touch_history_operation_t *operation);

/* Re-read a half-open wall-clock window [start_ms, end_ms) from SD. The range
 * must lie inside the night axis returned by touch_history_load_night(). Flow
 * prefers bucketed 25 Hz L0 data through the 22-minute/quarter-night zoom
 * threshold and reports an honest 1 Hz min/max fallback when L0 is absent;
 * other channels retain their native cadence. */
esp_err_t touch_history_load_range(
    const char *day, touch_history_signal_t signal,
    int64_t start_ms, int64_t end_ms, touch_history_overview_t *range);
esp_err_t touch_history_load_range_ex(
    const char *day, touch_history_signal_t signal,
    int64_t start_ms, int64_t end_ms, touch_history_overview_t *range,
    const touch_history_operation_t *operation);

/* Controller-oriented graph loader. A zero start/end pair selects the
 * all-night overview; otherwise it has the same ranged semantics as above.
 * `therapy_only` is meaningful only for SpO2 and filters source samples using
 * the exact eligible therapy intervals rather than display-bin inference. */
esp_err_t touch_history_load_view_ex(
    const char *day, touch_history_signal_t signal,
    int64_t start_ms, int64_t end_ms, bool therapy_only,
    touch_history_overview_t *view,
    const touch_history_operation_t *operation);

/* Stream exact source samples through a bounded PSRAM histogram. The selected
 * half-open wall-clock range must lie inside the night axis. AirSense signals
 * are restricted to the same eligible therapy intervals used for ST AHI;
 * SpO2 can optionally apply that therapy-only filter. Flow requires raw 25 Hz
 * L0 for every contributing interval and never labels a sidecar envelope as
 * exact. Motion currently returns a loaded, explicitly unavailable result
 * because its bit-field has no agreed scalar statistic. */
esp_err_t touch_history_load_stats(
    const char *day, touch_history_signal_t signal,
    int64_t start_ms, int64_t end_ms, bool therapy_only,
    touch_history_stats_t *stats);
esp_err_t touch_history_load_stats_ex(
    const char *day, touch_history_signal_t signal,
    int64_t start_ms, int64_t end_ms, bool therapy_only,
    touch_history_stats_t *stats,
    const touch_history_operation_t *operation);

/* Pageable event markers plus whole-night counts/indices.  A complete empty
 * events file is a valid zero-event night; missing/malformed eligible-session
 * files never become a false zero. */
esp_err_t touch_history_load_events(const char *day, size_t offset,
                                    touch_history_event_t *events,
                                    size_t capacity,
                                    touch_history_event_page_t *page);
esp_err_t touch_history_load_events_ex(
    const char *day, size_t offset, touch_history_event_t *events,
    size_t capacity, touch_history_event_page_t *page,
    const touch_history_operation_t *operation);

/* Small pure helpers are public so host tests and future zoom/range readers
 * share the exact event taxonomy and combined-duration arithmetic. */
touch_history_event_type_t touch_history_event_type_from_name(const char *name);
/* BLE spool replay can repeat an event. Compact equal whole-second report
 * time + event type identities in place and return the retained count. */
size_t touch_history_deduplicate_events(touch_history_event_t *events,
                                        size_t count);
bool touch_history_compute_event_indices(
    const uint32_t counts[TOUCH_HISTORY_EVENT_TYPE_COUNT],
    uint64_t eligible_therapy_ms,
    touch_history_event_totals_t *totals);
int touch_history_overview_bin(int64_t axis_start_ms, int64_t axis_end_ms,
                               int64_t timestamp_ms);
uint16_t touch_history_range_point_count(touch_history_signal_t signal,
                                         uint64_t duration_ms);
bool touch_history_flow_range_prefers_raw(uint64_t duration_ms,
                                          uint64_t night_duration_ms);
/* Raw Flow may be drawn as a conventional line only while the retained view
 * has at least one display bin per source sample. Wider windows must preserve
 * each bin's signed minimum and maximum; averaging breathing around zero can
 * otherwise turn real inspiration/expiration into a misleading flat line. */
bool touch_history_flow_bins_need_envelope(uint64_t duration_ms,
                                           uint16_t point_count,
                                           uint32_t sample_hz_x10);
/* Overflow-safe source-duration bound. `period_num_us / period_den` is the
 * exact sample period; unlike a record-count cap this accepts useful 25 Hz
 * Flow tracks without permitting an implausibly long corrupt trace. */
bool touch_history_sample_span_within(uint32_t sample_count,
                                      uint32_t period_num_us,
                                      uint32_t period_den,
                                      uint64_t maximum_ms);
/* Pure rich-History source-unit conversion used by overview, ranged reads,
 * exact stats, and host regressions. Legacy touch_history_load_trace keeps its
 * existing raw compatibility semantics. */
bool touch_history_scale_source_x100(touch_history_signal_t signal,
                                     int16_t raw, int16_t *scaled);
/* Applies a manifest drift only when it is within the same strict one-day
 * plausibility bound used at capture time and signed addition is safe. */
bool touch_history_apply_clock_drift(int64_t as11_ms, int64_t drift_ms,
                                     int64_t *corrected_ms);
/* Browser-compatible weighted percentile over an integer histogram. `p` is
 * per-mille (5 = 0.5%, 995 = 99.5%). */
bool touch_history_weighted_percentile_histogram(
    const uint32_t *counts, size_t bin_count, int32_t first_value,
    uint64_t sample_count, uint16_t percentile_per_mille,
    int32_t *value_out);
/* Allocation-free decoder for one unwrapped AS11 Summary spool record.
 * Existing day/session identity fields are preserved; summary fields are
 * published only after the complete protobuf record validates. */
bool touch_history_decode_summary_record(const uint8_t *record, size_t length,
                                         touch_history_day_t *day);

/* Loads one bounded overview for the given noon-day. Flow uses the longest
 * terminal session's 1 Hz min/max sidecar, Leak its 0.5 Hz PLD track, and
 * SpO2 the ready canonical O2 Ring vitals track with the greatest valid-sample
 * coverage. The function owns an upload/read lease and is intended for a
 * worker task; gaps remain explicit. */
esp_err_t touch_history_load_trace(const char *day,
                                   touch_history_channel_t channel,
                                   touch_history_trace_t *trace);

/* Optional, cancellable backfill for terminal sessions, called only after the
 * foreground graph and exact statistics have been published. */
esp_err_t touch_history_prepare_day_ex(const char *day,
                                      const touch_history_operation_t *operation);

/* Pure bounded reprojection for immediate navigation feedback. Missing source
 * bins remain missing; no interpolation across gaps. Source and out differ. */
void touch_history_make_preview(const touch_history_overview_t *source,
    touch_history_signal_t signal, int64_t start_ms, int64_t end_ms,
    touch_history_overview_t *out);

/* One collection/filter pass, including oversized nights that bypass retention.
 * total_count is the whole night; has_more reports truncated visible markers. */
esp_err_t touch_history_load_window_events_ex(const char *day,
    int64_t start_ms, int64_t end_ms, touch_history_event_t *events,
    size_t capacity, touch_history_event_page_t *page,
    const touch_history_operation_t *operation);
