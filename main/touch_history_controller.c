/* Single-worker, generation-safe native History controller. */
#include "touch_history_controller.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psram_task.h"
#include "sd_storage.h"
#include "history_cache.h"

#define HISTORY_CONTROLLER_QUEUE_LENGTH 1U
#define HISTORY_CONTROLLER_WORKER_STACK 12288U
#define HISTORY_CONTROLLER_ZOOM_90_MIN_MS (90LL * 60LL * 1000LL)
#define HISTORY_CONTROLLER_ZOOM_22_MIN_MS (22LL * 60LL * 1000LL)
#define HISTORY_CONTROLLER_ZOOM_10_MIN_MS (10LL * 60LL * 1000LL)
#define HISTORY_CONTROLLER_ZOOM_5_MIN_MS  (5LL * 60LL * 1000LL)
#define HISTORY_CONTROLLER_PROGRESS_STEP  20U

static const char *const TAG = "history_ctl";

static const char *const s_history_preview_days[] = {
    "20260901", "20260831", "20260830", "20260829",
    "20260828", "20260827", "20260826", "20260825",
    "20260824", "20260823", "20260822", "20260821",
};
#define HISTORY_PREVIEW_DAY_COUNT \
    (sizeof(s_history_preview_days) / sizeof(s_history_preview_days[0]))

typedef enum {
    HISTORY_JOB_INITIAL = 0,
    HISTORY_JOB_PAGE,
    HISTORY_JOB_DAY,
    HISTORY_JOB_VIEW,
    HISTORY_JOB_MONTH,
    HISTORY_JOB_STOP,
} history_job_kind_t;

typedef struct {
    history_job_kind_t kind;
    uint32_t generation;
    size_t page_offset;
    size_t global_index;
    touch_history_signal_t signal;
    int64_t window_start_ms;
    int64_t window_end_ms;
    uint16_t year;
    uint8_t month;
    bool therapy_only;
    /* Suppresses the user-facing Cancel control. The underlying read remains
     * generation-cancellable so tab changes and newer intents stop stale I/O. */
    bool non_cancellable;
    char day[9];
} history_job_t;

typedef struct {
    touch_history_day_t days[TOUCH_HISTORY_UI_LIST_ROWS];
    touch_history_index_page_t page;
    size_t selected_row;
    size_t selected_global_index;

    touch_history_night_t night;
    touch_history_session_t sessions[TOUCH_HISTORY_UI_MAX_SESSIONS];
    size_t session_count;
    touch_history_overview_t overview;
    touch_history_event_t events[TOUCH_HISTORY_UI_MAX_VISIBLE_EVENTS];
    size_t event_count;
    size_t event_total_count;
    touch_history_stats_t stats;
    touch_history_month_t month;

    touch_history_ui_state_t state;
    touch_history_ui_rail_mode_t rail_mode;
    touch_history_signal_t signal;
    int64_t window_start_ms;
    int64_t window_end_ms;
    int64_t cursor_ms;
    uint16_t progress_per_mille;
    bool details_pending;
    uint32_t source_generation;
    bool has_night;
    bool has_overview;
    bool has_month;
    bool has_newest_month;
    bool cursor_valid;
    bool therapy_only;
    bool usage_target_known;
    bool usage_on_target;
    bool events_truncated;
    bool calendar_loading;
    bool calendar_read_error;
    touch_history_ui_event_state_t event_state;
    uint16_t newest_year;
    uint8_t newest_month;
    char status[TOUCH_HISTORY_UI_TEXT_MAX];
    char error[TOUCH_HISTORY_UI_TEXT_MAX];
    char degraded[TOUCH_HISTORY_UI_TEXT_MAX];
    char stats_warning[TOUCH_HISTORY_UI_TEXT_MAX];
} history_model_t;

/* Small LVGL-side decision snapshot. The 480-bin overview and event/session
 * arrays never leave the PSRAM model merely to interpret a touch intent. */
typedef struct {
    touch_history_day_t days[TOUCH_HISTORY_UI_LIST_ROWS];
    touch_history_index_page_t page;
    touch_history_night_t night;
    touch_history_month_t month;
    size_t selected_global_index;
    touch_history_signal_t signal;
    int64_t window_start_ms;
    int64_t window_end_ms;
    int64_t cursor_ms;
    bool has_night;
    bool has_month;
    bool cursor_valid;
    bool therapy_only;
    touch_history_ui_rail_mode_t rail_mode;
    bool calendar_loading;
} history_navigation_t;

struct touch_history_controller {
    history_model_t model;
    touch_history_overview_t preview_source;
    uint32_t preview_source_generation;
    touch_history_controller_config_t config;
    SemaphoreHandle_t mutex;
    QueueHandle_t queue;
    TaskHandle_t worker;
    history_job_t retry_job;
    history_job_t pending_month;
    history_job_t active_month;
    uint32_t generation;
    uint32_t calendar_generation;
    uint32_t revision;
    uint32_t rendered_revision;
    bool active;
    bool ever_loaded;
    bool month_pending;
    bool month_running;
    bool closing;
};

typedef struct {
    touch_history_controller_t *controller;
    uint32_t generation;
    uint32_t source_generation;
    uint16_t base;
    uint16_t span;
    bool calendar;
} history_operation_context_t;

static esp_err_t history_controller_enqueue_locked(
    touch_history_controller_t *controller, history_job_t *job,
    bool *calendar_state_changed);
static void history_controller_month_from_day(const char day[9],
                                              uint16_t *year,
                                              uint8_t *month);

static void history_controller_remember_newest_month(history_model_t *model)
{
    if (!model || model->page.offset != 0 || !model->page.returned)
        return;
    uint16_t year = 0;
    uint8_t month = 0;
    history_controller_month_from_day(model->days[0].day, &year, &month);
    if (year < 2000 || year > 2200 || month < 1 || month > 12)
        return;
    model->newest_year = year;
    model->newest_month = month;
    model->has_newest_month = true;
}

static void history_controller_notify(touch_history_controller_t *controller)
{
    if (controller && controller->config.changed)
        controller->config.changed(controller->config.context);
}

static void history_controller_text(char *out, size_t size, const char *text)
{
    if (!out || !size) return;
    snprintf(out, size, "%s", text ? text : "");
}

static bool history_controller_can_advance_month(const history_model_t *model)
{
    const touch_history_month_t *month = model ? &model->month : NULL;
    if (!month || month->month < 1 || month->month > 12)
        return false;

    unsigned upper_year = 0;
    unsigned upper_month = 0;
    time_t now = time(NULL);
    struct tm local = {0};
    if (now >= 1700000000 && localtime_r(&now, &local)) {
        upper_year = (unsigned)local.tm_year + 1900U;
        upper_month = (unsigned)local.tm_mon + 1U;
    }
    /* A bedside device can boot and browse its card before NTP has restored
     * wall time. Retain the newest indexed night as a second safe upper bound
     * so Previous never becomes a one-way trip while the clock is invalid. */
    if (model && model->has_newest_month &&
        (model->newest_year > upper_year ||
         (model->newest_year == upper_year &&
          model->newest_month > upper_month))) {
        upper_year = model->newest_year;
        upper_month = model->newest_month;
    }
    if (!upper_year || !upper_month)
        return false;
    return month->year < upper_year ||
           (month->year == upper_year && month->month < upper_month);
}

static bool history_controller_generation_current(
    touch_history_controller_t *controller, uint32_t generation,
    bool calendar)
{
    bool current = false;
    if (!controller || xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE)
        return false;
    current = !controller->closing && controller->active &&
        (calendar ? controller->calendar_generation == generation
                  : controller->generation == generation);
    xSemaphoreGive(controller->mutex);
    return current;
}

static bool history_controller_job_current(
    touch_history_controller_t *controller, const history_job_t *job)
{
    bool current = false;
    if (!controller || !job ||
        xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE)
        return false;
    current = !controller->closing && controller->active &&
        (job->kind == HISTORY_JOB_MONTH
             ? controller->calendar_generation == job->generation
             : controller->generation == job->generation);
    xSemaphoreGive(controller->mutex);
    return current;
}

static bool history_controller_should_cancel(void *context)
{
    history_operation_context_t *operation = context;
    return !operation || operation->source_generation != sd_storage_content_generation() ||
        !history_controller_generation_current(
        operation->controller, operation->generation, operation->calendar);
}

static void history_controller_progress(void *context, uint16_t per_mille)
{
    history_operation_context_t *operation = context;
    if (!operation || !operation->controller) return;
    touch_history_controller_t *controller = operation->controller;
    uint16_t mapped = operation->base + (uint16_t)(
        (uint32_t)operation->span * per_mille / 1000U);
    bool changed = false;
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
        uint16_t current = controller->model.progress_per_mille;
        bool meaningful_step = mapped > current &&
            ((uint16_t)(mapped - current) >= HISTORY_CONTROLLER_PROGRESS_STEP ||
             mapped >= 1000U);
        if (!controller->closing && controller->active &&
            controller->generation == operation->generation &&
            meaningful_step) {
            controller->model.progress_per_mille = mapped;
            controller->revision++;
            changed = true;
        }
        xSemaphoreGive(controller->mutex);
    }
    if (changed) history_controller_notify(controller);
}

static touch_history_operation_t history_controller_operation(
    history_operation_context_t *context,
    touch_history_controller_t *controller, uint32_t generation,
    uint16_t base, uint16_t span, bool calendar)
{
    *context = (history_operation_context_t) {
        .controller = controller,
        .generation = generation,
        .source_generation = sd_storage_content_generation(),
        .base = base,
        .span = span,
        .calendar = calendar,
    };
    return (touch_history_operation_t) {
        .should_cancel = history_controller_should_cancel,
        .progress = history_controller_progress,
        .context = context,
    };
}

static void history_controller_begin_job(touch_history_controller_t *controller,
                                         const history_job_t *job)
{
    /* Month navigation only replaces the left rail. Keep the selected-night
     * lifecycle and right detail visible while the compact month index loads. */
    if (job->kind == HISTORY_JOB_MONTH)
        return;
    bool notify = false;
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
        if (!controller->closing && controller->generation == job->generation) {
            controller->model.state = job->kind == HISTORY_JOB_VIEW || job->non_cancellable
                ? TOUCH_HISTORY_UI_STATE_ZOOM_LOADING
                : TOUCH_HISTORY_UI_STATE_AUTO_LOADING;
            controller->model.details_pending = true;
            controller->model.progress_per_mille = 0;
            controller->model.error[0] = '\0';
            history_controller_text(
                controller->model.status, sizeof(controller->model.status),
                job->kind == HISTORY_JOB_VIEW ? "Updating…" : "Loading History…");
            controller->revision++;
            notify = true;
        }
        xSemaphoreGive(controller->mutex);
    }
    if (notify) history_controller_notify(controller);
}

static bool history_controller_copy_model(touch_history_controller_t *controller,
                                          history_model_t *result)
{
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE)
        return false;
    *result = controller->model;
    xSemaphoreGive(controller->mutex);
    return true;
}

static bool history_controller_job_selects_night(const history_job_t *job)
{
    if (!job)
        return false;
    return job->kind == HISTORY_JOB_INITIAL ||
           job->kind == HISTORY_JOB_DAY ||
           (job->kind == HISTORY_JOB_PAGE &&
            job->global_index != SIZE_MAX);
}

/* Caller owns controller->mutex. A night selection and the Calendar rail are
 * published by independent jobs, so reconcile them at the foreground commit
 * boundary. This makes the selected night win even when Calendar was opened
 * while that DAY/INITIAL/PAGE job was still queued or running. VIEW jobs are
 * intentionally excluded: changing a channel or zoom must not snap a month
 * the user is deliberately browsing back to the selected night. */
