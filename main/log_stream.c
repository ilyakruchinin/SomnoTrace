/*
 * SomnoTrace - Log stream: ring-buffered log capture with SSE delivery
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#include "log_stream.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "sd_storage.h"
#include "psram_task.h"
#include "net_provision.h"
#include "uploader.h"
#include "as11_ble.h"
#include "oximeter.h"

static const char *TAG = "log_stream";

/* ── Ring Buffer ──────────────────────────────────────────────────── */

#define RINGBUF_SIZE_INTERNAL   (8  * 1024)
#define RINGBUF_SIZE_PSRAM      (16 * 1024)
#define LOG_LINE_MAX            256

/* ── SD Persistent Logging ───────────────────────────────────────── */

#define LOG_DIR             SD_LOG_DIR
#define LOG_FILE_PREFIX     "somnotrace.log."
#define LOG_MAX_FILES       4
#define LOG_FILE_MAX_SIZE   (128 * 1024)  /* 128 KB per file */
#define LOG_TOTAL_MAX       (LOG_FILE_MAX_SIZE * LOG_MAX_FILES)  /* 512 KB */
#define WRITEBUF_SIZE       (8 * 1024)    /* PSRAM write buffer */
#define FLUSH_INTERVAL_MS   2000          /* flush at least every 2 s */
#define FLUSH_THRESHOLD     4096          /* flush when buffer reaches 4 KB */

static RingbufHandle_t s_ringbuf;
static vprintf_like_t  s_orig_vprintf;

/* Write buffer for SD persistence (separate from SSE ring buffer). */
static uint8_t *s_writebuf;          /* PSRAM buffer */
static size_t   s_writebuf_head;     /* write position (from vprintf hook) */
static size_t   s_writebuf_tail;     /* read position (from flush task) */
static SemaphoreHandle_t s_writebuf_mutex;
static TaskHandle_t s_flush_task;
static bool s_sd_ready;              /* SD card is mounted and log dir created */

/* ── WebSocket Live Tail ──────────────────────────────────────────── */
#define MAX_WS_CLIENTS 4

typedef struct {
    httpd_handle_t hd;
    int fd;
    bool paused;  /* per-client: when true, non-log pushes are skipped for this client */
} ws_client_t;

static ws_client_t       s_ws_clients[MAX_WS_CLIENTS];
static int               s_ws_client_count = 0;
static SemaphoreHandle_t s_ws_mutex;
static TaskHandle_t     s_ws_fwd_task;

/**
 * Custom vprintf hook installed via esp_log_set_vprintf().
 *
 * 1. Forward to the original UART handler (so serial console keeps working).
 * 2. Render the formatted string into a scratch buffer.
 * 3. Push it into the ring buffer (best-effort, drop if full).
 * 4. Notify the SD flush task if enough data has accumulated.
 */

static int log_vprintf_hook(const char *fmt, va_list args)
{
    /* Two consumers, two independent va_lists.
     *
     * A va_list is consumed by a vprintf-family call: after s_orig_vprintf()
     * returns, `args` is indeterminate and reusing it for vsnprintf() is
     * undefined behaviour, not merely unportable.  Each consumer therefore
     * gets its own va_copy, and the caller's list is never touched here. */
    va_list uart_args;
    va_copy(uart_args, args);
    int ret = s_orig_vprintf(fmt, uart_args);
    va_end(uart_args);

    /* Render into a stack-local scratch buffer. */
    char buf[LOG_LINE_MAX];
    va_list buf_args;
    va_copy(buf_args, args);
    int len = vsnprintf(buf, sizeof(buf), fmt, buf_args);
    va_end(buf_args);
    if (len <= 0) {
        return ret;
    }
    if (len >= (int)sizeof(buf)) {
        len = sizeof(buf) - 1;
    }

    /* Push to SSE ring buffer (best-effort, drop if full). */
    if (s_ringbuf) {
        xRingbufferSend(s_ringbuf, buf, (size_t)len, 0);
    }

    /* Push to SD write buffer (best-effort, drop if full or mutex busy). */
    if (s_writebuf && xSemaphoreTake(s_writebuf_mutex, 0) == pdTRUE) {
        size_t avail = WRITEBUF_SIZE - (s_writebuf_head - s_writebuf_tail);
        if ((size_t)len <= avail) {
            size_t pos = s_writebuf_head % WRITEBUF_SIZE;
            size_t chunk1 = WRITEBUF_SIZE - pos;
            if ((size_t)len <= chunk1) {
                memcpy(s_writebuf + pos, buf, (size_t)len);
            } else {
                memcpy(s_writebuf + pos, buf, chunk1);
                memcpy(s_writebuf, buf + chunk1, (size_t)len - chunk1);
            }
            s_writebuf_head += (size_t)len;
        }
        xSemaphoreGive(s_writebuf_mutex);
    }

    /* Wake the SD flush task if enough data has accumulated. */
    if (s_flush_task && s_writebuf_head - s_writebuf_tail >= FLUSH_THRESHOLD) {
        xTaskNotifyGive(s_flush_task);
    }

    return ret;
}

