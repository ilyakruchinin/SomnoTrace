/*
 * SomnoTrace - Centralised NVS/flash writer task
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

#include "nvs_writer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "nvs_writer";

/* Internal-RAM stack: this task performs the actual flash writes, so its own
 * stack must not be in PSRAM. NVS commits fit comfortably in 4 KB. */
#define NVS_WRITER_STACK   8192
#define NVS_WRITER_PRIO    6      /* >= httpd worker so a submitted write runs promptly */

typedef struct {
    nvs_writer_fn_t fn;
    void           *arg;
    esp_err_t       result;
} nvs_cmd_t;

static QueueHandle_t     s_cmd_q      = NULL;   /* holds nvs_cmd_t* */
static SemaphoreHandle_t s_submit_mtx = NULL;   /* serialises submitters */
static SemaphoreHandle_t s_done       = NULL;   /* writer -> submitter completion */
static SemaphoreHandle_t s_nvs_lock   = NULL;   /* global NVS access serialisation */
static volatile uint8_t  s_init_state = 0;      /* 0=not attempted, 1=ready, 2=failed */

void nvs_writer_lock(void)
{
    if (s_nvs_lock) xSemaphoreTakeRecursive(s_nvs_lock, portMAX_DELAY);
}

void nvs_writer_unlock(void)
{
    if (s_nvs_lock) xSemaphoreGiveRecursive(s_nvs_lock);
}

static void nvs_writer_task(void *arg)
{
    (void)arg;
    bool hwm_logged = false;
    for (;;) {
        nvs_cmd_t *cmd = NULL;
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) == pdTRUE && cmd) {
            /* Acquire the global NVS lock so that direct NVS callers (which
             * call nvs_writer_lock/unlock around their own nvs_open/commit)
             * are serialized with proxy operations.  This prevents a
             * concurrent flash erase from disabling the cache while the
             * other task is mid-read from flash-mapped memory. */
            nvs_writer_lock();
            cmd->result = cmd->fn ? cmd->fn(cmd->arg) : ESP_ERR_INVALID_ARG;
            nvs_writer_unlock();

            UBaseType_t high_water = uxTaskGetStackHighWaterMark(NULL);
            if (!hwm_logged) {
                hwm_logged = true;
                ESP_LOGI(TAG, "stack high-water = %u bytes",
                         (unsigned)(high_water * sizeof(StackType_t)));
            }
            if (high_water < 512) {
                ESP_LOGW(TAG, "low stack watermark after NVS operation: %u words",
                         (unsigned)high_water);
            }
            xSemaphoreGive(s_done);
        }
    }
}

void nvs_writer_init(void)
{
    if (s_init_state != 0) return;   /* already initialised/failed */
    s_init_state = 2;                /* fail closed until fully ready */

    s_submit_mtx = xSemaphoreCreateMutex();
    s_done       = xSemaphoreCreateBinary();
    s_nvs_lock   = xSemaphoreCreateRecursiveMutex();
    s_cmd_q      = xQueueCreate(1, sizeof(nvs_cmd_t *));
    if (!s_submit_mtx || !s_done || !s_nvs_lock || !s_cmd_q) {
        ESP_LOGE(TAG, "alloc failed (mtx=%p done=%p q=%p)",
                 s_submit_mtx, s_done, s_cmd_q);
        s_cmd_q = NULL;   /* nvs_writer_run() will fail closed */
        return;
    }

    if (xTaskCreatePinnedToCore(nvs_writer_task, "nvs_writer", NVS_WRITER_STACK,
                                NULL, NVS_WRITER_PRIO, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "task create failed — NVS proxy will fail closed");
        s_cmd_q = NULL;
        return;
    }
    s_init_state = 1;
    ESP_LOGI(TAG, "nvs_writer task started (internal stack=%d bytes)", NVS_WRITER_STACK);
}

esp_err_t nvs_writer_run(nvs_writer_fn_t fn, void *arg)
{
    if (!fn) return ESP_ERR_INVALID_ARG;

    /* Before initialisation is attempted, early boot callers are still on an
     * internal-RAM stack and may run inline. After an init failure, fail closed
     * rather than performing a flash operation from an unknown stack. */
    if (s_init_state == 0) return fn(arg);
    if (s_init_state != 1 || !s_cmd_q) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_submit_mtx, portMAX_DELAY);
    nvs_cmd_t cmd = { .fn = fn, .arg = arg, .result = ESP_FAIL };
    nvs_cmd_t *p = &cmd;
    xQueueSend(s_cmd_q, &p, portMAX_DELAY);
    xSemaphoreTake(s_done, portMAX_DELAY);
    esp_err_t r = cmd.result;
    xSemaphoreGive(s_submit_mtx);
    return r;
}