static void history_controller_reconcile_calendar_locked(
    touch_history_controller_t *controller, const history_job_t *job)
{
    if (!controller || !history_controller_job_selects_night(job) ||
        controller->model.rail_mode != TOUCH_HISTORY_UI_RAIL_CALENDAR ||
        !controller->model.has_night)
        return;

    uint16_t year = 0;
    uint8_t month = 0;
    history_controller_month_from_day(
        controller->model.night.day, &year, &month);
    if (!year || !month)
        return;

    bool pending_matches = controller->month_pending &&
        controller->pending_month.year == year &&
        controller->pending_month.month == month;
    bool published_matches = controller->model.has_month &&
        controller->model.month.year == year &&
        controller->model.month.month == month;
    bool aligned = controller->model.calendar_loading
        ? pending_matches : published_matches;
    if (aligned)
        return;

    history_job_t month_job = {
        .kind = HISTORY_JOB_MONTH,
        .year = year,
        .month = month,
        .signal = controller->model.signal,
        .global_index = SIZE_MAX,
    };
    (void)history_controller_enqueue_locked(
        controller, &month_job, NULL);
}

static bool history_controller_publish(touch_history_controller_t *controller,
                                       const history_job_t *job,
                                       const history_model_t *result)
{
    bool published = false;
    if (job->kind != HISTORY_JOB_MONTH && result->source_generation &&
        result->source_generation != sd_storage_content_generation()) return false;
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE)
        return false;
    bool generation_current = job->kind == HISTORY_JOB_MONTH
        ? controller->calendar_generation == job->generation
        : controller->generation == job->generation;
    if (!controller->closing && controller->active && generation_current) {
        if (job->kind == HISTORY_JOB_MONTH) {
            /* A month job owns only compact rail metadata. Never publish its
             * stale copy of the night, cursor, lifecycle, retry, or progress. */
            controller->model.month = result->month;
            controller->model.has_month = result->has_month;
            controller->model.calendar_loading = false;
            controller->model.calendar_read_error = false;
        } else {
            touch_history_ui_rail_mode_t rail_mode =
                controller->model.rail_mode;
            touch_history_month_t month = controller->model.month;
            bool has_month = controller->model.has_month;
            bool calendar_loading = controller->model.calendar_loading;
            bool calendar_read_error =
                controller->model.calendar_read_error;
            bool same_night = controller->model.has_night && result->has_night &&
                !strcmp(controller->model.night.day, result->night.day);
            int64_t cursor_ms = controller->model.cursor_ms;
            bool cursor_valid = controller->model.cursor_valid;
            controller->model = *result;
            if (same_night && (!cursor_valid ||
                (cursor_ms >= result->night.axis_start_ms && cursor_ms < result->night.axis_end_ms))) {
                controller->model.cursor_ms = cursor_ms;
                controller->model.cursor_valid = cursor_valid;
            }
            if (result->has_overview && !result->overview.preview) {
                touch_history_overview_t *preview = &controller->preview_source;
                uint32_t epoch = sd_storage_content_generation();
                if (!same_night || controller->preview_source_generation != epoch ||
                    preview->signal != result->signal || !preview->loaded ||
                    result->overview.axis_end_ms - result->overview.axis_start_ms >=
                    preview->axis_end_ms - preview->axis_start_ms) {
                    *preview = result->overview;
                    controller->preview_source_generation = epoch;
                }
            }
            /* Rail selection is synchronous. Foreground worker results must
             * not close it or roll back a month published between intents. */
            controller->model.rail_mode = rail_mode;
            if (job->kind != HISTORY_JOB_INITIAL) {
                controller->model.month = month;
                controller->model.has_month = has_month;
                controller->model.calendar_loading = calendar_loading;
                controller->model.calendar_read_error = calendar_read_error;
            } else if (calendar_loading) {
                /* A month selection can be queued while a full refresh owns
                 * the worker. Keep that pending state visible until its
                 * independently-versioned month result is published. */
                controller->model.calendar_loading = true;
                controller->model.calendar_read_error = false;
            }
            controller->model.progress_per_mille = 1000;
            controller->retry_job = *job;
            controller->ever_loaded = true;
            history_controller_reconcile_calendar_locked(controller, job);
        }
        controller->revision++;
        published = true;
    }
    xSemaphoreGive(controller->mutex);
    if (published) history_controller_notify(controller);
    return published;
}

static void history_controller_publish_error(
    touch_history_controller_t *controller, const history_job_t *job,
    esp_err_t error)
{
    bool notify = false;
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE) return;
    bool generation_current = job->kind == HISTORY_JOB_MONTH
        ? controller->calendar_generation == job->generation
        : controller->generation == job->generation;
    if (!controller->closing && controller->active && generation_current) {
        if (job->kind == HISTORY_JOB_MONTH) {
            /* A calendar-index failure belongs to the left rail. It must not
             * replace a still-valid selected night with the detail error. */
            controller->model.calendar_loading = false;
            controller->model.calendar_read_error = true;
        } else {
            bool retain_graph = controller->model.has_overview &&
                (job->kind == HISTORY_JOB_VIEW || controller->model.state == TOUCH_HISTORY_UI_STATE_READY);
            controller->model.state = retain_graph ? TOUCH_HISTORY_UI_STATE_DEGRADED_UNKNOWN
                                                   : TOUCH_HISTORY_UI_STATE_READ_ERROR;
            if (retain_graph) {
                history_controller_text(controller->model.degraded, sizeof(controller->model.degraded),
                    controller->model.overview.preview
                        ? "Update interrupted · showing preview. Use zoom to retry."
                        : "Details interrupted · graph retained. Use zoom to retry.");
                history_controller_text(controller->model.stats_warning, sizeof(controller->model.stats_warning),
                    "Percentiles unavailable for this window");
            }
            controller->model.progress_per_mille = 0;
            controller->model.status[0] = '\0';
            if (error == ESP_ERR_NOT_FOUND) {
                history_controller_text(controller->model.error,
                                        sizeof(controller->model.error),
                                        "No readable History data was found.");
            } else if (error == ESP_ERR_NO_MEM) {
                history_controller_text(controller->model.error,
                                        sizeof(controller->model.error),
                                        "History ran out of external memory.");
            } else if (error == ESP_ERR_TIMEOUT ||
                       error == ESP_ERR_INVALID_STATE) {
                history_controller_text(controller->model.error,
                                        sizeof(controller->model.error),
                                        "The card is busy. Try again in a moment.");
            } else {
                history_controller_text(controller->model.error,
                                        sizeof(controller->model.error),
                                        "Could not read the microSD card.");
            }
        }
        if (job->kind != HISTORY_JOB_MONTH)
            controller->retry_job = *job;
        controller->revision++;
        notify = true;
    }
    xSemaphoreGive(controller->mutex);
    if (notify) history_controller_notify(controller);
}

static touch_history_signal_t history_controller_first_signal(uint16_t mask)
{
    for (int signal = 0; signal < TOUCH_HISTORY_SIGNAL_COUNT; ++signal) {
        if (mask & TOUCH_HISTORY_SIGNAL_BIT(signal))
            return (touch_history_signal_t)signal;
    }
    return TOUCH_HISTORY_SIGNAL_FLOW;
}

static bool history_controller_signal_available(const history_model_t *model,
                                                touch_history_signal_t signal)
{
    return model->has_night && signal >= TOUCH_HISTORY_SIGNAL_FLOW &&
           signal < TOUCH_HISTORY_SIGNAL_COUNT &&
           (model->night.available_signals &
            TOUCH_HISTORY_SIGNAL_BIT(signal)) != 0;
}

static void history_controller_update_selection(history_model_t *model,
                                                const char day[9],
                                                size_t global_index)
{
    model->selected_row = SIZE_MAX;
    model->selected_global_index = global_index;
    for (size_t i = 0; i < model->page.returned; ++i) {
        if (!strcmp(model->days[i].day, day)) {
            model->selected_row = i;
            if (global_index == SIZE_MAX)
                model->selected_global_index = model->page.offset + i;
            break;
        }
    }
}

static esp_err_t history_controller_load_events(
    const history_job_t *job, history_model_t *model,
    const touch_history_operation_t *operation)
{
    model->event_count = 0;
    model->event_total_count = 0;
    model->events_truncated = false;
    model->event_state = TOUCH_HISTORY_UI_EVENT_STATE_UNAVAILABLE;
    touch_history_event_page_t page = {0};
    esp_err_t result = touch_history_load_window_events_ex(job->day,
        model->window_start_ms, model->window_end_ms, model->events,
        TOUCH_HISTORY_UI_MAX_VISIBLE_EVENTS, &page, operation);
    if (result != ESP_OK) return result;
    model->event_count = page.returned;
    model->event_total_count = page.total_count;
    model->events_truncated = page.has_more;
    model->event_state = page.totals.complete
        ? TOUCH_HISTORY_UI_EVENT_STATE_COMPLETE : TOUCH_HISTORY_UI_EVENT_STATE_INCOMPLETE;
    return ESP_OK;
}