/* ── SD Persistent Logging ───────────────────────────────────────── */

/* Ensure the log directory exists on the SD card. */
static void ensure_log_dir(void)
{
    if (!sd_storage_is_ready()) return;
    mkdir(SD_APP_DIR, 0777);  /* parent .somnotrace/ (non-recursive mkdir) */
    mkdir(LOG_DIR, 0777);     /* ignore EEXIST */
    s_sd_ready = true;
}

/* Rotate log files: delete oldest, shift others up, create new .0 */
static void rotate_logs(void)
{
    char path[64];

    /* Delete the oldest file (somnotrace.log.2) */
    snprintf(path, sizeof(path), "%s/%s%d", LOG_DIR, LOG_FILE_PREFIX, LOG_MAX_FILES - 1);
    remove(path);

    /* Shift: .1 -> .2, .0 -> .1 */
    for (int i = LOG_MAX_FILES - 2; i >= 0; i--) {
        char old_path[64], new_path[64];
        snprintf(old_path, sizeof(old_path), "%s/%s%d", LOG_DIR, LOG_FILE_PREFIX, i);
        snprintf(new_path, sizeof(new_path), "%s/%s%d", LOG_DIR, LOG_FILE_PREFIX, i + 1);
        rename(old_path, new_path);
    }
}

/* Get current log file size. */
static long log_file_size(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s0", LOG_DIR, LOG_FILE_PREFIX);
    struct stat st;
    if (stat(path, &st) == 0) return st.st_size;
    return 0;
}

/* Background flush task: drains write buffer to SD card. */
static void log_flush_task(void *arg)
{
    while (true) {
        /* Wait for notification or timeout (periodic flush). */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FLUSH_INTERVAL_MS));

        if (!s_sd_ready) {
            ensure_log_dir();
            if (!s_sd_ready) continue;
        }
        /* s_sd_ready is a latch; skip the flush while the card is actually
         * away (e.g. a reformat in progress) so it does not log an error per
         * tick against the unmounted volume. */
        if (!sd_storage_is_ready()) continue;

        size_t avail;
        xSemaphoreTake(s_writebuf_mutex, portMAX_DELAY);
        avail = s_writebuf_head - s_writebuf_tail;
        xSemaphoreGive(s_writebuf_mutex);

        if (avail == 0) continue;

        /* Check if we need to rotate before writing. */
        long cur_size = log_file_size();
        if (cur_size >= LOG_FILE_MAX_SIZE) {
            rotate_logs();
            cur_size = 0;
        }

        /* Open current log file for append. */
        char path[64];
        snprintf(path, sizeof(path), "%s/%s0", LOG_DIR, LOG_FILE_PREFIX);
        FILE *f = fopen(path, "ab");
        if (!f) {
            ESP_LOGE(TAG, "flush: cannot open %s (errno %d)", path, errno);
            continue;
        }

        /* Drain available data from write buffer. */
        uint8_t tmp[LOG_LINE_MAX];
        size_t total_written = 0;

        while (true) {
            xSemaphoreTake(s_writebuf_mutex, portMAX_DELAY);
            size_t pending = s_writebuf_head - s_writebuf_tail;
            if (pending == 0) {
                xSemaphoreGive(s_writebuf_mutex);
                break;
            }
            size_t pos = s_writebuf_tail % WRITEBUF_SIZE;
            size_t chunk = WRITEBUF_SIZE - pos;
            if (chunk > pending) chunk = pending;
            if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
            memcpy(tmp, s_writebuf + pos, chunk);
            s_writebuf_tail += chunk;
            xSemaphoreGive(s_writebuf_mutex);

            size_t written = fwrite(tmp, 1, chunk, f);
            total_written += written;
            if (written != chunk) break;

            /* Check rotation mid-flush. */
            if (cur_size + (long)total_written >= LOG_FILE_MAX_SIZE) {
                fclose(f);
                rotate_logs();
                cur_size = 0;
                total_written = 0;
                snprintf(path, sizeof(path), "%s/%s0", LOG_DIR, LOG_FILE_PREFIX);
                f = fopen(path, "ab");
                if (!f) break;
            }
        }

        if (f) fclose(f);

        /* One-shot high-water mark after the first flush to verify the
         * 4 KB PSRAM stack is sufficient for the FATFS write path. */
        static bool hwm_logged = false;
        if (!hwm_logged) {
            hwm_logged = true;
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "log_flush: stack high-water = %u bytes",
                     (unsigned)(hwm * sizeof(StackType_t)));
        }
    }
}

