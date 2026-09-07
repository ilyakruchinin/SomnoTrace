/*
 * SomnoTrace - worker-backed retained Logs controller
 *
 * Exact filtered counts can require scanning the complete retained ring, so
 * snapshot_page and card export never run in the LVGL task.  Two fixed PSRAM
 * page buffers allow a worker to publish ten rows without racing rendering.
 */

#include "touch_logs_controller.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_stream.h"
#include "psram_task.h"
#include "touch_logs_ui.h"
#if CONFIG_SOMNOTRACE_BOARD_QEMU
#include "touch_logs_qemu.h"
#endif

typedef struct {
    log_stream_retained_line_t page_lines[2][TOUCH_LOGS_UI_VISIBLE_ROWS];
    log_stream_retained_page_t page;
    log_stream_retained_info_t info;
    esp_err_t info_result;
    esp_err_t page_result;
    size_t line_count;
    size_t match_offset;
    uint32_t level_mask;
    char query[TOUCH_LOGS_UI_QUERY_MAX];
    uint64_t before_sequence;
    uint64_t pause_total_count;
    uint32_t desired_revision;
    uint32_t completed_revision;
    uint32_t model_revision;
    uint32_t rendered_revision;
    uint8_t active_buffer;

    touch_logs_ui_save_state_t save_state;
    size_t save_processed;
    size_t save_total;
    size_t saved_count;
    esp_err_t save_result;
    char saved_path[96];
    char save_error[96];

    bool page_valid;
    bool snapshot_busy;
    bool save_busy;
    bool retry_busy;
    bool card_available;
    bool paused;
    bool visible;
    unsigned workers;
} logs_controller_t;

typedef struct {
    logs_controller_t *controller;
    log_stream_retained_filter_t filter;
    char query[TOUCH_LOGS_UI_QUERY_MAX];
    size_t match_offset;
    uint32_t revision;
    uint8_t buffer_index;
} snapshot_job_t;

typedef struct {
    logs_controller_t *controller;
    log_stream_retained_filter_t filter;
    char query[TOUCH_LOGS_UI_QUERY_MAX];
} save_job_t;

static logs_controller_t *s_logs;
static portMUX_TYPE s_logs_lock = portMUX_INITIALIZER_UNLOCKED;

static bool start_snapshot_worker(logs_controller_t *controller);

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static void mark_view_dirty_locked(logs_controller_t *controller)
{
    controller->desired_revision++;
    if (controller->desired_revision == 0) controller->desired_revision = 1;
}

/* Capturing one unfiltered newest slot is bounded and supplies the exact
 * sequence ceiling that retained_info intentionally does not expose. */
static esp_err_t capture_pause_anchor(logs_controller_t *controller)
{
    log_stream_retained_line_t newest;
    log_stream_retained_info_t info = {0};
    size_t count = 0;
    const log_stream_retained_filter_t filter = {
        .level_mask = LOG_STREAM_RETAINED_LEVEL_ALL,
        .order = LOG_STREAM_RETAINED_NEWEST_FIRST,
    };
    esp_err_t result = log_stream_retained_snapshot(
        &newest, 1, &filter, &count, &info);
    if (result != ESP_OK) return result;

    uint64_t before = count > 0 && newest.sequence < UINT64_MAX
                    ? newest.sequence + 1 : count > 0 ? UINT64_MAX : 1;
    portENTER_CRITICAL(&s_logs_lock);
    controller->paused = true;
    controller->before_sequence = before; /* Always non-zero while paused. */
    controller->pause_total_count = info.total_count;
    controller->match_offset = 0;
    controller->info = info;
    controller->info_result = ESP_OK;
    mark_view_dirty_locked(controller);
    portEXIT_CRITICAL(&s_logs_lock);
    return ESP_OK;
}