static esp_err_t history_controller_load_view(
    touch_history_controller_t *controller, const history_job_t *job,
    history_model_t *model, bool load_night)
{
    history_operation_context_t operation_context;
    touch_history_operation_t operation = history_controller_operation(
        &operation_context, controller, job->generation, 50, 900, false);
    /* The updating badge has no progress bar. New navigation, source changes,
     * or leaving History still cancel every stage of its quiet card reads. */
    if (job->kind == HISTORY_JOB_VIEW || job->non_cancellable)
        operation.progress = NULL;
    const touch_history_operation_t *interruptible = &operation;

    uint32_t source_generation = sd_storage_content_generation();
    load_night = load_night || model->source_generation != source_generation;
    if (load_night) {
        bool same_night = model->has_night &&
                          !strcmp(model->night.day, job->day);
        memset(&model->night, 0, sizeof(model->night));
        memset(model->sessions, 0, sizeof(model->sessions));
        esp_err_t night_result = touch_history_load_night_ex(
            job->day, &model->night, model->sessions,
            TOUCH_HISTORY_UI_MAX_SESSIONS, interruptible);
        if (night_result != ESP_OK) {
            ESP_LOGE(TAG, "night day=%s failed: %s (0x%x)", job->day,
                     esp_err_to_name(night_result), (unsigned)night_result);
            return night_result;
        }
        model->has_night = true;
        model->source_generation = source_generation;
        model->session_count = model->night.sessions_returned;
        history_controller_update_selection(model, job->day,
                                            job->global_index);
        if (!history_controller_signal_available(model, job->signal))
            model->signal = history_controller_first_signal(
                model->night.available_signals);
        else
            model->signal = job->signal;
        model->therapy_only = job->therapy_only &&
                              model->signal == TOUCH_HISTORY_SIGNAL_SPO2;
        model->window_start_ms = job->window_start_ms > 0
            ? job->window_start_ms : model->night.axis_start_ms;
        model->window_end_ms = job->window_end_ms > model->window_start_ms
            ? job->window_end_ms : model->night.axis_end_ms;
        /* Recovery/import can shorten a source while this window is retained.
         * Clamp to the reloaded extent so navigation can recover immediately. */
        if (model->window_start_ms < model->night.axis_start_ms)
            model->window_start_ms = model->night.axis_start_ms;
        if (model->window_end_ms > model->night.axis_end_ms)
            model->window_end_ms = model->night.axis_end_ms;
        if (model->window_end_ms <= model->window_start_ms) {
            model->window_start_ms = model->night.axis_start_ms;
            model->window_end_ms = model->night.axis_end_ms;
        }
        if (!same_night ||
            model->cursor_ms < model->night.axis_start_ms ||
            model->cursor_ms >= model->night.axis_end_ms) {
            model->cursor_ms = 0;
            model->cursor_valid = false;
        }
    } else {
        model->signal = job->signal;
        model->therapy_only = job->therapy_only &&
                              job->signal == TOUCH_HISTORY_SIGNAL_SPO2;
        model->window_start_ms = job->window_start_ms;
        model->window_end_ms = job->window_end_ms;
    }
    if (!model->has_night || model->window_start_ms < model->night.axis_start_ms ||
        model->window_end_ms > model->night.axis_end_ms ||
        model->window_end_ms <= model->window_start_ms)
        return ESP_ERR_INVALID_ARG;

    touch_history_overview_t *next_overview = &model->overview;
    esp_err_t graph_result;
    bool fit = model->window_start_ms == model->night.axis_start_ms &&
               model->window_end_ms == model->night.axis_end_ms;
    graph_result = touch_history_load_view_ex(
        job->day, model->signal,
        fit ? 0 : model->window_start_ms,
        fit ? 0 : model->window_end_ms,
        model->therapy_only, next_overview, interruptible);
    bool graph_missing = graph_result == ESP_ERR_NOT_FOUND;
    if (graph_result != ESP_OK && !graph_missing) {
        ESP_LOGE(TAG,
                 "graph day=%s signal=%u window=%lld..%lld failed: %s (0x%x)",
                 job->day, (unsigned)model->signal,
                 (long long)model->window_start_ms,
                 (long long)model->window_end_ms,
                 esp_err_to_name(graph_result), (unsigned)graph_result);
        return graph_result;
    }
    if (graph_missing)
        ESP_LOGW(TAG, "graph day=%s signal=%u has no readable samples",
                 job->day, (unsigned)model->signal);

    /* The drawable snapshot has its own commit. Exact percentiles and event
     * IO must never delay it or attach values from the previous window. */
    memset(&model->stats, 0, sizeof(model->stats));
    model->event_count = model->event_total_count = 0;
    model->events_truncated = false;
    model->event_state = TOUCH_HISTORY_UI_EVENT_STATE_UNAVAILABLE;
    model->has_overview = true;
    model->details_pending = true;
    model->state = TOUCH_HISTORY_UI_STATE_READY;
    model->status[0] = model->error[0] = model->degraded[0] = '\0';
    history_controller_text(model->stats_warning, sizeof(model->stats_warning),
        model->signal == TOUCH_HISTORY_SIGNAL_MOTION ? "" : "Calculating percentiles…");
    if (!history_controller_publish(controller, job, model)) return TOUCH_HISTORY_ERR_CANCELLED;

    esp_err_t events_result = history_controller_load_events(
        job, model, interruptible);
    if (events_result == TOUCH_HISTORY_ERR_CANCELLED)
        return events_result;
    bool events_unavailable = events_result != ESP_OK;
    if (events_unavailable) {
        model->event_count = 0;
        model->event_total_count = 0;
        model->events_truncated = false;
        model->event_state = TOUCH_HISTORY_UI_EVENT_STATE_UNAVAILABLE;
        ESP_LOGW(TAG, "events day=%s unavailable: %s (0x%x)", job->day,
                 esp_err_to_name(events_result), (unsigned)events_result);
    }

    if (!history_controller_publish(controller, job, model)) return TOUCH_HISTORY_ERR_CANCELLED;

    touch_history_stats_t *next_stats = &model->stats;
    esp_err_t stats_result = touch_history_load_stats_ex(
        job->day, model->signal, model->window_start_ms,
        model->window_end_ms, model->therapy_only, next_stats,
        interruptible);
    if (stats_result == TOUCH_HISTORY_ERR_CANCELLED)
        return stats_result;
    bool stats_unavailable = stats_result != ESP_OK;
    if (stats_unavailable)
        ESP_LOGW(TAG,
                 "stats day=%s signal=%u window=%lld..%lld unavailable: %s (0x%x)",
                 job->day, (unsigned)model->signal,
                 (long long)model->window_start_ms,
                 (long long)model->window_end_ms,
                 esp_err_to_name(stats_result), (unsigned)stats_result);


    model->has_overview = true;
    model->details_pending = false;
    model->state = TOUCH_HISTORY_UI_STATE_READY;
    model->status[0] = '\0';
    model->error[0] = '\0';
    model->degraded[0] = '\0';
    model->stats_warning[0] = '\0';
    if (stats_unavailable && model->signal != TOUCH_HISTORY_SIGNAL_MOTION)
        history_controller_text(
            model->stats_warning, sizeof(model->stats_warning),
            "Percentiles unavailable for this window");
    if (graph_missing) {
        model->state = TOUCH_HISTORY_UI_STATE_DEGRADED_UNKNOWN;
        history_controller_text(model->degraded, sizeof(model->degraded),
                                "No readable samples for this channel.");
    } else if (model->overview.unreadable_sessions ||
               model->night.has_session_errors) {
        model->state = TOUCH_HISTORY_UI_STATE_DEGRADED_UNKNOWN;
        history_controller_text(
            model->degraded, sizeof(model->degraded),
            "Some recorded segments could not be read. Whole-night values may be incomplete.");
    } else if (model->night.has_oximetry_error) {
        model->state = TOUCH_HISTORY_UI_STATE_DEGRADED_UNKNOWN;
        history_controller_text(
            model->degraded, sizeof(model->degraded),
            "An O₂ recording was found but could not be read.");
    } else if (model->night.has_summary_error) {
        model->state = TOUCH_HISTORY_UI_STATE_DEGRADED_UNKNOWN;
        history_controller_text(
            model->degraded, sizeof(model->degraded),
            "Device summary values are unavailable; recorded traces are still shown.");
    } else if (model->night.has_event_loss) {
        model->state = TOUCH_HISTORY_UI_STATE_DEGRADED_UNKNOWN;
        history_controller_text(
            model->degraded, sizeof(model->degraded),
            "Some respiratory events were dropped. ST AHI is unavailable.");
    } else if (model->night.sessions_truncated) {
        model->state = TOUCH_HISTORY_UI_STATE_DEGRADED_UNKNOWN;
        history_controller_text(model->degraded, sizeof(model->degraded),
                                "Some session captions could not be shown.");
    }

    const touch_history_day_t *selected = model->selected_row < model->page.returned
        ? &model->days[model->selected_row] : NULL;
    model->usage_target_known = controller->config.usage_target_minutes > 0;
    model->usage_on_target = selected && selected->has_usage &&
        selected->usage_min >= controller->config.usage_target_minutes;
    return history_controller_publish(controller, job, model)
        ? ESP_OK : TOUCH_HISTORY_ERR_CANCELLED;
}

static esp_err_t history_controller_load_page(
    touch_history_controller_t *controller, const history_job_t *job,
    history_model_t *model)
{
    touch_history_day_t days[TOUCH_HISTORY_UI_LIST_ROWS] = {0};
    touch_history_index_page_t page = {0};
    history_operation_context_t operation_context;
    touch_history_operation_t operation = history_controller_operation(
        &operation_context, controller, job->generation, 0, 0, false);
    operation.progress = NULL;
    esp_err_t result = touch_history_load_page_ex(
        job->page_offset, days, TOUCH_HISTORY_UI_LIST_ROWS, &page,
        &operation);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "index page offset=%u failed: %s (0x%x)",
                 (unsigned)job->page_offset, esp_err_to_name(result),
                 (unsigned)result);
        return result;
    }
    memcpy(model->days, days, sizeof(days));
    model->page = page;
    history_controller_remember_newest_month(model);
    model->selected_row = SIZE_MAX;
    if (model->has_night)
        history_controller_update_selection(
            model, model->night.day, model->selected_global_index);
    return ESP_OK;
}

static void history_controller_month_from_day(const char day[9],
                                              uint16_t *year, uint8_t *month)
{
    if (!day || strlen(day) != 8) return;
    *year = (uint16_t)((day[0] - '0') * 1000 + (day[1] - '0') * 100 +
                       (day[2] - '0') * 10 + day[3] - '0');
    *month = (uint8_t)((day[4] - '0') * 10 + day[5] - '0');
}

static void history_controller_preview_day(size_t index,
                                           touch_history_day_t *day)
{
    memset(day, 0, sizeof(*day));
    if (index >= HISTORY_PREVIEW_DAY_COUNT) return;
    strlcpy(day->day, s_history_preview_days[index], sizeof(day->day));
    day->sessions = index == 1 || index == 5 ? 2 : 1;
    day->mask_off_count = (int)(index % 4U);
    day->usage_min = 432 - (int)(index % 5U) * 13;
    day->device_ahi = 1.7f + (float)(index % 4U) * 0.2f;
    day->st_ahi = 1.5f + (float)(index % 3U) * 0.2f;
    day->ahi = day->device_ahi;
    day->pressure_p95 = 10.4f + (float)(index % 3U) * 0.2f;
    day->leak_p95 = 7.8f + (float)(index % 4U) * 0.5f;
    day->has_summary = true;
    day->has_mask_off_count = true;
    day->has_usage = true;
    day->has_ahi = true;
    day->has_device_ahi = true;
    day->has_st_ahi = true;
    day->has_pressure_p95 = true;
    day->has_leak_p95 = true;
    day->has_therapy = true;
    day->has_oximetry = true;
}

