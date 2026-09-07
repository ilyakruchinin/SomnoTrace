/* Rev B native History presentation surface for the 1024x600 touch UI. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"
#include "touch_history.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The BSP places this surface inside the persistent 992x450 content rail. */
#define TOUCH_HISTORY_UI_WIDTH 992
#define TOUCH_HISTORY_UI_HEIGHT 450
#define TOUCH_HISTORY_UI_LIST_WIDTH 288
#define TOUCH_HISTORY_UI_COLUMN_GAP 12
#define TOUCH_HISTORY_UI_DETAIL_X 300
#define TOUCH_HISTORY_UI_DETAIL_WIDTH 692

/* These are presentation-object bounds, not storage or retention limits. */
#define TOUCH_HISTORY_UI_LIST_ROWS 7
#define TOUCH_HISTORY_UI_CHANNEL_CONTROLS TOUCH_HISTORY_SIGNAL_COUNT
#define TOUCH_HISTORY_UI_STAT_COUNT 4
#define TOUCH_HISTORY_UI_MAX_SESSIONS 24
#define TOUCH_HISTORY_UI_MAX_VISIBLE_EVENTS 64
#define TOUCH_HISTORY_UI_TEXT_MAX 96

typedef struct touch_history_ui touch_history_ui_t;

typedef enum {
    TOUCH_HISTORY_UI_STATE_EMPTY = 0,
    /* Initial/new-night aggregation. This operation may be cancelled. */
    TOUCH_HISTORY_UI_STATE_AUTO_LOADING,
    TOUCH_HISTORY_UI_STATE_READY,
    /* Ranged refinement with a compact badge; newer navigation cancels it. */
    TOUCH_HISTORY_UI_STATE_ZOOM_LOADING,
    TOUCH_HISTORY_UI_STATE_READ_ERROR,
    /* Some provenance is unknown; known values remain visible. */
    TOUCH_HISTORY_UI_STATE_DEGRADED_UNKNOWN,
} touch_history_ui_state_t;

/* List/Calendar is a left-rail presentation choice, not a data lifecycle
 * state. Keeping it independent lets Calendar remain selected while the
 * right-hand night detail loads, fails, or reports degraded data. */
typedef enum {
    TOUCH_HISTORY_UI_RAIL_LIST = 0,
    TOUCH_HISTORY_UI_RAIL_CALENDAR,
} touch_history_ui_rail_mode_t;

/* Provenance for the respiratory-event lane. COMPLETE with a zero total is a
 * trustworthy zero-event night; UNAVAILABLE must never be presented as zero.
 * INCOMPLETE retains any source-backed markers while warning that others may
 * be missing. Display-cap truncation is reported separately by the snapshot. */
typedef enum {
    TOUCH_HISTORY_UI_EVENT_STATE_UNAVAILABLE = 0,
    TOUCH_HISTORY_UI_EVENT_STATE_COMPLETE,
    TOUCH_HISTORY_UI_EVENT_STATE_INCOMPLETE,
} touch_history_ui_event_state_t;

typedef enum {
    TOUCH_HISTORY_UI_INTENT_SELECT_DAY = 0,
    TOUCH_HISTORY_UI_INTENT_PAGE_RELATIVE,
    TOUCH_HISTORY_UI_INTENT_OPEN_CALENDAR,
    TOUCH_HISTORY_UI_INTENT_CLOSE_CALENDAR,
    TOUCH_HISTORY_UI_INTENT_MONTH_RELATIVE,
    TOUCH_HISTORY_UI_INTENT_SELECT_CALENDAR_DAY,
    TOUCH_HISTORY_UI_INTENT_SELECT_CHANNEL,
    TOUCH_HISTORY_UI_INTENT_PREVIOUS_NIGHT,
    TOUCH_HISTORY_UI_INTENT_NEXT_NIGHT,
    TOUCH_HISTORY_UI_INTENT_CANCEL_AUTO_LOAD,
    TOUCH_HISTORY_UI_INTENT_RETRY_READ,
    TOUCH_HISTORY_UI_INTENT_OPEN_CARD,
    TOUCH_HISTORY_UI_INTENT_FIT_NIGHT,
    TOUCH_HISTORY_UI_INTENT_ZOOM_RELATIVE,
    TOUCH_HISTORY_UI_INTENT_PAN_RELATIVE,
    TOUCH_HISTORY_UI_INTENT_SET_CURSOR,
    TOUCH_HISTORY_UI_INTENT_CLEAR_CURSOR,
    TOUCH_HISTORY_UI_INTENT_TOGGLE_THERAPY_ONLY,
} touch_history_ui_intent_type_t;