static void snapshot_task(void *arg)
{
    snapshot_job_t *job = arg;
    logs_controller_t *controller = job->controller;
    log_stream_retained_page_t page = {0};
    log_stream_retained_info_t info = {0};
    esp_err_t result = log_stream_retained_snapshot_page(
        controller->page_lines[job->buffer_index],
        TOUCH_LOGS_UI_VISIBLE_ROWS, &job->filter, job->match_offset,
        &page, &info);

    portENTER_CRITICAL(&s_logs_lock);
    if (job->revision == controller->desired_revision) {
        /* A rolled-over ring can invalidate a previously valid last page.
         * Clamp once and let the next visible refresh request the corrected
         * offset rather than publishing a blank page. */
        if (result == ESP_OK && page.returned == 0 &&
            job->match_offset > 0) {
            controller->match_offset = page.matching_count > 0
                ? ((page.matching_count - 1) /
                   TOUCH_LOGS_UI_VISIBLE_ROWS) * TOUCH_LOGS_UI_VISIBLE_ROWS
                : 0;
            mark_view_dirty_locked(controller);
        } else {
            controller->active_buffer = job->buffer_index;
            controller->page = page;
            controller->line_count = page.returned;
            controller->page_result = result;
            controller->info = info;
            controller->info_result = result;
            controller->page_valid = true;
            controller->completed_revision = job->revision;
            controller->model_revision++;
        }
    }
    controller->snapshot_busy = false;
    controller->workers--;
    portEXIT_CRITICAL(&s_logs_lock);

    heap_caps_free(job);
    psram_task_delete(NULL);
}

static bool start_snapshot_worker(logs_controller_t *controller)
{
    snapshot_job_t *job = heap_caps_calloc(
        1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job) return false;
    /* Only the LVGL task mutates the desired query. Format it before taking
     * the cross-core spinlock; the fixed memcpy below makes publication of
     * the worker job atomic without holding the lock around snprintf. */
    char query[sizeof(job->query)] = {0};
    copy_text(query, sizeof(query), controller->query);

    portENTER_CRITICAL(&s_logs_lock);
    if (controller->snapshot_busy || !controller->visible ||
        !controller->info.available ||
        (controller->page_valid &&
         controller->completed_revision == controller->desired_revision)) {
        portEXIT_CRITICAL(&s_logs_lock);
        heap_caps_free(job);
        return false;
    }
    controller->snapshot_busy = true;
    controller->workers++;
    job->controller = controller;
    job->revision = controller->desired_revision;
    job->match_offset = controller->match_offset;
    job->buffer_index = controller->active_buffer ^ 1U;
    job->filter.level_mask = controller->level_mask;
    job->filter.before_sequence = controller->paused
                                ? controller->before_sequence : 0;
    job->filter.order = LOG_STREAM_RETAINED_NEWEST_FIRST;
    memcpy(job->query, query, sizeof(job->query));
    job->filter.query = job->query;
    portEXIT_CRITICAL(&s_logs_lock);

    TaskHandle_t task = psram_task_create(snapshot_task, "ui_log_page", 4096,
                                           job, 3, 0, NULL, NULL);
    if (task) return true;

    portENTER_CRITICAL(&s_logs_lock);
    controller->snapshot_busy = false;
    controller->workers--;
    controller->page_result = ESP_ERR_NO_MEM;
    controller->page_valid = true;
    controller->completed_revision = controller->desired_revision;
    controller->line_count = 0;
    controller->model_revision++;
    portEXIT_CRITICAL(&s_logs_lock);
    heap_caps_free(job);
    return false;
}

static void save_progress(size_t processed, size_t total, void *ctx)
{
    logs_controller_t *controller = ctx;
    portENTER_CRITICAL(&s_logs_lock);
    controller->save_processed = processed;
    controller->save_total = total;
    controller->model_revision++;
    portEXIT_CRITICAL(&s_logs_lock);
}

static void save_task(void *arg)
{
    save_job_t *job = arg;
    logs_controller_t *controller = job->controller;
    char path[96] = {0};
    size_t saved = 0;
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    esp_err_t result = touch_logs_qemu_save(&job->filter, &saved,
                                           save_progress, controller);
#else
    esp_err_t result = log_stream_retained_save_to_sd(
        &job->filter, path, sizeof(path), &saved, save_progress, controller);
#endif
    char published_path[sizeof(controller->saved_path)] = {0};
    copy_text(published_path, sizeof(published_path), path);

    portENTER_CRITICAL(&s_logs_lock);
    controller->save_busy = false;
    controller->save_state = result == ESP_OK
                           ? TOUCH_LOGS_UI_SAVE_SUCCEEDED
                           : TOUCH_LOGS_UI_SAVE_FAILED;
    controller->save_result = result;
    controller->saved_count = saved;
    memcpy(controller->saved_path, published_path,
           sizeof(controller->saved_path));
    controller->save_error[0] = '\0';
    if (result == ESP_OK) {
        controller->save_processed = controller->save_total;
    }
    controller->model_revision++;
    controller->workers--;
    portEXIT_CRITICAL(&s_logs_lock);

    heap_caps_free(job);
    psram_task_delete(NULL);
}