static esp_err_t history_controller_preview_page(
    const history_job_t *job, history_model_t *model)
{
    memset(model->days, 0, sizeof(model->days));
    size_t offset = job->page_offset;
    size_t available = offset < HISTORY_PREVIEW_DAY_COUNT
        ? HISTORY_PREVIEW_DAY_COUNT - offset : 0;
    size_t returned = available < TOUCH_HISTORY_UI_LIST_ROWS
        ? available : TOUCH_HISTORY_UI_LIST_ROWS;
    for (size_t i = 0; i < returned; ++i)
        history_controller_preview_day(offset + i, &model->days[i]);
    model->page = (touch_history_index_page_t) {
        .offset = offset,
        .returned = returned,
        .total_days = HISTORY_PREVIEW_DAY_COUNT,
        .has_more = offset + returned < HISTORY_PREVIEW_DAY_COUNT,
    };
    history_controller_remember_newest_month(model);
    model->selected_row = SIZE_MAX;
    if (model->has_night)
        history_controller_update_selection(
            model, model->night.day, model->selected_global_index);
    return returned ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static size_t history_controller_preview_index(const char day[9])
{
    for (size_t i = 0; i < HISTORY_PREVIEW_DAY_COUNT; ++i) {
        if (!strcmp(day, s_history_preview_days[i])) return i;
    }
    return SIZE_MAX;
}

static void history_controller_preview_month(uint16_t year, uint8_t month,
                                             touch_history_month_t *result)
{
    memset(result, 0, sizeof(*result));
    result->year = year;
    result->month = month;
    result->days_in_month = month == 9 ? 30 : 31;
    for (size_t i = 0; i < HISTORY_PREVIEW_DAY_COUNT; ++i) {
        const char *day = s_history_preview_days[i];
        unsigned candidate_year = (unsigned)(day[0] - '0') * 1000U +
            (unsigned)(day[1] - '0') * 100U +
            (unsigned)(day[2] - '0') * 10U + (unsigned)(day[3] - '0');
        unsigned candidate_month = (unsigned)(day[4] - '0') * 10U +
            (unsigned)(day[5] - '0');
        unsigned date = (unsigned)(day[6] - '0') * 10U +
            (unsigned)(day[7] - '0');
        if (candidate_year != year || candidate_month != month || !date)
            continue;
        result->therapy_days |= 1U << (date - 1U);
        result->oximetry_days |= 1U << (date - 1U);
        result->therapy_night_count++;
        result->oximetry_night_count++;
    }
}

static bool history_controller_preview_therapy(
    const history_model_t *model, int64_t timestamp_ms)
{
    for (size_t i = 0; i < model->session_count; ++i) {
        if (timestamp_ms >= model->sessions[i].start_ms &&
            timestamp_ms < model->sessions[i].end_ms) return true;
    }
    return false;
}

static void history_controller_preview_stats(history_model_t *model)
{
    touch_history_stats_t *stats = &model->stats;
    memset(stats, 0, sizeof(*stats));
    stats->signal = model->signal;
    stats->start_ms = model->window_start_ms;
    stats->end_ms = model->window_end_ms;
    stats->therapy_only = model->therapy_only;
    stats->source_raw = model->overview.source_raw;
    stats->loaded = true;
    stats->exact = model->signal != TOUCH_HISTORY_SIGNAL_MOTION;
    stats->sample_count = stats->exact ? 36000U : 0;
    static const touch_history_stat_kind_t percentile_kinds[] = {
        TOUCH_HISTORY_STAT_P50,
        TOUCH_HISTORY_STAT_P95,
        TOUCH_HISTORY_STAT_P995,
    };
    static const int32_t percentile_values[TOUCH_HISTORY_SIGNAL_COUNT][3] = {
        {35, 63, 69}, {820, 1040, 1160}, {240, 780, 1420},
        {5, 24, 66}, {0, 18, 72}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    };
    if (model->signal == TOUCH_HISTORY_SIGNAL_FLOW) {
        stats->value_count = 3;
        stats->values[0].kind = TOUCH_HISTORY_STAT_ABSOLUTE_P50;
        stats->values[1].kind = TOUCH_HISTORY_STAT_ABSOLUTE_P95;
        stats->values[2].kind = TOUCH_HISTORY_STAT_ABSOLUTE_P995;
    } else if (model->signal >= TOUCH_HISTORY_SIGNAL_PRESSURE &&
               model->signal <= TOUCH_HISTORY_SIGNAL_SNORE) {
        stats->value_count = 3;
        for (size_t i = 0; i < 3; ++i)
            stats->values[i].kind = percentile_kinds[i];
    } else if (model->signal == TOUCH_HISTORY_SIGNAL_SPO2) {
        stats->value_count = 4;
        stats->values[0].kind = TOUCH_HISTORY_STAT_MINIMUM;
        stats->values[0].value_x100 = 9100;
        stats->values[1].kind = TOUCH_HISTORY_STAT_P5;
        stats->values[1].value_x100 = 9500;
        stats->values[2].kind = TOUCH_HISTORY_STAT_P05;
        stats->values[2].value_x100 = 9250;
        stats->values[3].kind = TOUCH_HISTORY_STAT_TIME_BELOW_88;
        stats->values[3].value_x100 = 250;
    } else if (model->signal == TOUCH_HISTORY_SIGNAL_PULSE) {
        stats->value_count = 3;
        stats->values[0].kind = TOUCH_HISTORY_STAT_MINIMUM;
        stats->values[0].value_x100 = 5200;
        stats->values[1].kind = TOUCH_HISTORY_STAT_MEDIAN;
        stats->values[1].value_x100 = 6400;
        stats->values[2].kind = TOUCH_HISTORY_STAT_MAXIMUM;
        stats->values[2].value_x100 = 7900;
    }
    for (size_t i = 0; i < stats->value_count; ++i) {
        if (model->signal <= TOUCH_HISTORY_SIGNAL_SNORE)
            stats->values[i].value_x100 =
                percentile_values[model->signal][i];
        stats->values[i].available = true;
    }
}

static void history_controller_preview_graph(history_model_t *model)
{
    model->details_pending = false;
    touch_history_overview_t *overview = &model->overview;
    memset(overview, 0, sizeof(*overview));
    int64_t span = model->window_end_ms - model->window_start_ms;
    uint16_t count = touch_history_range_point_count(
        model->signal, (uint64_t)span);
    if (count < 2) count = 2;
    overview->point_count = count;
    overview->axis_start_ms = model->window_start_ms;
    overview->axis_end_ms = model->window_end_ms;
    overview->bin_width_ms = (uint32_t)((span + count - 1) / count);
    overview->signal = model->signal;
    overview->aggregation = model->signal == TOUCH_HISTORY_SIGNAL_FLOW
        ? TOUCH_HISTORY_AGGREGATION_ENVELOPE : TOUCH_HISTORY_AGGREGATION_MEAN;
    overview->source_raw = model->signal == TOUCH_HISTORY_SIGNAL_FLOW &&
        touch_history_flow_range_prefers_raw(
            (uint64_t)span,
            (uint64_t)(model->night.axis_end_ms - model->night.axis_start_ms));
    overview->has_companion = model->signal == TOUCH_HISTORY_SIGNAL_PRESSURE;
    overview->therapy_coverage_per_mille = 946;
    overview->has_therapy_coverage =
        model->signal == TOUCH_HISTORY_SIGNAL_SPO2;
    for (uint16_t i = 0; i < count; ++i) {
        int64_t timestamp = model->window_start_ms +
            (int64_t)((uint64_t)span * (2U * i + 1U) / (2U * count));
        overview->timestamp_ms[i] = timestamp;
        bool therapy = history_controller_preview_therapy(model, timestamp);
        bool valid = model->signal >= TOUCH_HISTORY_SIGNAL_SPO2
            ? (!model->therapy_only || therapy) : therapy;
        if (model->signal == TOUCH_HISTORY_SIGNAL_SPO2 &&
            i > count / 3U && i < count / 3U + count / 24U) valid = false;
        if (therapy) overview->flags[i] |= TOUCH_HISTORY_POINT_THERAPY;
        if (!valid) continue;
        int phase = i % 48U;
        int breath = phase < 24 ? phase - 12 : 36 - phase;
        int slow = (int)(i % 91U) - 45;
        int32_t value = 0;
        switch (model->signal) {
        case TOUCH_HISTORY_SIGNAL_FLOW:
            value = -(32 + (breath < 0 ? -breath : breath));
            overview->upper_x100[i] =
                (int16_t)(36 + (breath < 0 ? -breath : breath));
            overview->flags[i] |= TOUCH_HISTORY_POINT_UPPER_VALID;
            break;
        case TOUCH_HISTORY_SIGNAL_PRESSURE:
            value = 980 + slow * 2;
            overview->companion_x100[i] = (int16_t)(value - 210);
            overview->flags[i] |= TOUCH_HISTORY_POINT_COMPANION_VALID;
            break;
        case TOUCH_HISTORY_SIGNAL_LEAK: value = 620 + slow * 8; break;
        case TOUCH_HISTORY_SIGNAL_FLOW_LIMIT: value = 18 + (i % 17U); break;
        case TOUCH_HISTORY_SIGNAL_SNORE: value = i % 37U < 4U ? 68 : 4; break;
        case TOUCH_HISTORY_SIGNAL_SPO2: value = 9700 - (i % 29U) * 8; break;
        case TOUCH_HISTORY_SIGNAL_PULSE: value = 6200 + slow * 12; break;
        case TOUCH_HISTORY_SIGNAL_MOTION: value = i % 41U < 3U ? 220 : 10; break;
        case TOUCH_HISTORY_SIGNAL_COUNT: break;
        }
        overview->value_x100[i] = (int16_t)value;
        overview->sample_count[i] = 1;
        overview->flags[i] |= TOUCH_HISTORY_POINT_VALID;
        overview->valid_sample_count++;
    }
    overview->source_sample_count = count;
    overview->contributing_sessions = 2;
    overview->has_data = overview->valid_sample_count > 1;
    overview->loaded = true;
}

static void history_controller_preview_events(history_model_t *model)
{
    static const touch_history_event_type_t types[] = {
        TOUCH_HISTORY_EVENT_OBSTRUCTIVE_APNEA,
        TOUCH_HISTORY_EVENT_HYPOPNEA,
        TOUCH_HISTORY_EVENT_CENTRAL_APNEA,
        TOUCH_HISTORY_EVENT_GENERIC_APNEA,
        TOUCH_HISTORY_EVENT_RERA,
    };
    model->event_count = 0;
    model->event_total_count = sizeof(types) / sizeof(types[0]);
    model->events_truncated = false;
    model->event_state = TOUCH_HISTORY_UI_EVENT_STATE_COMPLETE;
    int64_t span = model->night.axis_end_ms - model->night.axis_start_ms;
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        int64_t end = model->night.axis_start_ms +
            span * (int64_t)(i + 1U) / 6;
        if (end < model->window_start_ms || end >= model->window_end_ms)
            continue;
        model->events[model->event_count++] = (touch_history_event_t) {
            .type = types[i],
            .start_ms = end - 12000,
            .end_ms = end,
            .session_index = end < model->sessions[0].end_ms ? 0 : 1,
            .time_corrected = true,
        };
    }
}

static esp_err_t history_controller_preview_view(
    touch_history_controller_t *controller, const history_job_t *job,
    history_model_t *model, bool load_night)
{
    size_t index = history_controller_preview_index(job->day);
    if (index == SIZE_MAX) return ESP_ERR_NOT_FOUND;
    if (load_night) {
        memset(&model->night, 0, sizeof(model->night));
        strlcpy(model->night.day, job->day, sizeof(model->night.day));
        int64_t start = 1788318240000LL - (int64_t)index * 86400000LL;
        model->night.axis_start_ms = start;
        model->night.axis_end_ms = start + 452LL * 60000LL;
        model->night.session_count = 2;
        model->night.sessions_returned = 2;
        model->night.available_signals = 0xffU;
        model->night.o2_coverage_per_mille = 946;
        model->night.device_ahi = 1.7f;
        model->night.st_ahi = 1.5f;
        model->night.has_device_ahi = true;
        model->night.has_st_ahi = true;
        model->night.has_o2_coverage = true;
        model->night.event_totals.complete = true;
        model->night.event_totals.has_indices = true;
        model->night.event_totals.eligible_therapy_ms = 432ULL * 60000ULL;
        model->night.event_totals.ahi = 1.5f;
        model->sessions[0] = (touch_history_session_t) {
            .start_ms = start,
            .end_ms = start + 196LL * 60000LL,
            .available_signals = 0xffU,
            .has_epr_companion = true,
        };
        strlcpy(model->sessions[0].id, "preview-session-1",
                sizeof(model->sessions[0].id));
        model->sessions[1] = (touch_history_session_t) {
            .start_ms = start + 216LL * 60000LL,
            .end_ms = model->night.axis_end_ms,
            .available_signals = 0xffU,
            .has_epr_companion = true,
        };
        strlcpy(model->sessions[1].id, "preview-session-2",
                sizeof(model->sessions[1].id));
        model->session_count = 2;
        model->has_night = true;
        model->signal = job->signal;
        model->therapy_only = job->therapy_only &&
                              job->signal == TOUCH_HISTORY_SIGNAL_SPO2;
        model->window_start_ms = job->window_start_ms > 0
            ? job->window_start_ms : model->night.axis_start_ms;
        model->window_end_ms = job->window_end_ms > model->window_start_ms
            ? job->window_end_ms : model->night.axis_end_ms;
        model->cursor_ms = 0;
        model->cursor_valid = false;
        history_controller_update_selection(model, job->day, index);
    } else {
        model->signal = job->signal;
        model->therapy_only = job->therapy_only &&
                              job->signal == TOUCH_HISTORY_SIGNAL_SPO2;
        model->window_start_ms = job->window_start_ms;
        model->window_end_ms = job->window_end_ms;
    }
    history_controller_preview_graph(model);
    history_controller_preview_stats(model);
    history_controller_preview_events(model);
    model->has_overview = true;
    model->state = TOUCH_HISTORY_UI_STATE_READY;
    model->progress_per_mille = 1000;
    model->status[0] = '\0';
    model->error[0] = '\0';
    model->degraded[0] = '\0';
    const touch_history_day_t *selected =
        model->selected_row < model->page.returned
            ? &model->days[model->selected_row] : NULL;
    model->usage_target_known = controller->config.usage_target_minutes > 0;
    model->usage_on_target = selected && selected->has_usage &&
        selected->usage_min >= controller->config.usage_target_minutes;
    return ESP_OK;
}

