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
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"

static const char *TAG = "psram_task";

#define PSRAM_REAPER_QUEUE_DEPTH 8
#define PSRAM_REAPER_PRIORITY    (configMAX_PRIORITIES - 1)

static StaticQueue_t s_reaper_queue_control;
static uint8_t s_reaper_queue_storage[
    PSRAM_REAPER_QUEUE_DEPTH * sizeof(TaskHandle_t)];
static QueueHandle_t s_reaper_queue;
static StaticTask_t s_reaper_task_control;
static StackType_t s_reaper_task_stack[configMINIMAL_STACK_SIZE];
static TaskHandle_t s_reaper_task;

static void psram_task_reaper(void *arg)
{
    (void)arg;
    while (true) {
        TaskHandle_t victim = NULL;
        if (xQueueReceive(s_reaper_queue, &victim, portMAX_DELAY) == pdTRUE &&
            victim != NULL) {
            /* This is deliberately never a self-delete. ESP-IDF therefore
             * reclaims the WithCaps buffers directly and does not allocate its
             * temporary cleanup task. */
            vTaskDeleteWithCaps(victim);
        }
    }
}

esp_err_t psram_task_init(void)
{
    if (s_reaper_task != NULL)
        return ESP_OK;

    s_reaper_queue = xQueueCreateStatic(
        PSRAM_REAPER_QUEUE_DEPTH, sizeof(TaskHandle_t),
        s_reaper_queue_storage, &s_reaper_queue_control);
    if (s_reaper_queue == NULL) {
        ESP_LOGE(TAG, "failed to create retained task-reaper queue");
        return ESP_ERR_NO_MEM;
    }

    s_reaper_task = xTaskCreateStaticPinnedToCore(
        psram_task_reaper, "psram_reaper", configMINIMAL_STACK_SIZE, NULL,
        PSRAM_REAPER_PRIORITY, s_reaper_task_stack, &s_reaper_task_control,
        tskNO_AFFINITY);
    if (s_reaper_task == NULL) {
        ESP_LOGE(TAG, "failed to create retained task reaper");
        s_reaper_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
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
    /* xTaskCreateStaticPinnedToCore() cannot reclaim caller-provided buffers
     * when a task deletes itself. ESP-IDF's WithCaps pair records the PSRAM
     * stack/internal-TCB ownership; our retained reaper performs deletion. */
    if (out_stack) *out_stack = NULL;
    if (out_tcb) *out_tcb = NULL;

    if (s_reaper_task == NULL) {
        ESP_LOGE(TAG, "cannot create %s before psram_task_init", name);
        return NULL;
    }

    TaskHandle_t h = NULL;
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        task_func, name, stack_size, arg, priority, &h, core_id,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS || !h) {
        ESP_LOGE(TAG,
                 "failed to allocate %s: stack=%u PSRAM free=%u, internal free=%u",
                 name, (unsigned)stack_size,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return NULL;
    }
    return h;
}

void psram_task_delete(TaskHandle_t task)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    if (task != NULL && task != current) {
        vTaskDeleteWithCaps(task);
        return;
    }

    if (s_reaper_task == NULL || s_reaper_queue == NULL) {
        ESP_LOGE(TAG, "retained task reaper unavailable");
        abort();
    }

    if (xQueueSend(s_reaper_queue, &current, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "could not enqueue task for reclamation");
        abort();
    }

    /* The higher-priority reaper normally deletes us before this executes.
     * Suspending also makes the handoff correct if it runs on the other core. */
    vTaskSuspend(NULL);
    ESP_LOGE(TAG, "reclaimed task unexpectedly resumed");
    abort();
}