static void retry_task(void *arg)
{
    logs_controller_t *controller = arg;
    esp_err_t result = log_stream_retained_retry();
    portENTER_CRITICAL(&s_logs_lock);
    controller->retry_busy = false;
    controller->page_result = result;
    controller->page_valid = false;
    mark_view_dirty_locked(controller);
    controller->model_revision++;
    controller->workers--;
    portEXIT_CRITICAL(&s_logs_lock);
    psram_task_delete(NULL);
}

static esp_err_t cb_set_paused(void *ctx, bool paused)
{
    logs_controller_t *controller = ctx;
    if (paused) {
        esp_err_t result = capture_pause_anchor(controller);
        if (result == ESP_OK) (void)start_snapshot_worker(controller);
        return result;
    }
    portENTER_CRITICAL(&s_logs_lock);
    controller->paused = false;
    controller->before_sequence = 0;
    controller->pause_total_count = controller->info.total_count;
    controller->match_offset = 0;
    mark_view_dirty_locked(controller);
    portEXIT_CRITICAL(&s_logs_lock);
    (void)start_snapshot_worker(controller);
    return ESP_OK;
}

static esp_err_t cb_begin_search(void *ctx)
{
    /* Re-anchor even if already paused: search starts a new stable view at the
     * exact moment the touch keyboard opens. */
    logs_controller_t *controller = ctx;
    esp_err_t result = capture_pause_anchor(controller);
    if (result == ESP_OK) (void)start_snapshot_worker(controller);
    return result;
}

static esp_err_t cb_search_query(void *ctx, const char *query)
{
    logs_controller_t *controller = ctx;
    char normalized[sizeof(controller->query)] = {0};
    copy_text(normalized, sizeof(normalized), query);
    portENTER_CRITICAL(&s_logs_lock);
    memcpy(controller->query, normalized, sizeof(controller->query));
    controller->match_offset = 0;
    /* Deliberately preserve paused, before_sequence, and pause total. */
    mark_view_dirty_locked(controller);
    portEXIT_CRITICAL(&s_logs_lock);
    (void)start_snapshot_worker(controller);
    return ESP_OK;
}

static esp_err_t cb_toggle_level(void *ctx, uint32_t bit, bool enabled)
{
    const uint32_t supported = LOG_STREAM_RETAINED_LEVEL_ERROR |
                               LOG_STREAM_RETAINED_LEVEL_WARN |
                               LOG_STREAM_RETAINED_LEVEL_INFO |
                               LOG_STREAM_RETAINED_LEVEL_DEBUG;
    if ((bit & supported) == 0 || (bit & ~supported) != 0)
        return ESP_ERR_INVALID_ARG;
    if ((bit & (bit - 1U)) != 0) return ESP_ERR_INVALID_ARG;
    logs_controller_t *controller = ctx;
    portENTER_CRITICAL(&s_logs_lock);
    if (enabled) controller->level_mask |= bit;
    else controller->level_mask &= ~bit;
    controller->level_mask &= supported;
    if (controller->level_mask == 0)
        controller->level_mask = LOG_STREAM_RETAINED_LEVEL_NONE;
    controller->match_offset = 0;
    mark_view_dirty_locked(controller);
    portEXIT_CRITICAL(&s_logs_lock);
    (void)start_snapshot_worker(controller);
    return ESP_OK;
}

static esp_err_t cb_page_older(void *ctx)
{
    logs_controller_t *controller = ctx;
    if (!controller->paused) {
        esp_err_t result = capture_pause_anchor(controller);
        if (result != ESP_OK) return result;
    }
    portENTER_CRITICAL(&s_logs_lock);
    size_t matching = controller->page.matching_count;
    size_t last_offset = matching > 0
        ? ((matching - 1) / TOUCH_LOGS_UI_VISIBLE_ROWS) *
          TOUCH_LOGS_UI_VISIBLE_ROWS
        : 0;
    if (controller->match_offset < last_offset) {
        size_t next = controller->match_offset + TOUCH_LOGS_UI_VISIBLE_ROWS;
        controller->match_offset = next < last_offset ? next : last_offset;
        mark_view_dirty_locked(controller);
    }
    portEXIT_CRITICAL(&s_logs_lock);
    (void)start_snapshot_worker(controller);
    return ESP_OK;
}