static esp_err_t history_controller_preview_process(
    touch_history_controller_t *controller, const history_job_t *job,
    history_model_t *result)
{
    if (job->kind == HISTORY_JOB_INITIAL) {
        memset(result, 0, sizeof(*result));
        result->selected_row = SIZE_MAX;
        result->selected_global_index = SIZE_MAX;
        result->signal = TOUCH_HISTORY_SIGNAL_FLOW;
        esp_err_t page = history_controller_preview_page(job, result);
        if (page != ESP_OK) return page;
        history_job_t detail = *job;
        detail.kind = HISTORY_JOB_DAY;
        detail.global_index = 0;
        strlcpy(detail.day, s_history_preview_days[0], sizeof(detail.day));
        esp_err_t view = history_controller_preview_view(
            controller, &detail, result, true);
        if (view != ESP_OK) return view;
        history_controller_preview_month(2026, 9, &result->month);
        result->has_month = true;
        return ESP_OK;
    }
    if (job->kind == HISTORY_JOB_PAGE) {
        esp_err_t page = history_controller_preview_page(job, result);
        if (page != ESP_OK) return page;
        if (job->global_index == SIZE_MAX) {
            result->state = result->has_night ? TOUCH_HISTORY_UI_STATE_READY
                                              : TOUCH_HISTORY_UI_STATE_EMPTY;
            return ESP_OK;
        }
        if (job->global_index >= HISTORY_PREVIEW_DAY_COUNT)
            return ESP_ERR_NOT_FOUND;
        history_job_t detail = *job;
        strlcpy(detail.day, s_history_preview_days[job->global_index],
                sizeof(detail.day));
        return history_controller_preview_view(
            controller, &detail, result, true);
    }
    if (job->kind == HISTORY_JOB_DAY) {
        size_t index = job->global_index == SIZE_MAX
            ? history_controller_preview_index(job->day) : job->global_index;
        if (index >= HISTORY_PREVIEW_DAY_COUNT) return ESP_ERR_NOT_FOUND;
        history_job_t detail = *job;
        detail.global_index = index;
        detail.page_offset = (index / TOUCH_HISTORY_UI_LIST_ROWS) *
                             TOUCH_HISTORY_UI_LIST_ROWS;
        if (detail.page_offset != result->page.offset) {
            esp_err_t page = history_controller_preview_page(&detail, result);
            if (page != ESP_OK) return page;
        }
        return history_controller_preview_view(
            controller, &detail, result, true);
    }
    if (job->kind == HISTORY_JOB_VIEW)
        return history_controller_preview_view(
            controller, job, result, false);
    if (job->kind == HISTORY_JOB_MONTH) {
        history_controller_preview_month(job->year, job->month, &result->month);
        result->has_month = true;
        result->calendar_read_error = false;
        return ESP_OK;
    }
    return ESP_OK;
}

static esp_err_t history_controller_process(
    touch_history_controller_t *controller, const history_job_t *job,
    history_model_t *result)
{
    if (!history_controller_copy_model(controller, result))
        return ESP_FAIL;
    if (controller->config.deterministic_preview)
        return history_controller_preview_process(controller, job, result);
    switch (job->kind) {
    case HISTORY_JOB_INITIAL: {
        history_cache_clear();
        memset(result, 0, sizeof(*result));
        result->selected_row = SIZE_MAX;
        result->selected_global_index = SIZE_MAX;
        result->signal = job->signal;
        esp_err_t load = history_controller_load_page(
            controller, job, result);
        if (load == ESP_ERR_NOT_FOUND) {
            result->state = TOUCH_HISTORY_UI_STATE_EMPTY;
            history_controller_text(result->status, sizeof(result->status),
                                    "No recorded nights were found.");
            return ESP_OK;
        }
        if (load != ESP_OK) return load;
        if (!result->page.returned) return ESP_ERR_NOT_FOUND;
        history_job_t detail = *job;
        detail.kind = HISTORY_JOB_DAY;
        detail.global_index = 0;
        memcpy(detail.day, result->days[0].day, sizeof(detail.day));
        load = history_controller_load_view(controller, &detail, result, true);
        if (load != ESP_OK) return load;
        uint16_t year = 0;
        uint8_t month = 0;
        history_controller_month_from_day(detail.day, &year, &month);
        if (year && month) {
            history_operation_context_t operation_context;
            touch_history_operation_t operation = history_controller_operation(
                &operation_context, controller, job->generation, 0, 0, false);
            operation.progress = NULL;
            esp_err_t month_result = touch_history_load_month_ex(
                year, month, &result->month, &operation);
            if (month_result == ESP_OK) {
                result->has_month = true;
                result->calendar_read_error = false;
            } else if (month_result == TOUCH_HISTORY_ERR_CANCELLED &&
                       !history_controller_generation_current(
                           controller, job->generation, false)) {
                /* A newer intent/deactivation owns the model now. */
                return month_result;
            } else {
                /* The night detail is still complete and useful. Scope a
                 * failed or recording-preempted compact index read to the
                 * Calendar rail. */
                result->has_month = false;
                result->calendar_read_error = true;
            }
        }
        return ESP_OK;
    }
    case HISTORY_JOB_PAGE: {
        esp_err_t page_result = history_controller_load_page(
            controller, job, result);
        if (page_result != ESP_OK) return page_result;
        if (job->global_index != SIZE_MAX &&
            job->global_index >= result->page.offset &&
            job->global_index - result->page.offset < result->page.returned) {
            history_job_t detail = *job;
            detail.kind = HISTORY_JOB_DAY;
            size_t row = job->global_index - result->page.offset;
            memcpy(detail.day, result->days[row].day, sizeof(detail.day));
            return history_controller_load_view(
                controller, &detail, result, true);
        }
        result->state = result->has_night ? TOUCH_HISTORY_UI_STATE_READY
                                         : TOUCH_HISTORY_UI_STATE_EMPTY;
        result->status[0] = '\0';
        return ESP_OK;
    }
    case HISTORY_JOB_DAY: {
        history_job_t resolved = *job;
        if (resolved.global_index == SIZE_MAX) {
            size_t total_days = 0;
            history_operation_context_t operation_context;
            touch_history_operation_t operation = history_controller_operation(
                &operation_context, controller, job->generation, 0, 0, false);
            operation.progress = NULL;
            esp_err_t found = touch_history_find_day_index_ex(
                resolved.day, &resolved.global_index, &total_days, &operation);
            if (found != ESP_OK) return found;
            resolved.page_offset =
                (resolved.global_index / TOUCH_HISTORY_UI_LIST_ROWS) *
                TOUCH_HISTORY_UI_LIST_ROWS;
            result->page.total_days = total_days;
        }
        if (resolved.page_offset != result->page.offset) {
            esp_err_t page = history_controller_load_page(
                controller, &resolved, result);
            if (page != ESP_OK) return page;
        }
        return history_controller_load_view(
            controller, &resolved, result, true);
    }
    case HISTORY_JOB_VIEW:
        return history_controller_load_view(controller, job, result, false);
    case HISTORY_JOB_MONTH: {
        history_operation_context_t operation_context;
        touch_history_operation_t operation = history_controller_operation(
            &operation_context, controller, job->generation, 0, 0, true);
        operation.progress = NULL;
        esp_err_t month = touch_history_load_month_ex(
            job->year, job->month, &result->month, &operation);
        if (month != ESP_OK) return month;
        result->has_month = true;
        result->calendar_read_error = false;
        result->status[0] = '\0';
        return ESP_OK;
    }
    case HISTORY_JOB_STOP:
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static bool history_controller_take_job(
    touch_history_controller_t *controller, history_job_t *job)
{
    bool found = false;
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
        /* Producers use this same lifecycle lock, so a foreground job cannot
         * arrive between the priority check and taking coalesced month work. */
        if (xQueueReceive(controller->queue, job, 0) == pdTRUE) {
            found = true;
        } else if (controller->month_pending) {
            *job = controller->pending_month;
            controller->month_pending = false;
            controller->active_month = *job;
            controller->month_running = true;
            found = true;
        }
        xSemaphoreGive(controller->mutex);
    }
    return found;
}

static void history_controller_finish_job(
    touch_history_controller_t *controller, const history_job_t *job)
{
    if (!controller || !job || job->kind != HISTORY_JOB_MONTH)
        return;
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
        if (controller->month_running &&
            controller->active_month.generation == job->generation)
            controller->month_running = false;
        xSemaphoreGive(controller->mutex);
    }
}

static void history_controller_worker(void *argument)
{
    touch_history_controller_t *controller = argument;
    history_job_t job;
    bool stopping = false;
    while (!stopping) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (history_controller_take_job(controller, &job)) {
            if (job.kind == HISTORY_JOB_STOP) {
                stopping = true;
                break;
            }
            if (!history_controller_job_current(controller, &job)) {
                ESP_LOGI(TAG, "drop stale job=%u generation=%u",
                         (unsigned)job.kind, (unsigned)job.generation);
                history_controller_finish_job(controller, &job);
                continue;
            }
            ESP_LOGI(TAG, "begin job=%u generation=%u day=%s signal=%u",
                     (unsigned)job.kind, (unsigned)job.generation,
                     job.day[0] ? job.day : "-", (unsigned)job.signal);
            history_controller_begin_job(controller, &job);
            history_model_t *result = heap_caps_malloc(
                sizeof(*result), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!result) {
                ESP_LOGE(
                    TAG,
                    "job=%u generation=%u could not allocate %u-byte result model",
                    (unsigned)job.kind, (unsigned)job.generation,
                    (unsigned)sizeof(*result));
                history_controller_publish_error(
                    controller, &job, ESP_ERR_NO_MEM);
                history_controller_finish_job(controller, &job);
                continue;
            }
            esp_err_t status = history_controller_process(
                controller, &job, result);
            if (status == ESP_OK) {
                bool published = history_controller_publish(controller, &job, result);
                if (published && result->has_night && !controller->config.deterministic_preview &&
                    job.kind != HISTORY_JOB_MONTH) {
                    history_operation_context_t prepare_context;
                    touch_history_operation_t prepare = history_controller_operation(
                        &prepare_context, controller, job.generation, 0, 0, false);
                    prepare.progress = NULL;
                    (void)touch_history_prepare_day_ex(result->night.day, &prepare);
                }
            } else if (history_controller_job_current(controller, &job)) {
                /* Cancellation can mean either a superseding UI generation
                 * or recording-priority preemption. The former is stale by
                 * the check above; the latter is still current and must leave
                 * a retryable state instead of a permanent loading surface. */
                esp_err_t visible_status =
                    status == TOUCH_HISTORY_ERR_CANCELLED
                        ? ESP_ERR_INVALID_STATE : status;
                ESP_LOGE(
                    TAG,
                    "job=%u generation=%u day=%s signal=%u failed: %s (0x%x)",
                    (unsigned)job.kind, (unsigned)job.generation,
                    job.day[0] ? job.day : "-", (unsigned)job.signal,
                    esp_err_to_name(visible_status),
                    (unsigned)visible_status);
                history_controller_publish_error(
                    controller, &job, visible_status);
            }
            heap_caps_free(result);
            history_controller_finish_job(controller, &job);
        }
    }
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
        controller->worker = NULL;
        xSemaphoreGive(controller->mutex);
    }
    psram_task_delete(NULL);
}