/* ── Initialisation ───────────────────────────────────────────────── */

void log_stream_init(void)
{
    /* Prefer PSRAM (larger buffer) if available, else internal RAM. */
    size_t buf_sz;
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > RINGBUF_SIZE_PSRAM * 2) {
        s_ringbuf = xRingbufferCreateWithCaps(RINGBUF_SIZE_PSRAM,
                                              RINGBUF_TYPE_BYTEBUF,
                                              MALLOC_CAP_SPIRAM);
        buf_sz = RINGBUF_SIZE_PSRAM;
    } else {
        s_ringbuf = xRingbufferCreateWithCaps(RINGBUF_SIZE_INTERNAL,
                                              RINGBUF_TYPE_BYTEBUF,
                                              MALLOC_CAP_INTERNAL);
        buf_sz = RINGBUF_SIZE_INTERNAL;
    }
    if (!s_ringbuf) {
        ESP_LOGE(TAG, "failed to create log ring buffer");
        return;
    }

    /* Allocate SD write buffer in PSRAM. */
    s_writebuf = heap_caps_malloc(WRITEBUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_writebuf) {
        s_writebuf = heap_caps_malloc(WRITEBUF_SIZE, MALLOC_CAP_INTERNAL);
    }
    if (s_writebuf) {
        s_writebuf_mutex = xSemaphoreCreateMutex();
        if (!s_writebuf_mutex) {
            free(s_writebuf);
            s_writebuf = NULL;
            ESP_LOGW(TAG, "failed to create write buffer mutex, SD logging disabled");
        }
    } else {
        ESP_LOGW(TAG, "failed to allocate write buffer, SD logging disabled");
    }

    ESP_LOGI(TAG, "log stream init: %u-byte ring buffer (%s), %s SD logging",
             (unsigned)buf_sz,
             buf_sz == RINGBUF_SIZE_PSRAM ? "PSRAM" : "internal",
             s_writebuf ? "with" : "without");

    /* Install our hook; stash the original handler. */
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf_hook);

    /* Start the SD flush task (low priority, core 0). */
    if (s_writebuf) {
        s_flush_task = psram_task_create(log_flush_task, "log_flush", 4096, NULL, 3, 0, NULL, NULL);
    }

    /* Create the WebSocket mutex (protects s_ws_hd/s_ws_fd). */
    s_ws_mutex = xSemaphoreCreateMutex();
}

/* Check if any non-paused WebSocket client is connected.
 * Used by the forwarder loop to skip building JSON when all clients
 * are paused (e.g. background tabs).  Log messages bypass this check. */
static bool ws_has_active_client(void)
{
    bool active = false;
    if (xSemaphoreTake(s_ws_mutex, 0) == pdTRUE) {
        for (int i = 0; i < s_ws_client_count; i++) {
            if (!s_ws_clients[i].paused) {
                active = true;
                break;
            }
        }
        xSemaphoreGive(s_ws_mutex);
    }
    return active;
}

/* Push-now flags — set by external event sources, consumed by the forwarder
 * task.  Volatile because they are written from other tasks/contexts. */
static volatile bool s_push_status_now = false;
static volatile bool s_push_upload_now = false;
static volatile bool s_push_ble_now    = false;
static volatile bool s_push_ox_now     = false;

/* Request an immediate upload-progress push on the next forwarder cycle
 * (e.g. on a backend state transition).  Safe to call from any task. */
void log_stream_request_upload_push(void)
{
    s_push_upload_now = true;
}

/* Request an immediate BLE-state push on the next forwarder cycle
 * (e.g. on a pairing state change).  Safe to call from any task. */
void log_stream_request_ble_push(void)
{
    s_push_ble_now = true;
}

/* Request an immediate oximeter-state push on the next forwarder cycle
 * (e.g. on a pairing or sync state change).  Safe to call from any task. */
void log_stream_request_ox_push(void)
{
    s_push_ox_now = true;
}

static esp_err_t ws_queue_send(httpd_handle_t hd, int fd, uint8_t type,
                               const char *payload, size_t len);

static void ws_remove_client_locked(int fd)
{
    for (int i = 0; i < s_ws_client_count; i++) {
        if (s_ws_clients[i].fd == fd) {
            for (int j = i; j < s_ws_client_count - 1; j++) {
                s_ws_clients[j] = s_ws_clients[j + 1];
            }
            s_ws_client_count--;
            break;
        }
    }
}