static esp_err_t cb_page_newer(void *ctx)
{
    logs_controller_t *controller = ctx;
    portENTER_CRITICAL(&s_logs_lock);
    size_t previous = controller->match_offset;
    controller->match_offset = previous > TOUCH_LOGS_UI_VISIBLE_ROWS
        ? previous - TOUCH_LOGS_UI_VISIBLE_ROWS : 0;
    if (controller->match_offset != previous)
        mark_view_dirty_locked(controller);
    portEXIT_CRITICAL(&s_logs_lock);
    (void)start_snapshot_worker(controller);
    return ESP_OK;
}

static esp_err_t cb_jump_newest(void *ctx)
{
    logs_controller_t *controller = ctx;
    /* Remain paused, but re-anchor at the newest line so the exact new count
     * restarts from zero without allowing the viewport to move again. */
    esp_err_t result = capture_pause_anchor(controller);
    if (result == ESP_OK) (void)start_snapshot_worker(controller);
    return result;
}

static esp_err_t cb_clear_ram(void *ctx)
{
    logs_controller_t *controller = ctx;
    portENTER_CRITICAL(&s_logs_lock);
    bool busy = controller->save_busy;
    portEXIT_CRITICAL(&s_logs_lock);
    if (busy) return ESP_ERR_INVALID_STATE;
    esp_err_t result = log_stream_retained_clear();
    if (result != ESP_OK) return result;

    if (controller->paused) {
        result = capture_pause_anchor(controller);
        if (result == ESP_OK) (void)start_snapshot_worker(controller);
        return result;
    }
    portENTER_CRITICAL(&s_logs_lock);
    controller->match_offset = 0;
    controller->save_state = TOUCH_LOGS_UI_SAVE_IDLE;
    mark_view_dirty_locked(controller);
    portEXIT_CRITICAL(&s_logs_lock);
    (void)start_snapshot_worker(controller);
    return ESP_OK;
}

static esp_err_t cb_save_card(void *ctx)
{
    logs_controller_t *controller = ctx;
    save_job_t *job = heap_caps_calloc(
        1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job) return ESP_ERR_NO_MEM;
    char query[sizeof(job->query)] = {0};
    copy_text(query, sizeof(query), controller->query);

    portENTER_CRITICAL(&s_logs_lock);
    if (controller->save_busy || !controller->card_available) {
        portEXIT_CRITICAL(&s_logs_lock);
        heap_caps_free(job);
        return ESP_ERR_INVALID_STATE;
    }
    controller->save_busy = true;
    controller->workers++;
    controller->save_state = TOUCH_LOGS_UI_SAVE_RUNNING;
    controller->save_processed = 0;
    controller->save_total = controller->info.retained_count;
    controller->saved_count = 0;
    controller->save_result = ESP_OK;
    controller->saved_path[0] = '\0';
    controller->save_error[0] = '\0';
    controller->model_revision++;
    job->controller = controller;
    job->filter.level_mask = controller->level_mask;
    /* Saving a paused view preserves the same strict sequence ceiling as the
     * rows on screen, excluding lines captured after Pause/search focus. */
    job->filter.before_sequence = controller->paused
                                ? controller->before_sequence : 0;
    job->filter.order = LOG_STREAM_RETAINED_OLDEST_FIRST;
    memcpy(job->query, query, sizeof(job->query));
    job->filter.query = job->query;
    portEXIT_CRITICAL(&s_logs_lock);

    TaskHandle_t task = psram_task_create(save_task, "ui_log_save", 6144,
                                           job, 3, 0, NULL, NULL);
    if (task) return ESP_OK;

    portENTER_CRITICAL(&s_logs_lock);
    controller->save_busy = false;
    controller->workers--;
    controller->save_state = TOUCH_LOGS_UI_SAVE_FAILED;
    controller->save_result = ESP_ERR_NO_MEM;
    controller->model_revision++;
    portEXIT_CRITICAL(&s_logs_lock);
    heap_caps_free(job);
    return ESP_ERR_NO_MEM;
}