/* Caller owns controller->mutex. Keeping lifecycle validation, generation
 * assignment, mailbox publication, and worker wake in one critical section
 * prevents refresh/activation and destroy from racing another producer. */
static esp_err_t history_controller_enqueue_locked(
    touch_history_controller_t *controller, history_job_t *job,
    bool *calendar_state_changed)
{
    if (!controller || !job) return ESP_ERR_INVALID_ARG;
    if (calendar_state_changed)
        *calendar_state_changed = false;
    if (controller->closing || !controller->active)
        return ESP_ERR_INVALID_STATE;
    TaskHandle_t worker = controller->worker;
    if (!worker)
        return ESP_ERR_INVALID_STATE;
    bool calendar_job = job->kind == HISTORY_JOB_MONTH;
    bool calendar_changed = false;
    BaseType_t queued = pdPASS;
    if (calendar_job) {
        controller->calendar_generation++;
        if (!controller->calendar_generation)
            controller->calendar_generation = 1;
        job->generation = controller->calendar_generation;
        controller->pending_month = *job;
        controller->month_pending = true;
        calendar_changed = !controller->model.calendar_loading ||
                           controller->model.calendar_read_error;
        controller->model.calendar_loading = true;
        controller->model.calendar_read_error = false;
        if (calendar_changed)
            controller->revision++;
    } else {
        /* A foreground gesture preempts an in-flight all-history month scan.
         * Re-pend the same target under a fresh calendar generation so it
         * resumes after the foreground mailbox drains instead of being lost. */
        if (controller->month_running &&
            controller->model.calendar_loading &&
            controller->model.rail_mode == TOUCH_HISTORY_UI_RAIL_CALENDAR) {
            controller->calendar_generation++;
            if (!controller->calendar_generation)
                controller->calendar_generation = 1;
            history_job_t resume = controller->month_pending
                ? controller->pending_month : controller->active_month;
            controller->pending_month = resume;
            controller->pending_month.generation =
                controller->calendar_generation;
            controller->month_pending = true;
        }
        controller->generation++;
        if (!controller->generation) controller->generation = 1;
        job->generation = controller->generation;
        controller->retry_job = *job;
        queued = xQueueOverwrite(controller->queue, job);
        if (queued == pdPASS && job->kind == HISTORY_JOB_VIEW) {
            history_model_t *live = &controller->model;
            if (controller->preview_source_generation != sd_storage_content_generation() ||
                live->therapy_only != job->therapy_only)
                controller->preview_source.loaded = false;
            touch_history_make_preview(&controller->preview_source, job->signal,
                job->window_start_ms, job->window_end_ms, &live->overview);
            live->has_overview = true;
            live->details_pending = true;
            live->signal = job->signal;
            live->therapy_only = job->therapy_only;
            live->window_start_ms = job->window_start_ms;
            live->window_end_ms = job->window_end_ms;
            live->state = TOUCH_HISTORY_UI_STATE_ZOOM_LOADING;
            memset(&live->stats, 0, sizeof(live->stats));
            live->event_count = live->event_total_count = 0;
            live->events_truncated = false;
            live->event_state = TOUCH_HISTORY_UI_EVENT_STATE_UNAVAILABLE;
            live->error[0] = live->degraded[0] = '\0';
            history_controller_text(live->status, sizeof(live->status), "Updating…");
            history_controller_text(live->stats_warning, sizeof(live->stats_warning), "");
            ++controller->revision;
            calendar_changed = true; /* same coalesced notification path */
        }
    }
    /* Queue publication and its wake edge stay inside the lifecycle lock.
     * Destroy cannot reset/delete the queue or retire this task handle between
     * the closing check and the nonblocking producer operation. */
    if (queued == pdPASS)
        xTaskNotifyGive(worker);
    if (calendar_state_changed)
        *calendar_state_changed = calendar_changed;
    return queued == pdPASS ? ESP_OK : ESP_FAIL;
}

static esp_err_t history_controller_enqueue(
    touch_history_controller_t *controller, history_job_t *job)
{
    if (!controller || !job) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE)
        return ESP_FAIL;
    bool calendar_state_changed = false;
    esp_err_t result = history_controller_enqueue_locked(
        controller, job, &calendar_state_changed);
    touch_history_controller_changed_fn changed =
        calendar_state_changed ? controller->config.changed : NULL;
    void *changed_context = controller->config.context;
    xSemaphoreGive(controller->mutex);
    if (result == ESP_OK) {
        if (changed)
            changed(changed_context);
        ESP_LOGI(TAG, "queued job=%u generation=%u day=%s signal=%u",
                 (unsigned)job->kind, (unsigned)job->generation,
                 job->day[0] ? job->day : "-", (unsigned)job->signal);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "queue rejected job=%u generation=%u",
             (unsigned)job->kind, (unsigned)job->generation);
    return result;
}

esp_err_t touch_history_controller_create(
    const touch_history_controller_config_t *config,
    touch_history_controller_t **out_controller)
{
    if (!out_controller) return ESP_ERR_INVALID_ARG;
    *out_controller = NULL;
    touch_history_controller_t *controller = heap_caps_calloc(
        1, sizeof(*controller), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!controller) return ESP_ERR_NO_MEM;
    if (config) controller->config = *config;
    controller->model.selected_row = SIZE_MAX;
    controller->model.selected_global_index = SIZE_MAX;
    controller->model.signal = TOUCH_HISTORY_SIGNAL_FLOW;
    controller->model.state = TOUCH_HISTORY_UI_STATE_EMPTY;
    /* Keep the small FreeRTOS kernel objects in cache-safe internal RAM. The
     * retained History model and worker stack remain the large PSRAM users. */
    controller->mutex = xSemaphoreCreateMutex();
    controller->queue = xQueueCreate(
        HISTORY_CONTROLLER_QUEUE_LENGTH, sizeof(history_job_t));
    if (!controller->mutex || !controller->queue) {
        if (controller->queue) vQueueDelete(controller->queue);
        if (controller->mutex) vSemaphoreDelete(controller->mutex);
        heap_caps_free(controller);
        return ESP_ERR_NO_MEM;
    }
    controller->worker = psram_task_create(
        history_controller_worker, "ui_history", HISTORY_CONTROLLER_WORKER_STACK,
        controller, 3, 0, NULL, NULL);
    if (!controller->worker) {
        vQueueDelete(controller->queue);
        vSemaphoreDelete(controller->mutex);
        heap_caps_free(controller);
        return ESP_ERR_NO_MEM;
    }
    *out_controller = controller;
    return ESP_OK;
}

void touch_history_controller_destroy(touch_history_controller_t *controller)
{
    if (!controller) return;
    history_job_t stop = {.kind = HISTORY_JOB_STOP};
    TaskHandle_t worker_to_notify = NULL;
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
        controller->closing = true;
        controller->active = false;
        controller->generation++;
        controller->calendar_generation++;
        controller->month_pending = false;
        controller->month_running = false;
        worker_to_notify = controller->worker;
        xSemaphoreGive(controller->mutex);
    }
    xQueueReset(controller->queue);
    (void)xQueueSend(controller->queue, &stop, portMAX_DELAY);
    if (worker_to_notify)
        xTaskNotifyGive(worker_to_notify);
    for (;;) {
        TaskHandle_t worker = NULL;
        if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
            worker = controller->worker;
            xSemaphoreGive(controller->mutex);
        }
        if (!worker) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vQueueDelete(controller->queue);
    vSemaphoreDelete(controller->mutex);
    heap_caps_free(controller);
}