static void ws_add_client_locked(httpd_handle_t hd, int fd)
{
    for (int i = 0; i < s_ws_client_count; i++) {
        if (s_ws_clients[i].fd == fd) return;
    }
    if (s_ws_client_count >= MAX_WS_CLIENTS) {
        httpd_handle_t old_hd = s_ws_clients[0].hd;
        int old_fd = s_ws_clients[0].fd;
        ESP_LOGI(TAG, "ws: evicting oldest client (fd=%d) for new client (fd=%d)", old_fd, fd);

        const char *evict_msg = "{\"type\":\"evicted\",\"reason\":\"max_clients_exceeded\"}";
        ws_queue_send(old_hd, old_fd, HTTPD_WS_TYPE_TEXT, evict_msg,
                      strlen(evict_msg));
        ws_queue_send(old_hd, old_fd, HTTPD_WS_TYPE_CLOSE, NULL, 0);

        for (int i = 0; i < s_ws_client_count - 1; i++) {
            s_ws_clients[i] = s_ws_clients[i + 1];
        }
        s_ws_client_count--;
    }
    s_ws_clients[s_ws_client_count].hd = hd;
    s_ws_clients[s_ws_client_count].fd = fd;
    s_ws_clients[s_ws_client_count].paused = false;
    s_ws_client_count++;
    ESP_LOGI(TAG, "ws: client connected (fd=%d, total=%d)", fd, s_ws_client_count);
}

typedef struct {
    httpd_handle_t hd;
    int fd;
    uint8_t type;
    char *payload;
    size_t len;
} ws_send_work_t;

static void ws_send_work(void *arg)
{
    ws_send_work_t *work = arg;
    if (!work) return;
    httpd_ws_frame_t pkt = {
        .final = true,
        .type = work->type,
        .payload = (uint8_t *)work->payload,
        .len = work->len,
    };
    esp_err_t err = httpd_ws_send_frame_async(work->hd, work->fd, &pkt);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_NO_MEM) {
        if (xSemaphoreTake(s_ws_mutex, 0) == pdTRUE) {
            ws_remove_client_locked(work->fd);
            xSemaphoreGive(s_ws_mutex);
        }
    }
    free(work->payload);
    free(work);
}

static esp_err_t ws_queue_send(httpd_handle_t hd, int fd, uint8_t type,
                               const char *payload, size_t len)
{
    ws_send_work_t *work = heap_caps_calloc(1, sizeof(*work), MALLOC_CAP_SPIRAM);
    if (!work) work = calloc(1, sizeof(*work));
    if (!work) return ESP_ERR_NO_MEM;
    if (len > 0) {
        work->payload = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (!work->payload) work->payload = malloc(len);
        if (!work->payload) { free(work); return ESP_ERR_NO_MEM; }
        memcpy(work->payload, payload, len);
    }
    work->hd = hd; work->fd = fd; work->type = type; work->len = len;
    esp_err_t err = httpd_queue_work(hd, ws_send_work, work);
    if (err != ESP_OK) {
        free(work->payload);
        free(work);
    }
    return err;
}

static esp_err_t ws_send_frame_internal(const char *payload_str, size_t len,
                                        bool skip_paused)
{
    ws_client_t clients[MAX_WS_CLIENTS];
    int count = 0;

    if (xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        count = s_ws_client_count;
        if (count > 0) {
            memcpy(clients, s_ws_clients, sizeof(ws_client_t) * count);
        }
        xSemaphoreGive(s_ws_mutex);
    }
    if (count <= 0) return ESP_OK;

    for (int i = 0; i < count; i++) {
        /* Skip paused clients for non-log messages (per-client pause). */
        if (skip_paused && clients[i].paused)
            continue;
        esp_err_t err = ws_queue_send(clients[i].hd, clients[i].fd,
                                      HTTPD_WS_TYPE_TEXT, payload_str, len);
        if (err != ESP_OK) {
            bool transient = (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NO_MEM);
            if (!transient) {
                ESP_LOGW(TAG, "ws: send failed (err=0x%x), removing fd=%d", err, clients[i].fd);
                if (xSemaphoreTake(s_ws_mutex, 0) == pdTRUE) {
                    ws_remove_client_locked(clients[i].fd);
                    xSemaphoreGive(s_ws_mutex);
                }
            }
        }
    }
    return ESP_OK;
}

esp_err_t log_stream_ws_send_json(const char *type, cJSON *data_obj)
{
    bool is_log = (strcmp(type, "log") == 0);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        if (data_obj) cJSON_Delete(data_obj);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", type);
    if (data_obj) {
        cJSON_AddItemToObject(root, "data", data_obj);
    }
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) return ESP_ERR_NO_MEM;

    esp_err_t err = ws_send_frame_internal(str, strlen(str), !is_log);
    free(str);
    return err;
}

