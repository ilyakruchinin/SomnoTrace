/*
 * SomnoTrace - Deferred execution of slow HTTP handlers
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

#include "httpd_defer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdlib.h>
#include "psram_task.h"

static const char *TAG = "httpd_defer";

typedef struct {
    httpd_req_t *req;
    httpd_defer_fn_t fn;
} defer_job_t;

#define DEFER_QUEUE_DEPTH  12     /* pending jobs before we answer 503 */
#define DEFER_WORKER_STACK 12288  /* handlers ran on httpd's 12 KB stack */
static QueueHandle_t s_defer_queue = NULL;

static void defer_worker(void *arg)
{
    (void)arg;
    for (;;) {
        defer_job_t *job = NULL;
        if (xQueueReceive(s_defer_queue, &job, portMAX_DELAY) != pdTRUE || !job)
            continue;
        esp_err_t err = job->fn(job->req);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "deferred handler failed (err=%d)", err);
        httpd_req_async_handler_complete(job->req);
        free(job);
    }
}

void httpd_defer_init(void)
{
    if (s_defer_queue) return;
    s_defer_queue = xQueueCreate(DEFER_QUEUE_DEPTH, sizeof(defer_job_t *));
    if (!s_defer_queue) {
        ESP_LOGE(TAG, "failed to create defer queue");
        return;
    }
    /* PSRAM stack: deferred handlers only read SD and write to sockets —
     * no flash writes — same constraint as the snt_download worker. */
    TaskHandle_t h = psram_task_create(defer_worker, "httpd_defer",
                                       DEFER_WORKER_STACK, NULL, 4,
                                       tskNO_AFFINITY, NULL, NULL);
    if (!h) {
        ESP_LOGE(TAG, "failed to create defer worker");
        vQueueDelete(s_defer_queue);
        s_defer_queue = NULL;
    } else {
        ESP_LOGI(TAG, "deferred-handler worker started (PSRAM stack)");
    }
}

static void send_503(httpd_req_t *req)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"error\":\"server busy, retry\"}");
}

esp_err_t httpd_defer_dispatch(httpd_req_t *req, httpd_defer_fn_t fn)
{
    if (!s_defer_queue) {
        send_503(req);
        return ESP_OK;
    }

    defer_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM);
    if (!job) job = calloc(1, sizeof(*job));

    httpd_req_t *async_req = NULL;
    if (!job || httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        free(job);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    job->req = async_req;
    job->fn = fn;

    if (xQueueSend(s_defer_queue, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "defer queue full, rejecting request");
        send_503(async_req);
        httpd_req_async_handler_complete(async_req);
        free(job);
    }
    return ESP_OK;
}