static esp_err_t cb_retry(void *ctx)
{
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    touch_logs_qemu_reconnect();
#endif
    logs_controller_t *controller = ctx;
    log_stream_retained_info_t info = {0};
    if (log_stream_retained_get_info(&info) == ESP_OK && info.available) {
        portENTER_CRITICAL(&s_logs_lock);
        controller->page_valid = false;
        mark_view_dirty_locked(controller);
        portEXIT_CRITICAL(&s_logs_lock);
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_logs_lock);
    if (controller->retry_busy) {
        portEXIT_CRITICAL(&s_logs_lock);
        return ESP_ERR_INVALID_STATE;
    }
    controller->retry_busy = true;
    controller->workers++;
    controller->model_revision++;
    portEXIT_CRITICAL(&s_logs_lock);
    TaskHandle_t task = psram_task_create(retry_task, "ui_log_retry", 4096,
                                           controller, 3, 0, NULL, NULL);
    if (task) return ESP_OK;

    portENTER_CRITICAL(&s_logs_lock);
    controller->retry_busy = false;
    controller->workers--;
    controller->page_result = ESP_ERR_NO_MEM;
    controller->model_revision++;
    portEXIT_CRITICAL(&s_logs_lock);
    return ESP_ERR_NO_MEM;
}

#if CONFIG_SOMNOTRACE_BOARD_QEMU
static void cb_simulate_disconnect(void *ctx)
{
    (void)ctx;
    touch_logs_qemu_disconnect();
}
#endif

static const touch_logs_ui_controller_t s_callbacks = {
    .set_paused = cb_set_paused,
    .begin_search = cb_begin_search,
    .search_query = cb_search_query,
    .toggle_level = cb_toggle_level,
    .page_older = cb_page_older,
    .page_newer = cb_page_newer,
    .jump_newest = cb_jump_newest,
    .clear_ram_only = cb_clear_ram,
    .save_card_snapshot = cb_save_card,
    .retry_connection = cb_retry,
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    .simulate_disconnect = cb_simulate_disconnect,
#endif
};

static void publish_model(logs_controller_t *controller)
{
    touch_logs_ui_update_t model = {0};
    uint32_t revision;
    portENTER_CRITICAL(&s_logs_lock);
    if (!controller->visible ||
        controller->rendered_revision == controller->model_revision) {
        portEXIT_CRITICAL(&s_logs_lock);
        return;
    }
    revision = controller->model_revision;
    model.snapshot_result = controller->page_valid
                          ? controller->page_result
                          : controller->info_result;
    model.lines = controller->page_lines[controller->active_buffer];
    model.line_count = controller->page_valid ? controller->line_count : 0;
    model.filter.level_mask = controller->level_mask;
    model.filter.query = controller->query;
    model.filter.before_sequence = controller->paused
                                 ? controller->before_sequence : 0;
    model.filter.order = LOG_STREAM_RETAINED_NEWEST_FIRST;
    model.page = controller->page_valid
               ? controller->page : (log_stream_retained_page_t){0};
    model.info = controller->info;
    model.paused = controller->paused;
    model.pause_anchor_total_count = controller->pause_total_count;
    model.retrying = controller->retry_busy;
    model.card_available = controller->card_available;
    model.save_state = controller->save_state;
    model.save_processed_lines = controller->save_processed;
    model.save_total_lines = controller->save_total;
    model.saved_line_count = controller->saved_count;
    model.save_result = controller->save_result;
    /* Worker page writes always target active_buffer ^ 1, so the active rows
     * remain immutable while an in-flight scan allows status/progress paint. */
    memcpy(model.saved_path, controller->saved_path, sizeof(model.saved_path));
    memcpy(model.save_error, controller->save_error, sizeof(model.save_error));
    portEXIT_CRITICAL(&s_logs_lock);

    if (touch_logs_ui_update(&model) == ESP_OK) {
        portENTER_CRITICAL(&s_logs_lock);
        if (revision > controller->rendered_revision)
            controller->rendered_revision = revision;
        portEXIT_CRITICAL(&s_logs_lock);
    }
}