esp_err_t touch_history_controller_set_active(
    touch_history_controller_t *controller, bool active)
{
    if (!controller) return ESP_ERR_INVALID_ARG;
    bool load = false;
    bool notify = false;
    esp_err_t result = ESP_OK;
    history_job_t job = {
        .kind = HISTORY_JOB_INITIAL,
        .page_offset = 0,
        .global_index = 0,
        .signal = TOUCH_HISTORY_SIGNAL_FLOW,
    };
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE)
        return ESP_FAIL;
    if (controller->closing) {
        xSemaphoreGive(controller->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    controller->active = active;
    if (!active) {
        controller->generation++;
        controller->calendar_generation++;
        controller->month_pending = false;
        if (controller->model.calendar_loading) {
            controller->model.calendar_loading = false;
            controller->revision++;
            notify = true;
        }
        controller->model.progress_per_mille = 0;
        if (controller->model.state == TOUCH_HISTORY_UI_STATE_AUTO_LOADING ||
            controller->model.state == TOUCH_HISTORY_UI_STATE_ZOOM_LOADING) {
            controller->model.state = controller->model.has_night
                ? TOUCH_HISTORY_UI_STATE_READY
                : TOUCH_HISTORY_UI_STATE_EMPTY;
            controller->model.status[0] = '\0';
            controller->revision++;
            notify = true;
        }
    } else if (controller->model.details_pending && controller->model.has_night) {
        /* Leaving during graph/stat IO cancels the generation. Resume that
         * requested window when returning, instead of stranding a preview. */
        job = controller->retry_job;
        load = true;
        result = history_controller_enqueue_locked(controller, &job, NULL);
    } else if (!controller->ever_loaded ||
               (controller->model.source_generation &&
                controller->model.source_generation != sd_storage_content_generation()) ||
               controller->model.state == TOUCH_HISTORY_UI_STATE_EMPTY ||
               controller->model.state == TOUCH_HISTORY_UI_STATE_READ_ERROR) {
        load = true;
        result = history_controller_enqueue_locked(controller, &job, NULL);
    }
    ESP_LOGI(TAG, "set active=%u load=%u generation=%u state=%u ever=%u",
             active ? 1U : 0U, load ? 1U : 0U,
             (unsigned)controller->generation,
             (unsigned)controller->model.state,
             controller->ever_loaded ? 1U : 0U);
    xSemaphoreGive(controller->mutex);
    if (notify) history_controller_notify(controller);
    if (load && result == ESP_OK)
        ESP_LOGI(TAG, "queued activation job=%u generation=%u",
                 (unsigned)job.kind, (unsigned)job.generation);
    return result;
}

esp_err_t touch_history_controller_refresh(
    touch_history_controller_t *controller)
{
    if (!controller) return ESP_ERR_INVALID_ARG;
    bool notify = false;
    esp_err_t result = ESP_OK;
    history_job_t job = {
        .kind = HISTORY_JOB_INITIAL,
        .page_offset = 0,
        .global_index = 0,
        .signal = TOUCH_HISTORY_SIGNAL_FLOW,
    };
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE)
        return ESP_FAIL;
    if (controller->closing) {
        xSemaphoreGive(controller->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    controller->ever_loaded = false;
    controller->generation++;
    controller->calendar_generation++;
    controller->month_pending = false;
    if (controller->model.calendar_loading ||
        controller->model.calendar_read_error) {
        controller->model.calendar_loading = false;
        controller->model.calendar_read_error = false;
        controller->revision++;
        notify = true;
    }
    if (controller->active)
        result = history_controller_enqueue_locked(controller, &job, NULL);
    xSemaphoreGive(controller->mutex);
    if (notify) history_controller_notify(controller);
    if (result == ESP_OK && job.generation)
        ESP_LOGI(TAG, "queued refresh job=%u generation=%u",
                 (unsigned)job.kind, (unsigned)job.generation);
    return result;
}

static bool history_controller_get_navigation(
    touch_history_controller_t *controller, history_navigation_t *navigation)
{
    if (!controller || !navigation ||
        xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE)
        return false;
    memcpy(navigation->days, controller->model.days,
           sizeof(navigation->days));
    navigation->page = controller->model.page;
    navigation->night = controller->model.night;
    navigation->month = controller->model.month;
    navigation->selected_global_index =
        controller->model.selected_global_index;
    navigation->signal = controller->model.signal;
    navigation->window_start_ms = controller->model.window_start_ms;
    navigation->window_end_ms = controller->model.window_end_ms;
    navigation->cursor_ms = controller->model.cursor_ms;
    navigation->has_night = controller->model.has_night;
    navigation->has_month = controller->model.has_month;
    navigation->cursor_valid = controller->model.cursor_valid;
    navigation->therapy_only = controller->model.therapy_only;
    navigation->rail_mode = controller->model.rail_mode;
    navigation->calendar_loading = controller->model.calendar_loading;
    xSemaphoreGive(controller->mutex);
    return true;
}

static void history_controller_queue_day(
    touch_history_controller_t *controller, const history_navigation_t *model,
    const char day[9], size_t global_index, size_t page_offset)
{
    history_job_t job = {
        .kind = HISTORY_JOB_DAY,
        .page_offset = page_offset,
        .global_index = global_index,
        .signal = model->signal,
        .therapy_only = model->therapy_only,
    };
    memcpy(job.day, day, sizeof(job.day));
    (void)history_controller_enqueue(controller, &job);
}

static int64_t history_controller_zoom_span(int64_t current,
                                           int64_t night_span,
                                           int direction)
{
    if (direction <= 0) {
        if (current >= night_span) return night_span;
        if (current < HISTORY_CONTROLLER_ZOOM_10_MIN_MS)
            return HISTORY_CONTROLLER_ZOOM_10_MIN_MS;
        if (current < HISTORY_CONTROLLER_ZOOM_22_MIN_MS)
            return HISTORY_CONTROLLER_ZOOM_22_MIN_MS;
        if (current < HISTORY_CONTROLLER_ZOOM_90_MIN_MS)
            return HISTORY_CONTROLLER_ZOOM_90_MIN_MS;
        int64_t quarter = night_span / 4;
        if (current < quarter) return quarter;
        return night_span;
    }
    int64_t quarter = night_span / 4;
    if (current > quarter && quarter > HISTORY_CONTROLLER_ZOOM_90_MIN_MS)
        return quarter;
    if (current > HISTORY_CONTROLLER_ZOOM_90_MIN_MS)
        return HISTORY_CONTROLLER_ZOOM_90_MIN_MS;
    if (current > HISTORY_CONTROLLER_ZOOM_22_MIN_MS)
        return HISTORY_CONTROLLER_ZOOM_22_MIN_MS;
    if (current > HISTORY_CONTROLLER_ZOOM_10_MIN_MS)
        return HISTORY_CONTROLLER_ZOOM_10_MIN_MS;
    return current > HISTORY_CONTROLLER_ZOOM_5_MIN_MS
        ? HISTORY_CONTROLLER_ZOOM_5_MIN_MS : current;
}

static void history_controller_clamp_window(const history_navigation_t *model,
                                            int64_t centre, int64_t span,
                                            int64_t *start, int64_t *end)
{
    int64_t night_span = model->night.axis_end_ms - model->night.axis_start_ms;
    if (span >= night_span) {
        *start = model->night.axis_start_ms;
        *end = model->night.axis_end_ms;
        return;
    }
    *start = centre - span / 2;
    *end = *start + span;
    if (*start < model->night.axis_start_ms) {
        *start = model->night.axis_start_ms;
        *end = *start + span;
    }
    if (*end > model->night.axis_end_ms) {
        *end = model->night.axis_end_ms;
        *start = *end - span;
    }
}

static void history_controller_queue_view(
    touch_history_controller_t *controller, const history_navigation_t *model,
    touch_history_signal_t signal, int64_t start_ms, int64_t end_ms,
    bool therapy_only, bool non_cancellable)
{
    history_job_t job = {
        .kind = HISTORY_JOB_VIEW,
        .signal = signal,
        .window_start_ms = start_ms,
        .window_end_ms = end_ms,
        .therapy_only = therapy_only,
        .non_cancellable = non_cancellable,
        .global_index = model->selected_global_index,
        .page_offset = model->page.offset,
    };
    memcpy(job.day, model->night.day, sizeof(job.day));
    (void)history_controller_enqueue(controller, &job);
}

void touch_history_controller_handle_intent(
    void *context, const touch_history_ui_intent_t *intent)
{
    touch_history_controller_t *controller = context;
    if (!controller || !intent) return;
    if (intent->type == TOUCH_HISTORY_UI_INTENT_OPEN_CARD) {
        if (controller->config.route_card)
            controller->config.route_card(controller->config.context);
        return;
    }
    if (intent->type == TOUCH_HISTORY_UI_INTENT_CANCEL_AUTO_LOAD) {
        bool notify = false;
        if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
            if (controller->model.state == TOUCH_HISTORY_UI_STATE_AUTO_LOADING) {
                controller->generation++;
                controller->model.details_pending = false;
                controller->model.state = controller->model.has_night
                    ? TOUCH_HISTORY_UI_STATE_READY
                    : TOUCH_HISTORY_UI_STATE_EMPTY;
                controller->model.progress_per_mille = 0;
                controller->model.status[0] = '\0';
                controller->revision++;
                notify = true;
            }
            xSemaphoreGive(controller->mutex);
        }
        if (notify) history_controller_notify(controller);
        return;
    }

    history_navigation_t model;
    if (!history_controller_get_navigation(controller, &model)) return;
    switch (intent->type) {
    case TOUCH_HISTORY_UI_INTENT_SELECT_DAY:
        if (intent->row_index < model.page.returned)
            history_controller_queue_day(
                controller, &model, model.days[intent->row_index].day,
                model.page.offset + intent->row_index, model.page.offset);
        break;
    case TOUCH_HISTORY_UI_INTENT_PAGE_RELATIVE: {
        int64_t offset = (int64_t)model.page.offset +
            intent->relative * TOUCH_HISTORY_UI_LIST_ROWS;
        if (offset < 0) offset = 0;
        if ((size_t)offset >= model.page.total_days && model.page.total_days)
            offset = (int64_t)(((model.page.total_days - 1U) /
                      TOUCH_HISTORY_UI_LIST_ROWS) *
                      TOUCH_HISTORY_UI_LIST_ROWS);
        history_job_t job = {
            .kind = HISTORY_JOB_PAGE,
            .page_offset = (size_t)offset,
            .signal = model.signal,
            .global_index = SIZE_MAX,
        };
        (void)history_controller_enqueue(controller, &job);
        break;
    }
    case TOUCH_HISTORY_UI_INTENT_OPEN_CALENDAR: {
        bool notify = false;
        bool queued = false;
        uint16_t year = 0;
        uint8_t month = 0;
        if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
            history_model_t *live = &controller->model;
            if (!controller->closing && controller->active) {
                /* The selected night wins over a cached month left behind by
                 * an earlier calendar visit. Resolve it inside the same lock
                 * that publishes the rail and optional month job. */
                const char *target_day = live->has_night
                    ? live->night.day : NULL;
                /* A just-tapped row may already own the foreground
                 * generation without having published its night yet. Use
                 * that known DAY target so Calendar does not briefly open on
                 * the selection it is replacing. The commit-time reconcile
                 * below remains the final invariant for every interleaving. */
                if (controller->retry_job.kind == HISTORY_JOB_DAY &&
                    controller->retry_job.generation ==
                        controller->generation &&
                    controller->retry_job.day[0])
                    target_day = controller->retry_job.day;
                if (target_day)
                    history_controller_month_from_day(
                        target_day, &year, &month);
                if ((!year || !month) && live->has_month) {
                    year = live->month.year;
                    month = live->month.month;
                }
                if (live->rail_mode != TOUCH_HISTORY_UI_RAIL_CALENDAR ||
                    live->calendar_read_error) {
                    live->rail_mode = TOUCH_HISTORY_UI_RAIL_CALENDAR;
                    live->calendar_read_error = false;
                    controller->revision++;
                    notify = true;
                }
                bool month_is_current = live->has_month &&
                    live->month.year == year && live->month.month == month;
                if (year && month && !month_is_current) {
                    history_job_t job = {
                        .kind = HISTORY_JOB_MONTH,
                        .year = year,
                        .month = month,
                        .signal = live->signal,
                        .global_index = SIZE_MAX,
                    };
                    bool calendar_changed = false;
                    queued = history_controller_enqueue_locked(
                        controller, &job, &calendar_changed) == ESP_OK;
                    notify = notify || calendar_changed;
                }
            }
            touch_history_controller_changed_fn changed =
                notify ? controller->config.changed : NULL;
            void *changed_context = controller->config.context;
            xSemaphoreGive(controller->mutex);
            if (changed)
                changed(changed_context);
        }
        if (queued)
            ESP_LOGI(TAG, "queued Calendar month=%04u-%02u",
                     (unsigned)year, (unsigned)month);
        break;
    }
    case TOUCH_HISTORY_UI_INTENT_CLOSE_CALENDAR: {
        bool notify = false;
        if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
            if (controller->model.rail_mode != TOUCH_HISTORY_UI_RAIL_LIST) {
                controller->model.rail_mode = TOUCH_HISTORY_UI_RAIL_LIST;
                controller->model.calendar_loading = false;
                controller->model.calendar_read_error = false;
                controller->calendar_generation++;
                controller->month_pending = false;
                controller->revision++;
                notify = true;
            }
            xSemaphoreGive(controller->mutex);
        }
        if (notify) history_controller_notify(controller);
        break;
    }
    case TOUCH_HISTORY_UI_INTENT_MONTH_RELATIVE: {
        bool notify = false;
        bool queued = false;
        int year = 0;
        int month = 0;
        if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
            history_model_t *live = &controller->model;
            if (!controller->closing && controller->active &&
                live->rail_mode == TOUCH_HISTORY_UI_RAIL_CALENDAR &&
                live->has_month && !live->calendar_loading &&
                (intent->relative <= 0 ||
                 history_controller_can_advance_month(live))) {
                year = live->month.year;
                month = live->month.month + (int)intent->relative;
                while (month < 1) { month += 12; year--; }
                while (month > 12) { month -= 12; year++; }
                if (year >= 2000 && year <= 2200) {
                    history_job_t job = {
                        .kind = HISTORY_JOB_MONTH,
                        .year = (uint16_t)year,
                        .month = (uint8_t)month,
                        .signal = live->signal,
                        .global_index = SIZE_MAX,
                    };
                    queued = history_controller_enqueue_locked(
                        controller, &job, &notify) == ESP_OK;
                }
            }
            touch_history_controller_changed_fn changed =
                notify ? controller->config.changed : NULL;
            void *changed_context = controller->config.context;
            xSemaphoreGive(controller->mutex);
            if (changed)
                changed(changed_context);
        }
        if (queued)
            ESP_LOGI(TAG, "queued Calendar month=%04d-%02d", year, month);
        break;
    }
    case TOUCH_HISTORY_UI_INTENT_SELECT_CALENDAR_DAY: {
        bool queued = false;
        if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
            history_model_t *live = &controller->model;
            uint16_t selected_year = 0;
            uint8_t selected_month = 0;
            history_controller_month_from_day(
                intent->day, &selected_year, &selected_month);
            if (!controller->closing && controller->active &&
                live->rail_mode == TOUCH_HISTORY_UI_RAIL_CALENDAR &&
                !live->calendar_loading && live->has_month && intent->day[0] &&
                selected_year == live->month.year &&
                selected_month == live->month.month) {
                history_job_t job = {
                    .kind = HISTORY_JOB_DAY,
                    .page_offset = live->page.offset,
                    .global_index = SIZE_MAX,
                    .signal = live->signal,
                    .therapy_only = live->therapy_only,
                };
                memcpy(job.day, intent->day, sizeof(job.day));
                queued = history_controller_enqueue_locked(
                    controller, &job, NULL) == ESP_OK;
            }
            xSemaphoreGive(controller->mutex);
        }
        if (queued)
            ESP_LOGI(TAG, "queued Calendar day=%s", intent->day);
        break;
    }
    case TOUCH_HISTORY_UI_INTENT_PREVIOUS_NIGHT:
    case TOUCH_HISTORY_UI_INTENT_NEXT_NIGHT: {
        if (model.selected_global_index == SIZE_MAX) break;
        size_t target = model.selected_global_index;
        if (intent->type == TOUCH_HISTORY_UI_INTENT_PREVIOUS_NIGHT) {
            if (target + 1U >= model.page.total_days) break;
            target++;
        } else {
            if (!target) break;
            target--;
        }
        size_t page_offset = (target / TOUCH_HISTORY_UI_LIST_ROWS) *
                             TOUCH_HISTORY_UI_LIST_ROWS;
        if (page_offset == model.page.offset &&
            target - page_offset < model.page.returned) {
            history_controller_queue_day(
                controller, &model, model.days[target - page_offset].day,
                target, page_offset);
        } else {
            /* DAY asks the worker to load the destination page first. The day
             * string is filled from that page by a small special-case job. */
            history_job_t job = {
                .kind = HISTORY_JOB_PAGE, .page_offset = page_offset,
                .global_index = target, .signal = model.signal,
                .therapy_only = model.therapy_only,
            };
            (void)history_controller_enqueue(controller, &job);
        }
        break;
    }
    case TOUCH_HISTORY_UI_INTENT_SELECT_CHANNEL:
        if (model.has_night && intent->signal >= TOUCH_HISTORY_SIGNAL_FLOW &&
            intent->signal < TOUCH_HISTORY_SIGNAL_COUNT &&
            (model.night.available_signals &
             TOUCH_HISTORY_SIGNAL_BIT(intent->signal)) != 0)
            history_controller_queue_view(
                controller, &model, intent->signal, model.window_start_ms,
                model.window_end_ms,
                model.therapy_only && intent->signal == TOUCH_HISTORY_SIGNAL_SPO2,
                model.window_start_ms != model.night.axis_start_ms ||
                model.window_end_ms != model.night.axis_end_ms);
        break;
    case TOUCH_HISTORY_UI_INTENT_FIT_NIGHT:
        if (model.has_night)
            history_controller_queue_view(
                controller, &model, model.signal, model.night.axis_start_ms,
                model.night.axis_end_ms, model.therapy_only, false);
        break;
    case TOUCH_HISTORY_UI_INTENT_ZOOM_RELATIVE:
        if (model.has_night) {
            int64_t current = model.window_end_ms - model.window_start_ms;
            int64_t night_span = model.night.axis_end_ms -
                                 model.night.axis_start_ms;
            int64_t span = history_controller_zoom_span(
                current, night_span, intent->relative > 0 ? 1 : -1);
            int64_t centre = model.cursor_valid ? model.cursor_ms
                : model.window_start_ms + current / 2;
            int64_t start = 0, end = 0;
            history_controller_clamp_window(&model, centre, span, &start, &end);
            if (start != model.window_start_ms || end != model.window_end_ms)
                history_controller_queue_view(
                    controller, &model, model.signal, start, end,
                    model.therapy_only, true);
        }
        break;
    case TOUCH_HISTORY_UI_INTENT_PAN_RELATIVE:
        if (model.has_night && intent->relative) {
            int64_t span = model.window_end_ms - model.window_start_ms;
            int64_t centre = model.window_start_ms + span / 2;
            if ((intent->relative > 0 && centre <= INT64_MAX - intent->relative) ||
                (intent->relative < 0 && centre >= INT64_MIN - intent->relative))
                centre += intent->relative;
            int64_t start = 0, end = 0;
            history_controller_clamp_window(&model, centre, span, &start, &end);
            if (start != model.window_start_ms || end != model.window_end_ms)
                history_controller_queue_view(
                    controller, &model, model.signal, start, end,
                    model.therapy_only, true);
        }
        break;
    case TOUCH_HISTORY_UI_INTENT_SET_CURSOR: {
        bool notify = false;
        if (model.has_night && intent->timestamp_ms >= model.window_start_ms &&
            intent->timestamp_ms < model.window_end_ms &&
            xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
            controller->model.cursor_ms = intent->timestamp_ms;
            controller->model.cursor_valid = true;
            controller->revision++;
            notify = true;
            xSemaphoreGive(controller->mutex);
        }
        if (notify) history_controller_notify(controller);
        break;
    }
    case TOUCH_HISTORY_UI_INTENT_CLEAR_CURSOR: {
        bool notify = false;
        if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
            if (controller->model.cursor_valid) {
                controller->model.cursor_valid = false;
                controller->model.cursor_ms = 0;
                controller->revision++;
                notify = true;
            }
            xSemaphoreGive(controller->mutex);
        }
        if (notify) history_controller_notify(controller);
        break;
    }
    case TOUCH_HISTORY_UI_INTENT_TOGGLE_THERAPY_ONLY:
        if (model.has_night && model.signal == TOUCH_HISTORY_SIGNAL_SPO2)
            history_controller_queue_view(
                controller, &model, model.signal, model.window_start_ms,
                model.window_end_ms, intent->relative != 0,
                model.window_start_ms != model.night.axis_start_ms ||
                model.window_end_ms != model.night.axis_end_ms);
        break;
    case TOUCH_HISTORY_UI_INTENT_RETRY_READ: {
        history_job_t job;
        if (xSemaphoreTake(controller->mutex, portMAX_DELAY) == pdTRUE) {
            job = controller->retry_job;
            xSemaphoreGive(controller->mutex);
            (void)history_controller_enqueue(controller, &job);
        }
        break;
    }
    case TOUCH_HISTORY_UI_INTENT_CANCEL_AUTO_LOAD:
    case TOUCH_HISTORY_UI_INTENT_OPEN_CARD:
        break;
    }
}