esp_err_t log_stream_ws_send_json_raw(const char *type, const char *data_json_str)
{
    bool is_log = (strcmp(type, "log") == 0);
    size_t type_len = strlen(type);
    size_t data_len = data_json_str ? strlen(data_json_str) : 4;
    size_t total = type_len + data_len + 32;
    char *buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(total);
    if (!buf) return ESP_ERR_NO_MEM;

    int len = snprintf(buf, total, "{\"type\":\"%s\",\"data\":%s}", type, data_json_str ? data_json_str : "null");
    esp_err_t err = ws_send_frame_internal(buf, len, !is_log);
    free(buf);
    return err;
}

static void ws_forwarder_task(void *arg)
{
    (void)arg;
    char *frame_buf = heap_caps_malloc(LOG_LINE_MAX * 16, MALLOC_CAP_SPIRAM);
    if (!frame_buf) {
        ESP_LOGE(TAG, "ws_fwd: failed to allocate frame buffer");
        vTaskDelete(NULL);
        return;
    }
    size_t frame_cap = LOG_LINE_MAX * 16;
    size_t frame_pos = 0;
    const TickType_t status_interval = pdMS_TO_TICKS(3000);
    const TickType_t upload_interval = pdMS_TO_TICKS(5000);
    TickType_t last_status_tick = xTaskGetTickCount();
    TickType_t last_upload_tick = xTaskGetTickCount();
    bool hwm_logged = false;

    while (true) {
        int client_count = 0;
        if (xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            client_count = s_ws_client_count;
            xSemaphoreGive(s_ws_mutex);
        }

        /* One-shot high-water mark after the first status push to verify
         * the 12 KB PSRAM stack is sufficient for the JSON build path. */
        if (!hwm_logged && client_count > 0) {
            hwm_logged = true;
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "ws_fwd: stack high-water = %u bytes",
                     (unsigned)(hwm * sizeof(StackType_t)));
        }

        if (client_count <= 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            frame_pos = 0;
            last_status_tick = xTaskGetTickCount();
            last_upload_tick = xTaskGetTickCount();
            s_push_status_now = false;
            s_push_upload_now = false;
            s_push_ble_now    = false;
            s_push_ox_now     = false;
            continue;
        }

        /* Status push: periodic (~3 s) or on-demand (resume). */
        TickType_t now = xTaskGetTickCount();
        if (ws_has_active_client() &&
            ((now - last_status_tick) >= status_interval || s_push_status_now)) {
            last_status_tick = now;
            s_push_status_now = false;
            cJSON *st = netprov_build_status_json();
            if (st) {
                log_stream_ws_send_json("status", st);
            }
        }

        /* Upload progress push: periodic (~5 s) or on-demand (transition). */
        if (ws_has_active_client() &&
            ((now - last_upload_tick) >= upload_interval || s_push_upload_now)) {
            last_upload_tick = now;
            s_push_upload_now = false;
            char *up_json = NULL;
            if (uploader_get_progress_json(&up_json) == ESP_OK && up_json) {
                log_stream_ws_send_json_raw("upload", up_json);
                free(up_json);
            }
        }

        /* BLE state push: event-driven only (no periodic polling). */
        if (ws_has_active_client() && s_push_ble_now) {
            s_push_ble_now = false;
            cJSON *ble = cJSON_CreateObject();
            if (ble) {
                cJSON_AddStringToObject(ble, "state", as11_ble_get_status());
                cJSON_AddStringToObject(ble, "error", as11_ble_get_error());
                cJSON_AddBoolToObject(ble, "paired", as11_ble_is_paired());
                if (as11_ble_is_paired()) {
                    cJSON *info = as11_ble_get_paired_info();
                    if (info) {
                        cJSON_AddItemToObject(ble, "device", info);
                    }
                }
                log_stream_ws_send_json("ble", ble);
            }
        }

        /* Oximeter state push: event-driven only (no periodic polling). */
        if (ws_has_active_client() && s_push_ox_now) {
            s_push_ox_now = false;
            cJSON *ox = cJSON_CreateObject();
            if (ox) {
                cJSON_AddStringToObject(ox, "state", oximeter_get_status());
                cJSON_AddStringToObject(ox, "error", oximeter_get_error());
                cJSON_AddBoolToObject(ox, "paired", oximeter_is_paired());
                if (oximeter_is_paired()) {
                    cJSON *info = oximeter_get_paired_info();
                    if (info) {
                        cJSON_AddItemToObject(ox, "device", info);
                    }
                }
                log_stream_ws_send_json("oximeter", ox);
            }
        }

        /* Try to drain available ring-buffer data (non-blocking). */
        size_t item_sz = 0;
        void *item = xRingbufferReceiveUpTo(s_ringbuf, &item_sz, 0, LOG_LINE_MAX);
        if (item) {
            size_t copy_len = item_sz;
            if (frame_pos + copy_len > frame_cap - 1) {
                copy_len = frame_cap - 1 - frame_pos;
            }
            memcpy(frame_buf + frame_pos, item, copy_len);
            frame_pos += copy_len;
            vRingbufferReturnItem(s_ringbuf, item);

            if (frame_pos >= 256) {
                frame_buf[frame_pos] = '\0';
                cJSON *cjs = cJSON_CreateString(frame_buf);
                if (cjs) {
                    log_stream_ws_send_json("log", cjs);
                }
                frame_pos = 0;
            }
        } else {
            if (frame_pos > 0) {
                frame_buf[frame_pos] = '\0';
                cJSON *cjs = cJSON_CreateString(frame_buf);
                if (cjs) {
                    log_stream_ws_send_json("log", cjs);
                }
                frame_pos = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

static esp_err_t logs_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        if (xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            ws_add_client_locked(req->handle, httpd_req_to_sockfd(req));
            xSemaphoreGive(s_ws_mutex);
        }

        if (!s_ws_fwd_task) {
            s_ws_fwd_task = psram_task_create(ws_forwarder_task, "ws_fwd", 12288,
                                               NULL, 3, 0, NULL, NULL);
        }
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t buf[32] = {0};
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.payload = buf;
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    if (httpd_ws_recv_frame(req, &ws_pkt, sizeof(buf) - 1) != ESP_OK) {
        return ESP_FAIL;
    }

    int req_fd = httpd_req_to_sockfd(req);

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT && ws_pkt.len > 0) {
        buf[ws_pkt.len] = '\0';
        if (strncmp((char *)buf, "pause", 5) == 0) {
            if (xSemaphoreTake(s_ws_mutex, 0) == pdTRUE) {
                for (int i = 0; i < s_ws_client_count; i++) {
                    if (s_ws_clients[i].fd == req_fd) {
                        s_ws_clients[i].paused = true;
                        break;
                    }
                }
                xSemaphoreGive(s_ws_mutex);
            }
        } else if (strncmp((char *)buf, "resume", 6) == 0) {
            if (xSemaphoreTake(s_ws_mutex, 0) == pdTRUE) {
                for (int i = 0; i < s_ws_client_count; i++) {
                    if (s_ws_clients[i].fd == req_fd) {
                        s_ws_clients[i].paused = false;
                        break;
                    }
                }
                xSemaphoreGive(s_ws_mutex);
            }
            /* Defer the status push to the forwarder task (single-writer model). */
            s_push_status_now = true;
        }
    } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        if (xSemaphoreTake(s_ws_mutex, 0) == pdTRUE) {
            ws_remove_client_locked(req_fd);
            xSemaphoreGive(s_ws_mutex);
        }
        ESP_LOGI(TAG, "ws: client closed connection (fd=%d)", req_fd);
    }
    return ESP_OK;
}