typedef struct {
    touch_history_ui_intent_type_t type;
    /* Page/month/zoom direction, or pan delta in milliseconds. */
    int64_t relative;
    /* Wall-clock cursor target for SET_CURSOR. */
    int64_t timestamp_ms;
    size_t row_index;
    touch_history_signal_t signal;
    char day[9];
} touch_history_ui_intent_t;

typedef void (*touch_history_ui_intent_fn)(
    void *context, const touch_history_ui_intent_t *intent);

typedef struct {
    touch_history_ui_intent_fn on_intent;
    void *intent_context;
} touch_history_ui_config_t;

/* Window-specific statistics are supplied by the History worker. The UI does
 * not invent percentiles from the 480 display bins. Values use x100 scaling;
 * unit may override the channel unit (for example, "min" for time <88%). */
typedef struct {
    const char *label;
    const char *unit;
    int32_t value_x100;
    bool available;
} touch_history_ui_stat_snapshot_t;

/* This is a transient input snapshot. touch_history_ui_apply() deep-copies
 * every bounded array/string it needs, so the caller may release the input as
 * soon as the function returns. Supplying more than the declared UI bounds is
 * rejected instead of silently dropping sessions, events, or rows. */
typedef struct {
    touch_history_ui_state_t state;
    touch_history_ui_rail_mode_t rail_mode;

    const touch_history_day_t *days;
    size_t day_count;
    touch_history_index_page_t page;
    size_t selected_row; /* SIZE_MAX means no selection. */

    const touch_history_night_t *night;
    const touch_history_session_t *sessions;
    size_t session_count;
    const touch_history_overview_t *overview;
    const touch_history_event_t *events;
    size_t event_count;
    /* Whole-night source total; event_count is only the selected-window set. */
    size_t event_total_count;
    touch_history_ui_event_state_t event_state;
    bool events_truncated;

    const touch_history_month_t *month;
    bool can_previous_month;
    bool can_next_month;
    bool calendar_loading;
    bool calendar_read_error;

    touch_history_signal_t selected_signal;
    touch_history_ui_stat_snapshot_t stats[TOUCH_HISTORY_UI_STAT_COUNT];

    bool can_previous_night;
    bool can_next_night;
    bool usage_target_known;
    bool usage_on_target;
    bool therapy_only;
    bool cursor_valid;
    int64_t cursor_ms;

    uint16_t progress_per_mille;
    const char *status_text;
    const char *error_text;
    const char *degraded_text;
    /* Non-blocking channel-statistics warning rendered in the graph header;
     * it must not cover or disable an otherwise valid graph. */
    const char *stats_warning_text;
} touch_history_ui_snapshot_t;

/* Allocates the retained UI/context only from external PSRAM. LVGL owns its
 * own object allocations; this module never falls back to internal RAM for
 * the sizeable copied History snapshot. Must be called with the LVGL lock. */
esp_err_t touch_history_ui_create(lv_obj_t *parent,
                                  const touch_history_ui_config_t *config,
                                  touch_history_ui_t **out_ui);

/* Deletes the owned LVGL tree and frees the PSRAM context. */
void touch_history_ui_destroy(touch_history_ui_t *ui);

/* Applies a coherent worker/BSP snapshot and invalidates the graph. Must be
 * called with the LVGL lock. ZOOM_LOADING may omit overview to retain the
 * previously resolved waveform below its non-cancellable read overlay. */
esp_err_t touch_history_ui_apply(touch_history_ui_t *ui,
                                 const touch_history_ui_snapshot_t *snapshot);

lv_obj_t *touch_history_ui_root(touch_history_ui_t *ui);

#ifdef __cplusplus
}
#endif
