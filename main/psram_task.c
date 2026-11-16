/*
 * SomnoTrace - PSRAM-backed FreeRTOS task creation helper
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#include "psram_task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

static const char *TAG = "psram_task";

static unsigned s_work_active;

static void log_work_phase(const char *phase, uint32_t stack_bytes, unsigned active)
{
    multi_heap_info_t internal, psram;
    heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heap_caps_get_info(&psram, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    unsigned hwm = (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
    ESP_LOGI(TAG, "work phase=%s task=%s stack=%u hwm=%u active=%u "
             "internal=%u/%u/%u psram=%u/%u/%u (free/min/largest)",
             phase, pcTaskGetName(NULL), (unsigned)stack_bytes, hwm, active,
             (unsigned)internal.total_free_bytes, (unsigned)internal.minimum_free_bytes,
             (unsigned)internal.largest_free_block,
             (unsigned)psram.total_free_bytes, (unsigned)psram.minimum_free_bytes,
             (unsigned)psram.largest_free_block);
    if (hwm < 2048)
        ESP_LOGW(TAG, "work phase=%s task=%s low stack watermark=%u B",
                 phase, pcTaskGetName(NULL), hwm);
}

void psram_work_begin(const char *phase, uint32_t stack_bytes)
{
    unsigned active = __atomic_add_fetch(&s_work_active, 1, __ATOMIC_RELAXED);
    log_work_phase(phase, stack_bytes, active);
}

void psram_work_phase(const char *phase, uint32_t stack_bytes)
{
    log_work_phase(phase, stack_bytes, __atomic_load_n(&s_work_active, __ATOMIC_RELAXED));
}

void psram_work_end(const char *phase, uint32_t stack_bytes)
{
    log_work_phase(phase, stack_bytes, __atomic_load_n(&s_work_active, __ATOMIC_RELAXED));
    __atomic_sub_fetch(&s_work_active, 1, __ATOMIC_RELAXED);
}

#define REAPER_INTERVAL_US (2000 * 1000) /* 2 seconds */

typedef struct psram_task_entry {
    TaskHandle_t handle;
    StackType_t *stack;
    StaticTask_t *tcb;
    bool marked_deleted;
    struct psram_task_entry *next;
} psram_task_entry_t;

static psram_task_entry_t *s_task_list = NULL;
static StaticSemaphore_t s_mutex_buffer;
static SemaphoreHandle_t s_reaper_mutex = NULL;
static esp_timer_handle_t s_reaper_timer = NULL;
static bool s_timer_running = false;

static void reaper_timer_cb(void *arg);

static portMUX_TYPE s_reaper_spinlock = portMUX_INITIALIZER_UNLOCKED;

static void init_reaper_mutex(void)
{
    if (s_reaper_mutex == NULL) {
        portENTER_CRITICAL(&s_reaper_spinlock);
        if (s_reaper_mutex == NULL) {
            s_reaper_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buffer);
        }
        portEXIT_CRITICAL(&s_reaper_spinlock);
    }
}

static void ensure_reaper_timer_locked(void)
{
    if (s_reaper_timer == NULL) {
        esp_timer_create_args_t timer_args = {
            .callback = reaper_timer_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "psram_reap",
            .skip_unhandled_events = true,
        };
        esp_err_t err = esp_timer_create(&timer_args, &s_reaper_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to create reaper timer: %s", esp_err_to_name(err));
            return;
        }
    }
    if (!s_timer_running) {
        esp_err_t err = esp_timer_start_periodic(s_reaper_timer, REAPER_INTERVAL_US);
        if (err == ESP_OK) {
            s_timer_running = true;
        }
    }
}

void psram_task_reap(void)
{
    init_reaper_mutex();
    if (!s_reaper_mutex) return;

    if (xSemaphoreTake(s_reaper_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    psram_task_entry_t **curr = &s_task_list;
    while (*curr != NULL) {
        psram_task_entry_t *entry = *curr;
        eTaskState state = eTaskGetState(entry->handle);
        if (state == eDeleted) {
            if (entry->marked_deleted) {
                /* Confirmed dead across two sweeps; safe to free. */
                *curr = entry->next;
                ESP_LOGD(TAG, "reaped deleted task %p: stack=%p, tcb=%p",
                         (void *)entry->handle, (void *)entry->stack, (void *)entry->tcb);
                heap_caps_free(entry->stack);
                heap_caps_free(entry->tcb);
                free(entry);
                continue;
            } else {
                /* First sweep seeing it deleted: mark it for the next sweep
                 * to ensure context switch and idle task cleanup have finished. */
                entry->marked_deleted = true;
            }
        } else {
            entry->marked_deleted = false;
        }
        curr = &(entry->next);
    }

    if (s_task_list == NULL && s_reaper_timer != NULL && s_timer_running) {
        esp_timer_stop(s_reaper_timer);
        s_timer_running = false;
    }

    xSemaphoreGive(s_reaper_mutex);
}

static void reaper_timer_cb(void *arg)
{
    (void)arg;
    psram_task_reap();
}

TaskHandle_t psram_task_create(TaskFunction_t task_func,
                               const char *name,
                               uint32_t stack_size,
                               void *arg,
                               UBaseType_t priority,
                               BaseType_t core_id,
                               StackType_t **out_stack,
                               StaticTask_t **out_tcb)
{
    /* Opportunistically reclaim any exited tasks first */
    psram_task_reap();

    StackType_t *stack = heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
    StaticTask_t *tcb  = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    psram_task_entry_t *entry = NULL;

    bool auto_reap = (out_stack == NULL && out_tcb == NULL);
    if (auto_reap) {
        entry = heap_caps_malloc(sizeof(psram_task_entry_t), MALLOC_CAP_INTERNAL);
    }

    if (!stack || !tcb || (auto_reap && !entry)) {
        ESP_LOGE(TAG, "failed to allocate %s: stack=%u PSRAM free=%u, tcb internal free=%u",
                 name, (unsigned)stack_size,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        free(stack);
        free(tcb);
        free(entry);
        return NULL;
    }

    TaskHandle_t h = xTaskCreateStaticPinnedToCore(
        task_func, name, stack_size, arg, priority, stack, tcb, core_id);
    if (!h) {
        ESP_LOGE(TAG, "xTaskCreateStaticPinnedToCore failed for %s", name);
        free(stack);
        free(tcb);
        free(entry);
        return NULL;
    }

    if (auto_reap) {
        entry->handle = h;
        entry->stack = stack;
        entry->tcb = tcb;
        entry->marked_deleted = false;

        init_reaper_mutex();
        if (xSemaphoreTake(s_reaper_mutex, portMAX_DELAY) == pdTRUE) {
            entry->next = s_task_list;
            s_task_list = entry;
            ensure_reaper_timer_locked();
            xSemaphoreGive(s_reaper_mutex);
        } else {
            /* Highly unexpected: couldn't take mutex; clean up entry node.
             * Task still runs, but won't be auto-reaped. */
            free(entry);
        }
    } else {
        if (out_stack) *out_stack = stack;
        if (out_tcb)   *out_tcb   = tcb;
    }

    return h;
}