/* ── Polling Endpoint: GET /api/logs/recent ───────────────────────── */
/* Replaces the old SSE /api/logs/stream endpoint.  Each request drains
 * available ring-buffer data and returns it as a JSON array, then closes
 * immediately — no long-lived connection, no blocking of the single httpd
 * worker task.  The frontend polls this every 1-2 seconds.
 *
 * Optional query parameter: ?since=<cursor>  — returns only lines produced
 * after the given cursor value (monotonic counter).  Omit or pass 0 to get
 * all currently-buffered lines.  Response includes the new cursor value so
 * the client can pass it on the next poll.
 *
 * Response format:
 *   {"lines":["line1","line2",...],"cursor":N}
 */

static esp_err_t logs_recent_handler(httpd_req_t *req)
{
    /* Parse optional ?since=<cursor> query parameter. */
    int since = 0;
    char query[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16] = {0};
        if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
            since = atoi(val);
            if (since < 0) since = 0;
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    /* Build the entire JSON response in a single buffer to avoid
     * hundreds of tiny chunked sends (each one a socket write that
     * generates ENOTCONN spam if the client has disconnected). */
    char *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t cap = 8192;
    size_t pos = 0;

    pos += snprintf(buf + pos, cap - pos, "{\"lines\":[");

    int local_cursor = since;
    int chunks_sent = 0;

    /* Drain available ring-buffer data, non-blocking. */
    while (true) {
        size_t item_sz = 0;
        void *item = xRingbufferReceiveUpTo(s_ringbuf, &item_sz, 0, LOG_LINE_MAX);
        if (!item) break;

        local_cursor++;

        /* Split chunk into individual lines on '\n' and emit as JSON strings. */
        const char *p   = (const char *)item;
        const char *end = p + item_sz;

        while (p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);

            /* Strip trailing CR from CRLF lines. */
            if (line_len > 0 && p[line_len - 1] == '\r') {
                line_len--;
            }

            if (line_len > 0) {
                if (chunks_sent > 0) {
                    if (pos + 1 >= cap) goto buf_full;
                    buf[pos++] = ',';
                }

                if (pos + 1 >= cap) goto buf_full;
                buf[pos++] = '"';

                /* Escape JSON-special characters. */
                for (size_t i = 0; i < line_len; i++) {
                    char c = p[i];
                    if (c == '\\' || c == '"') {
                        if (pos + 2 >= cap) goto buf_full;
                        buf[pos++] = '\\';
                        buf[pos++] = c;
                    } else if ((unsigned char)c < 0x20) {
                        if (pos + 6 >= cap) goto buf_full;
                        pos += snprintf(buf + pos, cap - pos, "\\u%04x", (unsigned char)c);
                    } else {
                        if (pos + 1 >= cap) goto buf_full;
                        buf[pos++] = c;
                    }
                }

                if (pos + 1 >= cap) goto buf_full;
                buf[pos++] = '"';
                chunks_sent++;
            }

            p = nl ? nl + 1 : end;
        }

        vRingbufferReturnItem(s_ringbuf, item);
    }

