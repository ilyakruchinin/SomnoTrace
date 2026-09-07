/*
 * SomnoTrace - bounded native Logs pane for the Waveshare 7B
 *
 * This module owns presentation only.  A controller takes retained-ring
 * snapshots and executes potentially blocking log/card operations elsewhere.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "log_stream.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOUCH_LOGS_UI_WIDTH          768U
#define TOUCH_LOGS_UI_HEIGHT         450U
#define TOUCH_LOGS_UI_VISIBLE_ROWS    10U
#define TOUCH_LOGS_UI_QUERY_MAX        48U

typedef enum {
    TOUCH_LOGS_UI_SAVE_IDLE = 0,
    TOUCH_LOGS_UI_SAVE_RUNNING,
    TOUCH_LOGS_UI_SAVE_SUCCEEDED,
    TOUCH_LOGS_UI_SAVE_FAILED,
} touch_logs_ui_save_state_t;

/* Borrowed retained-page input. `lines` and `filter.query` only need to stay
 * valid for the duration of touch_logs_ui_update().  The filter must use
 * LOG_STREAM_RETAINED_NEWEST_FIRST.  While paused, the controller must keep a
 * non-zero before_sequence anchor so incoming ring lines cannot move the page.
 * pause_anchor_total_count is the total_count captured at the instant Pause
 * (or search focus) began; the UI derives the exact new-line count from it.
 */
typedef struct {
    esp_err_t snapshot_result;
    const log_stream_retained_line_t *lines;
    size_t line_count;
    log_stream_retained_filter_t filter;
    log_stream_retained_page_t page;
    log_stream_retained_info_t info;

    bool paused;
    uint64_t pause_anchor_total_count;
    bool retrying;
    bool card_available;

    touch_logs_ui_save_state_t save_state;
    size_t save_processed_lines;
    size_t save_total_lines;
    size_t saved_line_count;
    esp_err_t save_result;
    char saved_path[96];
    char save_error[96];
} touch_logs_ui_update_t;

/* Every operation callback is emitted from an LV_EVENT_PRESSED handler and
 * must return promptly.  Slow retained scans and card writes belong on a
 * worker.  begin_search must atomically pause/anchor the view.  search_query
 * commits and dismisses the keyboard but must not resume. Paging enters or
 * remains in an anchored paused view. jump_newest changes the paused page
 * offset only; it must not resume either.
 */
typedef struct {
    void *context;
    esp_err_t (*set_paused)(void *context, bool paused);
    esp_err_t (*begin_search)(void *context);
    esp_err_t (*search_query)(void *context, const char *query);
    esp_err_t (*toggle_level)(void *context, uint32_t level_bit, bool enabled);
    esp_err_t (*page_older)(void *context);
    esp_err_t (*page_newer)(void *context);
    esp_err_t (*jump_newest)(void *context);
    esp_err_t (*clear_ram_only)(void *context);
    esp_err_t (*save_card_snapshot)(void *context);
    esp_err_t (*retry_connection)(void *context);
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    void (*simulate_disconnect)(void *context);
#endif
} touch_logs_ui_controller_t;

/* LVGL calls must run on its task or while the application holds its LVGL
 * lock.  create() is lazy and uses PSRAM only for its context; there is no
 * internal-RAM fallback.  show() renders a complete first frame before making
 * the 768 x 450 detail pane visible. */
esp_err_t touch_logs_ui_create(
    lv_obj_t *parent, const touch_logs_ui_controller_t *controller);
void touch_logs_ui_destroy(void);
esp_err_t touch_logs_ui_show(void);
void touch_logs_ui_hide(void);
bool touch_logs_ui_is_visible(void);

esp_err_t touch_logs_ui_update(const touch_logs_ui_update_t *snapshot);
lv_obj_t *touch_logs_ui_root(void);

#ifdef __cplusplus
}
#endif