static const char *history_controller_stat_label(touch_history_stat_kind_t kind)
{
    switch (kind) {
    case TOUCH_HISTORY_STAT_P50: return "P50";
    case TOUCH_HISTORY_STAT_P95: return "P95";
    case TOUCH_HISTORY_STAT_P995: return "P99.5";
    case TOUCH_HISTORY_STAT_ABSOLUTE_P50: return "|P50|";
    case TOUCH_HISTORY_STAT_ABSOLUTE_P95: return "|P95|";
    case TOUCH_HISTORY_STAT_ABSOLUTE_P995: return "|P99.5|";
    case TOUCH_HISTORY_STAT_MINIMUM: return "Minimum";
    case TOUCH_HISTORY_STAT_P5: return "P5";
    case TOUCH_HISTORY_STAT_P05: return "P0.5";
    case TOUCH_HISTORY_STAT_TIME_BELOW_88: return "Time <88%";
    case TOUCH_HISTORY_STAT_MEDIAN: return "Median";
    case TOUCH_HISTORY_STAT_MAXIMUM: return "Maximum";
    }
    return "—";
}

static const char *history_controller_signal_unit(touch_history_signal_t signal)
{
    switch (signal) {
    case TOUCH_HISTORY_SIGNAL_FLOW: return "L/s";
    case TOUCH_HISTORY_SIGNAL_LEAK: return "L/min";
    case TOUCH_HISTORY_SIGNAL_PRESSURE: return "cmH2O";
    case TOUCH_HISTORY_SIGNAL_SPO2: return "%";
    case TOUCH_HISTORY_SIGNAL_PULSE: return "bpm";
    case TOUCH_HISTORY_SIGNAL_FLOW_LIMIT:
    case TOUCH_HISTORY_SIGNAL_SNORE:
    case TOUCH_HISTORY_SIGNAL_MOTION:
    case TOUCH_HISTORY_SIGNAL_COUNT: return "";
    }
    return "";
}

esp_err_t touch_history_controller_apply(
    touch_history_controller_t *controller, touch_history_ui_t *ui)
{
    if (!controller || !ui) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE)
        return ESP_FAIL;
    history_model_t *model = &controller->model;
    touch_history_ui_snapshot_t snapshot = {
        .state = model->state,
        .rail_mode = model->rail_mode,
        .days = model->days,
        .day_count = model->page.returned,
        .page = model->page,
        .selected_row = model->selected_row,
        .night = model->has_night ? &model->night : NULL,
        .sessions = model->sessions,
        .session_count = model->session_count,
        .overview = model->has_overview ? &model->overview : NULL,
        .events = model->events,
        .event_count = model->event_count,
        .event_total_count = model->event_total_count,
        .event_state = model->event_state,
        .events_truncated = model->events_truncated,
        .month = model->has_month ? &model->month : NULL,
        .can_previous_month = model->has_month && model->month.year > 2000,
        .can_next_month = model->has_month &&
            history_controller_can_advance_month(model),
        .calendar_loading = model->calendar_loading,
        .calendar_read_error = model->calendar_read_error,
        .selected_signal = model->signal,
        .can_previous_night = model->selected_global_index != SIZE_MAX &&
            model->selected_global_index + 1U < model->page.total_days,
        .can_next_night = model->selected_global_index != SIZE_MAX &&
            model->selected_global_index > 0,
        .usage_target_known = model->usage_target_known,
        .usage_on_target = model->usage_on_target,
        .therapy_only = model->therapy_only,
        .cursor_valid = model->cursor_valid,
        .cursor_ms = model->cursor_ms,
        .progress_per_mille = model->progress_per_mille,
        .status_text = model->status,
        .error_text = model->error,
        .degraded_text = model->degraded,
        .stats_warning_text = model->stats_warning,
    };
    const char *unit = history_controller_signal_unit(model->signal);
    for (size_t i = 0; i < TOUCH_HISTORY_UI_STAT_COUNT; ++i) {
        if (i < model->stats.value_count) {
            const touch_history_stat_value_t *value = &model->stats.values[i];
            snapshot.stats[i].label = history_controller_stat_label(value->kind);
            snapshot.stats[i].unit = value->kind == TOUCH_HISTORY_STAT_TIME_BELOW_88
                ? "min" : unit;
            snapshot.stats[i].value_x100 = value->value_x100;
            snapshot.stats[i].available = value->available;
        } else {
            snapshot.stats[i].label = "";
            snapshot.stats[i].unit = "";
        }
    }
    esp_err_t result = touch_history_ui_apply(ui, &snapshot);
    if (result == ESP_OK) controller->rendered_revision = controller->revision;
    xSemaphoreGive(controller->mutex);
    return result;
}

uint32_t touch_history_controller_revision(
    touch_history_controller_t *controller)
{
    if (!controller || xSemaphoreTake(controller->mutex, portMAX_DELAY) != pdTRUE)
        return 0;
    uint32_t revision = controller->revision;
    xSemaphoreGive(controller->mutex);
    return revision;
}