buf_full:
    pos += snprintf(buf + pos, cap - pos, "],\"cursor\":%d}", local_cursor);

    httpd_resp_send(req, buf, pos);
    free(buf);
    return ESP_OK;
}

/* ── History Endpoint: GET /api/logs/history ─────────────────────── */

static esp_err_t logs_history_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    bool got_any = false;

    /* Read log files from oldest (.2) to newest (.0). */
    if (s_sd_ready || sd_storage_is_ready()) {
        for (int i = LOG_MAX_FILES - 1; i >= 0; i--) {
            char path[64];
            snprintf(path, sizeof(path), "%s/%s%d", LOG_DIR, LOG_FILE_PREFIX, i);
            FILE *f = fopen(path, "rb");
            if (!f) continue;

            char chunk[1024];
            size_t n;
            while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
                if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) {
                    fclose(f);
                    goto hist_done;
                }
                got_any = true;
            }
            fclose(f);
        }
    }

    /* Also include current write buffer contents (not yet flushed to SD). */
    if (s_writebuf && s_writebuf_mutex) {
        xSemaphoreTake(s_writebuf_mutex, portMAX_DELAY);
        size_t pending = s_writebuf_head - s_writebuf_tail;
        if (pending > 0) {
            uint8_t *tmp = heap_caps_malloc(pending, MALLOC_CAP_SPIRAM);
            if (!tmp) tmp = malloc(pending);
            if (tmp) {
                size_t pos = s_writebuf_tail % WRITEBUF_SIZE;
                size_t chunk1 = WRITEBUF_SIZE - pos;
                if (pending <= chunk1) {
                    memcpy(tmp, s_writebuf + pos, pending);
                } else {
                    memcpy(tmp, s_writebuf + pos, chunk1);
                    memcpy(tmp + chunk1, s_writebuf, pending - chunk1);
                }
                httpd_resp_send_chunk(req, (const char *)tmp, (ssize_t)pending);
                free(tmp);
                got_any = true;
            }
        }
        xSemaphoreGive(s_writebuf_mutex);
    }

    if (!got_any) {
        httpd_resp_send_chunk(req, "(no log history available)\n", -1);
    }

hist_done:
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── Download Endpoint: GET /api/logs/download ────────────────────── */