esp_err_t touch_logs_controller_show(lv_obj_t *parent)
{
    if (!parent) return ESP_ERR_INVALID_ARG;
    if (!s_logs) {
        logs_controller_t *controller = heap_caps_calloc(
            1, sizeof(*controller), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!controller) return ESP_ERR_NO_MEM;
        controller->level_mask = LOG_STREAM_RETAINED_LEVEL_ERROR |
                                 LOG_STREAM_RETAINED_LEVEL_WARN |
                                 LOG_STREAM_RETAINED_LEVEL_INFO;
        controller->desired_revision = 1;
        controller->model_revision = 1;
        controller->page_result = ESP_ERR_INVALID_STATE;
        controller->info_result = log_stream_retained_get_info(&controller->info);

        touch_logs_ui_controller_t callbacks = s_callbacks;
        callbacks.context = controller;
        esp_err_t result = touch_logs_ui_create(parent, &callbacks);
        if (result != ESP_OK) {
            heap_caps_free(controller);
            return result;
        }
        s_logs = controller;
    }

    portENTER_CRITICAL(&s_logs_lock);
    s_logs->visible = true;
    s_logs->model_revision++;
    portEXIT_CRITICAL(&s_logs_lock);
    publish_model(s_logs);
    (void)start_snapshot_worker(s_logs);
    return touch_logs_ui_show();
}

void touch_logs_controller_hide(void)
{
    if (!s_logs) return;
    portENTER_CRITICAL(&s_logs_lock);
    s_logs->visible = false;
    portEXIT_CRITICAL(&s_logs_lock);
    touch_logs_ui_hide();
}

bool touch_logs_controller_is_visible(void)
{
    if (!s_logs) return false;
    portENTER_CRITICAL(&s_logs_lock);
    bool visible = s_logs->visible;
    portEXIT_CRITICAL(&s_logs_lock);
    return visible;
}

bool touch_logs_controller_is_paused(void)
{
    if (!s_logs) return false;
    portENTER_CRITICAL(&s_logs_lock);
    bool paused = s_logs->paused;
    portEXIT_CRITICAL(&s_logs_lock);
    return paused;
}

void touch_logs_controller_refresh(bool card_available)
{
    logs_controller_t *controller = s_logs;
    if (!controller || !touch_logs_controller_is_visible()) return;

    log_stream_retained_info_t info = {0};
    esp_err_t info_result = log_stream_retained_get_info(&info);
#if CONFIG_SOMNOTRACE_BOARD_QEMU
    if (touch_logs_qemu_disconnected()) {
        info_result = ESP_ERR_INVALID_STATE;
        info.available = false;
    }
#endif
    bool start_snapshot = false;
    portENTER_CRITICAL(&s_logs_lock);
    bool available_changed = controller->info.available != info.available ||
                             controller->info_result != info_result;
    bool total_changed = controller->info.total_count != info.total_count;
    bool generation_changed = controller->info.generation != info.generation;
    if (controller->card_available != card_available) {
        controller->card_available = card_available;
        controller->model_revision++;
    }
    controller->info = info;
    controller->info_result = info_result;
    if (available_changed || (controller->paused && total_changed))
        controller->model_revision++;
    if (!info.available || info_result != ESP_OK) {
        controller->page_result = info_result != ESP_OK
                                ? info_result : info.last_error;
        /* Keep the last completed page readable during service recovery. */
    } else if (!controller->snapshot_busy) {
        if (!controller->paused && generation_changed &&
            controller->completed_revision == controller->desired_revision) {
            mark_view_dirty_locked(controller);
        }
        start_snapshot = !controller->page_valid ||
            controller->completed_revision != controller->desired_revision;
    }
    portEXIT_CRITICAL(&s_logs_lock);

    publish_model(controller);
    if (start_snapshot) (void)start_snapshot_worker(controller);
}

esp_err_t touch_logs_controller_destroy(void)
{
    logs_controller_t *controller = s_logs;
    if (!controller) return ESP_OK;
    portENTER_CRITICAL(&s_logs_lock);
    if (controller->workers != 0 || controller->snapshot_busy ||
        controller->save_busy || controller->retry_busy) {
        portEXIT_CRITICAL(&s_logs_lock);
        return ESP_ERR_INVALID_STATE;
    }
    controller->visible = false;
    s_logs = NULL;
    portEXIT_CRITICAL(&s_logs_lock);
    touch_logs_ui_destroy();
    heap_caps_free(controller);
    return ESP_OK;
}