static esp_err_t logs_download_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"somnotrace-logs.txt\"");

    bool got_any = false;

    /* Include SD history files (oldest to newest). */
    if (s_sd_ready || sd_storage_is_ready()) {
        for (int i = LOG_MAX_FILES - 1; i >= 0; i--) {
            char path[64];
            snprintf(path, sizeof(path), "%s/%s%d", LOG_DIR, LOG_FILE_PREFIX, i);
            FILE *f = fopen(path, "rb");
            if (!f) continue;

            char chunk[1024];
            size_t n;
            while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
                if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) {
                    fclose(f);
                    goto dl_done;
                }
                got_any = true;
            }
            fclose(f);
        }
    }

    /* Include current write buffer (not yet flushed). */
    if (s_writebuf && s_writebuf_mutex) {
        xSemaphoreTake(s_writebuf_mutex, portMAX_DELAY);
        size_t pending = s_writebuf_head - s_writebuf_tail;
        if (pending > 0) {
            uint8_t *tmp = heap_caps_malloc(pending, MALLOC_CAP_SPIRAM);
            if (!tmp) tmp = malloc(pending);
            if (tmp) {
                size_t pos = s_writebuf_tail % WRITEBUF_SIZE;
                size_t chunk1 = WRITEBUF_SIZE - pos;
                if (pending <= chunk1) {
                    memcpy(tmp, s_writebuf + pos, pending);
                } else {
                    memcpy(tmp, s_writebuf + pos, chunk1);
                    memcpy(tmp + chunk1, s_writebuf, pending - chunk1);
                }
                httpd_resp_send_chunk(req, (const char *)tmp, (ssize_t)pending);
                free(tmp);
                got_any = true;
            }
        }
        xSemaphoreGive(s_writebuf_mutex);
    }

    if (!got_any) {
        httpd_resp_send_chunk(req, "(no log data available)\n", -1);
    }

dl_done:
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── Log Level Endpoint: POST /api/logs/level ─────────────────────── */

static const char *level_to_str(esp_log_level_t level)
{
    switch (level) {
        case ESP_LOG_NONE:    return "none";
        case ESP_LOG_ERROR:   return "error";
        case ESP_LOG_WARN:    return "warn";
        case ESP_LOG_INFO:    return "info";
        case ESP_LOG_DEBUG:   return "debug";
        case ESP_LOG_VERBOSE: return "verbose";
        default:              return "unknown";
    }
}

static esp_log_level_t str_to_level(const char *s)
{
    if (strcmp(s, "error")   == 0) return ESP_LOG_ERROR;
    if (strcmp(s, "warn")    == 0) return ESP_LOG_WARN;
    if (strcmp(s, "info")    == 0) return ESP_LOG_INFO;
    if (strcmp(s, "debug")   == 0) return ESP_LOG_DEBUG;
    if (strcmp(s, "verbose") == 0) return ESP_LOG_VERBOSE;
    if (strcmp(s, "none")    == 0) return ESP_LOG_NONE;
    return (esp_log_level_t)-1;
}

/* Cached current global level so we can report it back to the UI.
 * ESP-IDF doesn't expose a getter for the global default. */
static esp_log_level_t s_current_level = ESP_LOG_INFO;

static esp_err_t logs_level_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Return current level as JSON. */
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"level\":\"%s\"}", level_to_str(s_current_level));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, resp);
        return ESP_OK;
    }

    /* POST — set the global log level. */
    char body[64];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *lvl_item = cJSON_GetObjectItem(json, "level");
    if (!cJSON_IsString(lvl_item)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'level' string");
        return ESP_FAIL;
    }

    esp_log_level_t new_level = str_to_level(lvl_item->valuestring);
    cJSON_Delete(json);

    if ((int)new_level < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "invalid level — use: none, error, warn, info, debug, verbose");
        return ESP_FAIL;
    }

    /* Apply globally (wildcard "*" sets the default for all tags). */
    esp_log_level_set("*", new_level);
    s_current_level = new_level;

    ESP_LOGI(TAG, "global log level changed to %s", level_to_str(new_level));

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"level\":\"%s\"}", level_to_str(new_level));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* ── Handler Registration ─────────────────────────────────────────── */

void log_stream_register_handlers(httpd_handle_t server)
{
    httpd_uri_t recent = {
        .uri     = "/api/logs/recent",
        .method  = HTTP_GET,
        .handler = logs_recent_handler,
    };
    httpd_register_uri_handler(server, &recent);

    httpd_uri_t download = {
        .uri     = "/api/logs/download",
        .method  = HTTP_GET,
        .handler = logs_download_handler,
    };
    httpd_register_uri_handler(server, &download);

    httpd_uri_t level_get = {
        .uri     = "/api/logs/level",
        .method  = HTTP_GET,
        .handler = logs_level_handler,
    };
    httpd_register_uri_handler(server, &level_get);

    httpd_uri_t level_post = {
        .uri     = "/api/logs/level",
        .method  = HTTP_POST,
        .handler = logs_level_handler,
    };
    httpd_register_uri_handler(server, &level_post);

    httpd_uri_t history = {
        .uri     = "/api/logs/history",
        .method  = HTTP_GET,
        .handler = logs_history_handler,
    };
    httpd_register_uri_handler(server, &history);

    httpd_uri_t ws = {
        .uri        = "/api/ws",
        .method     = HTTP_GET,
        .handler    = logs_ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(server, &ws);

    ESP_LOGI(TAG, "registered /api/ws and /api/logs/{recent,download,history,level}");
}
