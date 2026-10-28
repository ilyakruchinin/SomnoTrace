/*
 * SomnoTrace - SomnoBridge BLE client (Phase 2 of IMPORT-EDFs.md)
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

/* Client side of somnopal/docs/implementation-spec.md §10.  Verified against
 * the reference client (somnopal/scripts/ble_query.py) and a live GEEK:
 *
 *   service 8ba7d100-6e4b-4bb4-9f1d-7306766e6f50
 *   control 8ba7d101-… (write-with-response + indicate)
 *   data    8ba7d102-… (notify)
 *
 * Every characteristic value is a 16-byte LE header
 *   {u8 ver=1, u8 kind, u16 flags(START=1,END=2), u32 request_id, u64 offset}
 * followed by the payload.  JSON is fragmented START→END with contiguous
 * offsets; streams are byte ranges on Data with ACK credit windows.
 *
 * SomnoTrace is the sole persistent sync authority: the bridge's catalog is
 * volatile, bridge_ledger.json decides what is re-pulled, and staged files
 * land in /somnotrace/IMPORT/ for the Phase-1 importer. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"

#include "mbedtls/sha256.h"
#include "miniz.h"
#include "nvs.h"

#include "cJSON.h"
#include "edf_import.h"
#include "nvs_writer.h"
#include "psram_task.h"
#include "sb_ble.h"
#include "sb_proto.h"
#include "sd_storage.h"
#include "time_sync.h"

static const char *TAG = "sb_ble";

/* ── Protocol constants ─────────────────────────────────────────────── */

#define SB_SVC_UUID_STR "8ba7d100-6e4b-4bb4-9f1d-7306766e6f50"

static const ble_uuid128_t UUID_SVC =
    BLE_UUID128_INIT(0x50, 0x6f, 0x6e, 0x76, 0x06, 0x73, 0x1d, 0x9f,
                     0xb4, 0x4b, 0x4b, 0x6e, 0x00, 0xd1, 0xa7, 0x8b);
static const ble_uuid128_t UUID_CTRL =
    BLE_UUID128_INIT(0x50, 0x6f, 0x6e, 0x76, 0x06, 0x73, 0x1d, 0x9f,
                     0xb4, 0x4b, 0x4b, 0x6e, 0x01, 0xd1, 0xa7, 0x8b);
static const ble_uuid128_t UUID_DATA =
    BLE_UUID128_INIT(0x50, 0x6f, 0x6e, 0x76, 0x06, 0x73, 0x1d, 0x9f,
                     0xb4, 0x4b, 0x4b, 0x6e, 0x02, 0xd1, 0xa7, 0x8b);

#define SB_VER            1
#define SB_KIND_JSON_REQ  1
#define SB_KIND_JSON_RSP  2
#define SB_KIND_FILE      3
#define SB_KIND_ACK       4
#define SB_KIND_ABORT     5
#define SB_KIND_TIME_REQ  6
#define SB_KIND_TIME_RSP  7
#define SB_KIND_TIME_CMT  8
#define SB_KIND_EVIDENCE  9
#define SB_KIND_STREAM2   10

#define SB_FLAG_START     0x0001
#define SB_FLAG_END       0x0002

#define SB_HDR_LEN        16
#define SB_JSON_MAX       (8 * 1024)      /* GET_DRIFT can reach ~4-8 KiB   */
#define SB_WINDOW         16384
#define SB_ACK_EVERY      2880
#define SB_JSON_TIMEOUT   (20 * 1000)
#define SB_STREAM_TIMEOUT (120 * 1000)
#define SB_TIME_ROUNDS    5
#define SB_TIME_PERIOD_MS (10 * 60 * 1000)
#define SB_POLL_MS        (30 * 1000)
#define SB_IDLE_SYNC_MS   (5 * 60 * 1000)
#define SB_PIN_BUDGET_MS  90000   /* > the bridge's 60 s attempt limit */
#define SB_SYNC_DAYS      14
#define SB_SYNC_DAYS_FULL 365

#define SB_NVS_NS         "sbble"
#define SB_NVS_ADDR       "addr"
#define SB_LEDGER_PATH    SD_SESSIONS_DIR "/bridge_ledger.json"

/* ── State ──────────────────────────────────────────────────────────── */

static bool s_available;
static bool s_paired;                 /* bonded peer known                */
static char s_state[24] = SB_STATUS_IDLE;
static char s_error[96];
static ble_addr_t s_peer;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;
static uint16_t s_mtu = 23;

static uint16_t s_ctrl_val, s_ctrl_cccd, s_data_val, s_data_cccd;
static uint16_t s_svc_start, s_svc_end;
static bool s_disc_done;

static SemaphoreHandle_t s_req_mtx;   /* one JSON exchange at a time      */
static SemaphoreHandle_t s_resp_sem;  /* completion of the current one    */
static SemaphoreHandle_t s_write_mtx; /* one control write at a time      */
static SemaphoreHandle_t s_write_done;
static SemaphoreHandle_t s_pin_sem;
static SemaphoreHandle_t s_sync_mtx;
/* PIN hand-off: the HTTP handler must not call ble_sm_inject_io() itself —
 * that path runs the SM procedure and overflowed the httpd task's PSRAM
 * stack (the device panicked on Confirm).  Queue it to the bridge task. */
static SemaphoreHandle_t s_pin_mbox;
static char s_pin_mbox_val[8];
static volatile bool s_pin_pending;   /* PIN injected, SM still running */

static uint32_t s_rid;                /* next request id                  */
static uint32_t s_pending_rid;        /* rid the response semaphore waits */
static char    *s_resp_json;          /* completed JSON body (heap)       */
static uint8_t *s_rx_buf;             /* indication reassembly (PSRAM)    */
static size_t   s_rx_len;
static uint32_t s_rx_rid;
static uint64_t s_rx_next_off;

/* Time exchange */
static bool     s_time_pending;
static uint32_t s_time_rid;
static int64_t  s_time_t1_us;
static uint32_t s_time_epoch;
static int64_t  s_time_best_rtt;
static uint64_t s_time_best_t1, s_time_best_t4, s_time_best_m2, s_time_best_m3;
static uint32_t s_time_best_epoch;
static int      s_time_rounds;
static int64_t  s_time_last_ms;

/* File stream */
static bool     s_stream_active;
static uint32_t s_stream_rid;
static FILE    *s_stream_f;
static char     s_stream_path[192];
static char     s_stream_tmp[200];
static int64_t  s_stream_expected;
static uint64_t s_stream_got;         /* decoded bytes written            */
static uint64_t s_stream_acked;
static uint64_t s_stream_wire;        /* wire bytes since last ACK        */
static mbedtls_sha256_context s_sha;
static bool     s_sha_active;
static char     s_stream_sha[72];
static uint8_t *s_v2_buf;             /* compact-frame accumulator        */
static size_t   s_v2_len;
static uint16_t s_v2_seq;
static int      s_v2_seq_errors;

/* Telemetry (read by the HTTP layer) */
static bool     s_therapy_active;
static bool     s_transfer_allowed;
static char     s_last_error[64];
static int64_t  s_last_sync_ms;
static int      s_sync_days_done, s_sync_files_done;
static bool     s_sync_running;
static bool     s_sync_full;
static sb_import_kick_fn_t s_import_kick;

/* Ledger (loaded during a sync pass, freed at the end) */
static cJSON   *s_ledger;

static int sb_gap_event(struct ble_gap_event *event, void *arg);

static const char *state_str(void) { return s_state; }

static void set_state(const char *st)
{
    if (strcmp(s_state, st) == 0) return;
    snprintf(s_state, sizeof(s_state), "%s", st);
    ESP_LOGI(TAG, "state -> %s", st);
}

static void set_error(const char *msg)
{
    snprintf(s_error, sizeof(s_error), "%s", msg ? msg : "");
    snprintf(s_last_error, sizeof(s_last_error), "%s", msg ? msg : "");
    set_state(SB_STATUS_ERROR);
    ESP_LOGW(TAG, "%s", s_error);
}

/* ── NVS: bonded peer ───────────────────────────────────────────────── */

typedef struct { ble_addr_t *addr; bool save; } peer_nvs_arg_t;

static esp_err_t peer_nvs_op(void *arg)
{
    peer_nvs_arg_t *a = arg;
    nvs_handle_t h;
    esp_err_t err = nvs_open(SB_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (a->save) {
        err = nvs_set_blob(h, SB_NVS_ADDR, a->addr, sizeof(*a->addr));
        if (err == ESP_OK) err = nvs_commit(h);
    } else {
        size_t n = sizeof(*a->addr);
        err = nvs_get_blob(h, SB_NVS_ADDR, a->addr, &n);
        if (err == ESP_OK && n != sizeof(*a->addr)) err = ESP_ERR_INVALID_SIZE;
    }
    nvs_close(h);
    return err;
}

static void peer_save(const ble_addr_t *addr)
{
    peer_nvs_arg_t a = { .addr = (ble_addr_t *)addr, .save = true };
    if (nvs_writer_run(peer_nvs_op, &a) != ESP_OK)
        ESP_LOGW(TAG, "could not persist the bonded peer");
}

static bool peer_load(ble_addr_t *out)
{
    peer_nvs_arg_t a = { .addr = out, .save = false };
    return nvs_writer_run(peer_nvs_op, &a) == ESP_OK;
}

/* ── Framing helpers ────────────────────────────────────────────────── */

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static int write_cb(uint16_t conn, const struct ble_gatt_error *err,
                    struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (err && err->status != 0) ESP_LOGW(TAG, "control write status=%d", err->status);
    xSemaphoreGive(s_write_done);
    return 0;
}

/* One control-characteristic write (with response), serialized. */
static esp_err_t ctrl_write(const uint8_t *data, size_t len, bool rsp)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_ctrl_val)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_write_mtx, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
    if (rsp) {
        xSemaphoreTake(s_write_done, 0);
        int rc = ble_gattc_write_flat(s_conn, s_ctrl_val, data, len, write_cb, NULL);
        if (rc != 0) ret = ESP_FAIL;
        else if (xSemaphoreTake(s_write_done, pdMS_TO_TICKS(5000)) != pdTRUE)
            ret = ESP_ERR_TIMEOUT;
    } else {
        int rc = ble_gattc_write_no_rsp_flat(s_conn, s_ctrl_val, data, len);
        if (rc != 0) ret = ESP_FAIL;
    }
    xSemaphoreGive(s_write_mtx);
    return ret;
}

/* Send a logical control message, fragmented to the negotiated MTU. */
static esp_err_t ctrl_send(uint8_t kind, uint32_t rid, const uint8_t *payload,
                           size_t len)
{
    const size_t chunk = (s_mtu > SB_HDR_LEN + 3) ? (s_mtu - 3 - SB_HDR_LEN) : 20;
    uint8_t *buf = malloc(SB_HDR_LEN + chunk);
    if (!buf) return ESP_ERR_NO_MEM;
    esp_err_t ret = ESP_OK;
    size_t off = 0;
    do {
        size_t n = len - off;
        if (n > chunk) n = chunk;
        uint16_t flags = 0;
        if (off == 0) flags |= SB_FLAG_START;
        if (off + n == len) flags |= SB_FLAG_END;
        sb_hdr_pack(buf, kind, flags, rid, off);
        if (n) memcpy(buf + SB_HDR_LEN, payload + off, n);
        /* The last fragment carries END and is the one we wait on; every
         * fragment uses write-with-response so the bridge never overruns. */
        if ((ret = ctrl_write(buf, SB_HDR_LEN + n, true)) != ESP_OK) break;
        off += n;
    } while (off < len || (len == 0 && off == 0));
    free(buf);
    return ret;
}

/* ── JSON request / response ────────────────────────────────────────── */

static void json_reset_rx(void)
{
    s_rx_len = 0;
    s_rx_rid = 0;
    s_rx_next_off = 0;
}

/* A complete JSON body arrived: hand it to the waiter. */
static void json_complete(uint32_t rid, const char *body)
{
    if (rid == s_pending_rid) {
        free(s_resp_json);
        s_resp_json = strdup(body);
        xSemaphoreGive(s_resp_sem);
    }
    json_reset_rx();
}

static esp_err_t json_request(const char *op, cJSON *args, uint32_t *out_rid,
                              cJSON **out_result, cJSON **out_error,
                              uint32_t timeout_ms)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_req_mtx, portMAX_DELAY);

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "op", op);
    if (args) cJSON_AddItemToObject(req, "args", args);
    else cJSON_AddObjectToObject(req, "args");
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) { xSemaphoreGive(s_req_mtx); return ESP_ERR_NO_MEM; }

    const uint32_t rid = ++s_rid;
    s_pending_rid = rid;
    free(s_resp_json);
    s_resp_json = NULL;
    xSemaphoreTake(s_resp_sem, 0);
    json_reset_rx();

    esp_err_t ret = ctrl_send(SB_KIND_JSON_REQ, rid, (const uint8_t *)body,
                              strlen(body));
    free(body);
    if (ret != ESP_OK) { s_pending_rid = 0; xSemaphoreGive(s_req_mtx); return ret; }

    if (xSemaphoreTake(s_resp_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "%s: no response in %u ms", op, (unsigned)timeout_ms);
        s_pending_rid = 0;
        xSemaphoreGive(s_req_mtx);
        return ESP_ERR_TIMEOUT;
    }

    cJSON *root = s_resp_json ? cJSON_Parse(s_resp_json) : NULL;
    free(s_resp_json);
    s_resp_json = NULL;
    s_pending_rid = 0;
    xSemaphoreGive(s_req_mtx);

    if (!root) return ESP_ERR_INVALID_RESPONSE;

    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    cJSON *err = cJSON_GetObjectItem(root, "error");
    cJSON *res = cJSON_GetObjectItem(root, "result");
    if (cJSON_IsTrue(ok)) {
        if (out_result) *out_result = cJSON_Duplicate(res ? res : cJSON_CreateObject(), 1);
        if (out_error) *out_error = NULL;
        ret = ESP_OK;
    } else {
        if (out_error) *out_error = err ? cJSON_Duplicate(err, 1) : NULL;
        if (out_result) *out_result = NULL;
        ret = ESP_FAIL;
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(err, "code"));
        ESP_LOGW(TAG, "%s: error %s", op, code ? code : "?");
    }
    if (out_rid) *out_rid = rid;
    cJSON_Delete(root);
    return ret;
}

/* Error code of the last failed json_request(). */
static char s_last_code[32];

static esp_err_t json_op(const char *op, cJSON *args, cJSON **result)
{
    cJSON *err = NULL;
    esp_err_t ret = json_request(op, args, NULL, result, &err, SB_JSON_TIMEOUT);
    s_last_code[0] = '\0';
    if (err) {
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(err, "code"));
        if (code) snprintf(s_last_code, sizeof(s_last_code), "%s", code);
        cJSON_Delete(err);
    }
    return ret;
}

/* ── Time exchange (§10.4) ──────────────────────────────────────────── */

static void time_on_reply(const uint8_t *p, size_t len)
{
    if (len < 8 + 8 + 8 + 4) return;
    const uint64_t t1 = rd_u64(p);
    const uint64_t m2 = rd_u64(p + 8);
    const uint64_t m3 = rd_u64(p + 16);
    const uint32_t epoch = rd_u32(p + 24);
    const int64_t t4 = esp_timer_get_time();
    if (t1 != (uint64_t)s_time_t1_us || epoch != s_time_epoch) return;

    const int64_t rtt = (t4 - (int64_t)t1) - ((int64_t)m3 - (int64_t)m2);
    if (rtt < 0) return;
    if (s_time_best_rtt < 0 || rtt < s_time_best_rtt) {
        s_time_best_rtt = rtt;
        s_time_best_t1 = t1;
        s_time_best_t4 = (uint64_t)t4;
        s_time_best_m2 = m2;
        s_time_best_m3 = m3;
        s_time_best_epoch = epoch;
    }
    s_time_pending = false;
    xSemaphoreGive(s_resp_sem);      /* reuse the waiter: round finished */
}

/* One TIME_REQUEST round; returns the RTT in µs or -1. */
static int64_t time_round(void)
{
    if (!time_sync_is_synced()) return -1;

    uint8_t pkt[8 + 4 + 4];
    const int64_t t1 = esp_timer_get_time();
    const uint32_t epoch = (uint32_t)(time(NULL) & 0xFFFFFFFFu);
    wr_u64(pkt, (uint64_t)t1);
    wr_u32(pkt + 8, 25000);          /* source uncertainty: 25 ms */
    wr_u32(pkt + 12, epoch);

    const uint32_t rid = ++s_rid;
    s_time_rid = rid;
    s_time_t1_us = t1;
    s_time_epoch = epoch;
    s_time_pending = true;
    s_time_best_rtt = -1;
    xSemaphoreTake(s_resp_sem, 0);

    if (ctrl_send(SB_KIND_TIME_REQ, rid, pkt, sizeof(pkt)) != ESP_OK) return -1;
    if (xSemaphoreTake(s_resp_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        s_time_pending = false;
        return -1;
    }
    return s_time_best_rtt;
}

static void time_sync_rounds(void)
{
    s_time_rounds = 0;
    for (int i = 0; i < SB_TIME_ROUNDS; i++) {
        if (time_round() < 0) break;
        s_time_rounds++;
    }
    if (s_time_best_rtt < 0) {
        ESP_LOGW(TAG, "time sync: no usable round");
        return;
    }

    uint8_t pkt[8 + 8 + 4];
    wr_u64(pkt, s_time_best_t1);
    wr_u64(pkt + 8, s_time_best_t4);
    wr_u32(pkt + 16, s_time_best_epoch);
    if (ctrl_send(SB_KIND_TIME_CMT, s_time_rid, pkt, sizeof(pkt)) == ESP_OK) {
        ESP_LOGI(TAG, "time sync: %d round(s), best RTT %lld us committed",
                 s_time_rounds, (long long)s_time_best_rtt);
        s_time_last_ms = esp_timer_get_time() / 1000;
    }
}

/* ── File streaming (§10.3) ─────────────────────────────────────────── */

static void stream_ack(uint64_t off, uint32_t window)
{
    uint8_t pkt[4];
    wr_u32(pkt, window);
    if (ctrl_send(SB_KIND_ACK, s_stream_rid, pkt, sizeof(pkt)) == ESP_OK)
        s_stream_acked = off;
}

static void stream_fail(const char *why)
{
    if (s_stream_f) { fclose(s_stream_f); s_stream_f = NULL; }
    if (s_stream_tmp[0]) unlink(s_stream_tmp);
    s_stream_tmp[0] = '\0';
    s_stream_active = false;
    if (s_sha_active) { mbedtls_sha256_free(&s_sha); s_sha_active = false; }
    snprintf(s_last_error, sizeof(s_last_error), "%s", why ? why : "stream failed");
    ESP_LOGW(TAG, "stream %s: %s", s_stream_path, why ? why : "failed");
}

static esp_err_t stream_begin(const char *directory, const char *name,
                              const char *expected_size)
{
    char day[16];
    if (!sb_stage_path(directory, name, s_stream_path, sizeof(s_stream_path),
                       day, sizeof(day)))
        return ESP_ERR_INVALID_ARG;
    snprintf(s_stream_tmp, sizeof(s_stream_tmp), "%s.part", s_stream_path);

    /* Create the parents the target needs (IMPORT/, /DATALOG[/<day>], /SETTINGS). */
    char mk[200];
    mkdir(SD_IMPORT_DIR, 0775);
    snprintf(mk, sizeof(mk), SD_IMPORT_DIR "/DATALOG");
    mkdir(mk, 0775);
    if (day[0]) {
        snprintf(mk, sizeof(mk), SD_IMPORT_DIR "/DATALOG/%s", day);
        mkdir(mk, 0775);
    }
    snprintf(mk, sizeof(mk), SD_IMPORT_DIR "/SETTINGS");
    mkdir(mk, 0775);

    s_stream_f = fopen(s_stream_tmp, "wb");
    if (!s_stream_f) { s_stream_tmp[0] = '\0'; return ESP_FAIL; }
    s_stream_got = 0;
    s_stream_acked = 0;
    s_stream_wire = 0;
    s_v2_len = 0;
    s_v2_seq = 0;
    s_v2_seq_errors = 0;
    s_stream_expected = expected_size ? strtoll(expected_size, NULL, 10) : -1;
    mbedtls_sha256_init(&s_sha);
    mbedtls_sha256_starts(&s_sha, 0);
    s_sha_active = true;
    s_stream_active = true;
    return ESP_OK;
}

/* Decoded raw bytes → file + hash. */
static bool stream_write(const uint8_t *p, size_t n)
{
    if (!s_stream_f || !n) return true;
    if (fwrite(p, 1, n, s_stream_f) != n) return false;
    mbedtls_sha256_update(&s_sha, p, n);
    s_stream_got += n;
    return true;
}

/* v2 block stream: 12-byte self-describing headers, codecs 0/1/2. */
static bool stream_v2_decode(const uint8_t *p, size_t n)
{
    if (!s_v2_buf) s_v2_buf = heap_caps_malloc(96 * 1024, MALLOC_CAP_SPIRAM);
    if (!s_v2_buf) return false;
    if (s_v2_len + n > 96 * 1024) { s_v2_seq_errors++; s_v2_len = 0; return false; }
    memcpy(s_v2_buf + s_v2_len, p, n);
    s_v2_len += n;

    size_t pos = 0;
    while (s_v2_len - pos >= 12) {
        const uint8_t codec = s_v2_buf[pos];
        const uint32_t raw_len = rd_u32(s_v2_buf + pos + 4);
        const uint32_t enc_len = rd_u32(s_v2_buf + pos + 8);
        if (raw_len > 64 * 1024 || enc_len > 64 * 1024) return false;
        if (s_v2_len - pos < 12 + enc_len) break;      /* wait for more */

        uint8_t *blk = heap_caps_malloc(raw_len ? raw_len : 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!blk) return false;
        bool ok = sb_v2_block(codec, s_v2_buf + pos + 12, enc_len, blk, raw_len);
        if (ok) ok = stream_write(blk, raw_len);
        free(blk);
        if (!ok) return false;
        pos += 12 + enc_len;
    }
    if (pos) {
        memmove(s_v2_buf, s_v2_buf + pos, s_v2_len - pos);
        s_v2_len -= pos;
    }
    return true;
}

static void stream_data(const uint8_t *data, size_t len, uint8_t kind)
{
    if (!s_stream_active) return;
    const uint8_t *payload = data;
    size_t plen = len;

    if (kind == SB_KIND_STREAM2) {
        if (plen < 4) return;
        const uint16_t seq = rd_u16(payload + 2);
        if (seq < s_v2_seq) return;                    /* duplicate delivery */
        if (seq != s_v2_seq) s_v2_seq_errors++;
        s_v2_seq = (uint16_t)(seq + 1);
        payload += 4;
        plen -= 4;
        if (!stream_v2_decode(payload, plen)) { stream_fail("v2 decode"); return; }
    } else {
        if (!stream_write(payload, plen)) { stream_fail("write"); return; }
    }

    s_stream_wire += len;
    if (s_stream_wire >= SB_ACK_EVERY) {
        s_stream_wire = 0;
        stream_ack(s_stream_got, SB_WINDOW);
    }
}

/* STREAM_END: verify everything the bridge promises before publishing. */
static bool stream_end(cJSON *result)
{
    bool ok = false;
    const char *sha = cJSON_GetStringValue(cJSON_GetObjectItem(result, "sha256"));
    const char *actual = cJSON_GetStringValue(cJSON_GetObjectItem(result, "actual_bytes"));
    const bool t_ok = cJSON_IsTrue(cJSON_GetObjectItem(result, "ok"));
    const bool allow = cJSON_IsTrue(cJSON_GetObjectItem(result, "completion_allowed"));
    const bool unchanged = cJSON_IsTrue(cJSON_GetObjectItem(result, "source_unchanged"));
    const bool checked = cJSON_IsTrue(cJSON_GetObjectItem(result, "monitoring_checked"));
    const int64_t actual_n = actual ? strtoll(actual, NULL, 10) : -1;

    if (s_stream_f) { fclose(s_stream_f); s_stream_f = NULL; }
    if (s_sha_active) {
        uint8_t dig[32];
        mbedtls_sha256_finish(&s_sha, dig);
        mbedtls_sha256_free(&s_sha);
        s_sha_active = false;
        for (int i = 0; i < 32; i++)
            snprintf(s_stream_sha + i * 2, 3, "%02x", dig[i]);
        s_stream_sha[64] = '\0';
    }

    if (!t_ok) snprintf(s_last_error, sizeof(s_last_error), "stream not ok");
    else if (!allow) snprintf(s_last_error, sizeof(s_last_error), "completion not allowed");
    else if (!unchanged) snprintf(s_last_error, sizeof(s_last_error), "source changed");
    else if (!checked) snprintf(s_last_error, sizeof(s_last_error), "monitoring not checked");
    else if (actual_n >= 0 && (uint64_t)actual_n != s_stream_got)
        snprintf(s_last_error, sizeof(s_last_error), "size mismatch");
    else if (s_stream_expected > 0 && s_stream_got != (uint64_t)s_stream_expected)
        snprintf(s_last_error, sizeof(s_last_error), "expected size mismatch");
    else if (s_v2_seq_errors) snprintf(s_last_error, sizeof(s_last_error), "stream seq errors");
    else if (sha && strcmp(sha, s_stream_sha) != 0)
        snprintf(s_last_error, sizeof(s_last_error), "sha256 mismatch");
    else if (s_stream_tmp[0] && rename(s_stream_tmp, s_stream_path) == 0) ok = true;
    else snprintf(s_last_error, sizeof(s_last_error), "publish failed");

    if (!ok && s_stream_tmp[0]) unlink(s_stream_tmp);
    s_stream_tmp[0] = '\0';
    s_stream_active = false;
    return ok;
}

/* GET_FILE with v2 framing, retried on the bridge's transient errors. */
static esp_err_t sb_get_file(const char *day, const char *name,
                             const char *expected_size)
{
    /* The bridge canonicalises "A:\DATALOG\<day>" (ezp_logical_to_canon
     * requires the drive prefix; a bare "/DATALOG/…" is PATH_INVALID). */
    char dir[64];
    snprintf(dir, sizeof(dir), "A:\\DATALOG\\%s", day);

    for (int attempt = 0; attempt < 3; attempt++) {
        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "directory", dir);
        cJSON_AddStringToObject(args, "name", name);
        if (expected_size && expected_size[0])
            cJSON_AddStringToObject(args, "expected_size", expected_size);
        cJSON_AddBoolToObject(args, "v2", true);

        /* Stream responses reuse the request id: BEGIN then END. */
        const uint32_t rid = s_rid + 1;
        if (stream_begin(dir, name, expected_size) != ESP_OK) {
            cJSON_Delete(args);
            return ESP_FAIL;
        }
        s_stream_rid = rid;

        xSemaphoreTake(s_req_mtx, portMAX_DELAY);
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "op", "GET_FILE");
        cJSON_AddItemToObject(req, "args", args);
        char *body = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        if (!body) { xSemaphoreGive(s_req_mtx); stream_fail("no mem"); return ESP_ERR_NO_MEM; }

        s_pending_rid = rid;
        s_rid = rid;
        free(s_resp_json);
        s_resp_json = NULL;
        xSemaphoreTake(s_resp_sem, 0);
        json_reset_rx();
        esp_err_t ret = ctrl_send(SB_KIND_JSON_REQ, rid, (const uint8_t *)body,
                                  strlen(body));
        free(body);
        if (ret != ESP_OK) { s_pending_rid = 0; xSemaphoreGive(s_req_mtx); stream_fail("send"); return ret; }

        /* BEGIN is acknowledged by the first response for this rid. */
        if (xSemaphoreTake(s_resp_sem, pdMS_TO_TICKS(SB_JSON_TIMEOUT)) != pdTRUE) {
            s_pending_rid = 0;
            xSemaphoreGive(s_req_mtx);
            stream_fail("no STREAM_BEGIN");
            return ESP_ERR_TIMEOUT;
        }
        free(s_resp_json);
        s_resp_json = NULL;
        stream_ack(0, SB_WINDOW);      /* grant the initial credit window */
        xSemaphoreGive(s_req_mtx);

        /* END arrives on the same rid once the body is complete. */
        if (xSemaphoreTake(s_resp_sem, pdMS_TO_TICKS(SB_STREAM_TIMEOUT)) != pdTRUE) {
            stream_fail("no STREAM_END");
            s_pending_rid = 0;
            return ESP_ERR_TIMEOUT;
        }
        cJSON *root = s_resp_json ? cJSON_Parse(s_resp_json) : NULL;
        free(s_resp_json);
        s_resp_json = NULL;
        s_pending_rid = 0;
        if (!root) { stream_fail("bad STREAM_END"); return ESP_ERR_INVALID_RESPONSE; }

        cJSON *res = cJSON_GetObjectItem(root, "result");
        cJSON *err = cJSON_GetObjectItem(root, "error");
        bool ok = false;
        if (cJSON_IsTrue(cJSON_GetObjectItem(root, "ok")) && res)
            ok = stream_end(res);
        else {
            const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(err, "code"));
            snprintf(s_last_error, sizeof(s_last_error), "%s", code ? code : "error");
            stream_fail(code ? code : "error");
        }
        cJSON_Delete(root);
        if (ok) return ESP_OK;

        /* Transient bridge states: STALE (freshness guard) and BUSY. */
        if (strcmp(s_last_error, "STALE") == 0) { vTaskDelay(pdMS_TO_TICKS(12000)); continue; }
        if (strcmp(s_last_error, "BUSY") == 0)  { vTaskDelay(pdMS_TO_TICKS(1500)); continue; }
        return ESP_FAIL;
    }
    return ESP_FAIL;
}

/* ── Ledger ─────────────────────────────────────────────────────────── */

static void ledger_load(void)
{
    if (s_ledger) return;
    FILE *f = fopen(SB_LEDGER_PATH, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *b = (n > 0 && n < 256 * 1024) ? malloc((size_t)n + 1) : NULL;
        if (b && fread(b, 1, (size_t)n, f) == (size_t)n) {
            b[n] = '\0';
            s_ledger = cJSON_Parse(b);
        }
        free(b);
        fclose(f);
    }
    if (!s_ledger) {
        s_ledger = cJSON_CreateObject();
        cJSON_AddNumberToObject(s_ledger, "v", 1);
        cJSON_AddObjectToObject(s_ledger, "root");
        cJSON_AddObjectToObject(s_ledger, "days");
    }
}

static void ledger_save(void)
{
    if (!s_ledger) return;
    char *txt = cJSON_PrintUnformatted(s_ledger);
    if (!txt) return;
    char tmp[200];
    snprintf(tmp, sizeof(tmp), "%s.tmp", SB_LEDGER_PATH);
    mkdir(SD_SESSIONS_DIR, 0775);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        fputs(txt, f);
        fclose(f);
        unlink(SB_LEDGER_PATH);
        rename(tmp, SB_LEDGER_PATH);
    }
    free(txt);
}

static cJSON *ledger_day(const char *day, bool create)
{
    if (!s_ledger) return NULL;
    cJSON *days = cJSON_GetObjectItem(s_ledger, "days");
    cJSON *d = cJSON_GetObjectItem(days, day);
    if (!d && create) {
        d = cJSON_AddObjectToObject(days, day);
    }
    return d;
}

static bool ledger_seen(const char *day, const char *name, const char *size,
                        const char *ts)
{
    cJSON *d = ledger_day(day, false);
    cJSON *e = d ? cJSON_GetObjectItem(d, name) : NULL;
    if (!e) return false;
    const char *sz = cJSON_GetStringValue(cJSON_GetObjectItem(e, "size"));
    const char *tt = cJSON_GetStringValue(cJSON_GetObjectItem(e, "ts"));
    return sz && tt && size && ts && !strcmp(sz, size) && !strcmp(tt, ts);
}

static void ledger_mark(const char *day, const char *name, const char *size,
                        const char *ts)
{
    cJSON *d = ledger_day(day, true);
    if (!d) return;
    cJSON *e = cJSON_AddObjectToObject(d, name);
    if (!e) return;
    cJSON_AddStringToObject(e, "size", size ? size : "");
    cJSON_AddStringToObject(e, "ts", ts ? ts : "");
}

/* ── Sync engine (§5.4) ─────────────────────────────────────────────── */

/* Root + SETTINGS bundle: Identification.{json,tgt}, CurrentSettings.json and
 * STR.edf.  The importer needs them for identity, settings and the multi-day
 * summary, so they are refreshed before the day files. */
static void sync_root_dir(const char *directory, const char *ledger_prefix)
{
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "directory", directory);
    cJSON *res = NULL;
    if (json_op("LIST_DIRECTORY", args, &res) != ESP_OK || !res) {
        cJSON_Delete(res);
        return;
    }

    cJSON *files = cJSON_GetObjectItem(res, "files");
    cJSON *f;
    cJSON_ArrayForEach(f, files) {
        const char *fn = cJSON_GetStringValue(cJSON_GetObjectItem(f, "name"));
        const char *size = cJSON_GetStringValue(cJSON_GetObjectItem(f, "size"));
        const char *ts = cJSON_GetStringValue(cJSON_GetObjectItem(f, "ts"));
        if (!fn || !size) continue;
        /* Only the files the importer consumes. */
        bool want = strcmp(fn, "STR.edf") == 0 ||
                    strcmp(fn, "Identification.json") == 0 ||
                    strcmp(fn, "Identification.tgt") == 0 ||
                    strcmp(fn, "CurrentSettings.json") == 0;
        if (!want) continue;

        char key[96];
        snprintf(key, sizeof(key), "%s%s", ledger_prefix, fn);
        cJSON *root = cJSON_GetObjectItem(s_ledger, "root");
        cJSON *e = root ? cJSON_GetObjectItem(root, key) : NULL;
        bool same = false;
        if (e) {
            const char *sz = cJSON_GetStringValue(cJSON_GetObjectItem(e, "size"));
            const char *tt = cJSON_GetStringValue(cJSON_GetObjectItem(e, "ts"));
            same = sz && tt && ts && !strcmp(sz, size) && !strcmp(tt, ts);
        }
        if (same) continue;

        ESP_LOGI(TAG, "root: pulling %s (%s bytes)", key, size);
        if (sb_get_file(directory, fn, size) != ESP_OK) {
            ESP_LOGW(TAG, "root: %s failed (%s)", key, s_last_error);
            continue;
        }
        root = cJSON_GetObjectItem(s_ledger, "root");
        if (root) {
            cJSON *n = cJSON_AddObjectToObject(root, key);
            if (n) {
                cJSON_AddStringToObject(n, "size", size);
                cJSON_AddStringToObject(n, "ts", ts ? ts : "");
            }
            ledger_save();
        }
        s_sync_files_done++;
    }
    cJSON_Delete(res);
}

/* One sync pass: days newest-first, files smallest-first, BRP last. */
static void sync_run(bool full)
{
    const int days = full ? SB_SYNC_DAYS_FULL : SB_SYNC_DAYS;
    cJSON *args = cJSON_CreateObject();
    cJSON_AddNumberToObject(args, "days", days);
    cJSON *res = NULL;
    if (json_op("LIST_RECENT_DAYS", args, &res) != ESP_OK || !res) {
        cJSON_Delete(res);
        return;
    }

    ledger_load();
    sync_root_dir("A:\\", "");
    sync_root_dir("A:\\SETTINGS", "SETTINGS/");
    cJSON *list = cJSON_GetObjectItem(res, "days");
    int n = cJSON_GetArraySize(list);
    ESP_LOGI(TAG, "sync: %d day(s) in the %d-day window", n, days);

    for (int i = 0; i < n; i++) {
        cJSON *d = cJSON_GetArrayItem(list, i);
        const char *day = cJSON_IsString(d) ? d->valuestring : NULL;
        if (!day || strlen(day) != 8) continue;

        cJSON *da = cJSON_CreateObject();
        cJSON_AddStringToObject(da, "day", day);
        cJSON *dres = NULL;
        if (json_op("LIST_DAY", da, &dres) != ESP_OK || !dres) {
            cJSON_Delete(dres);
            continue;
        }

        /* Pending = listed entries the ledger has not recorded verbatim. */
        cJSON *files = cJSON_GetObjectItem(dres, "files");
        int pending = 0;
        cJSON *f;
        cJSON_ArrayForEach(f, files) {
            const char *fn = cJSON_GetStringValue(cJSON_GetObjectItem(f, "name"));
            const char *size = cJSON_GetStringValue(cJSON_GetObjectItem(f, "size"));
            const char *ts = cJSON_GetStringValue(cJSON_GetObjectItem(f, "ts"));
            if (!fn || !size) continue;
            if (!ledger_seen(day, fn, size, ts)) pending++;
        }
        if (pending == 0) { cJSON_Delete(dres); continue; }

        /* Pre-warm the bridge's freshness guard. */
        cJSON *pa = cJSON_CreateObject();
        cJSON *arr = cJSON_AddArrayToObject(pa, "days");
        cJSON_AddItemToArray(arr, cJSON_CreateString(day));
        cJSON *pres = NULL;
        if (json_op("PREPARE_DAYS", pa, &pres) == ESP_OK && pres) {
            cJSON *eta = cJSON_GetObjectItem(pres, "eta_ms");
            if (cJSON_IsNumber(eta) && eta->valuedouble > 0)
                vTaskDelay(pdMS_TO_TICKS((uint32_t)eta->valuedouble));
        }
        cJSON_Delete(pres);

        /* Smallest first, BRP last (its BRP is the big one). */
        static const char *const order[] = { "EVE", "CSL", "SA2", "SAD",
                                             "PLD", "BRP" };
        for (size_t k = 0; k < sizeof(order) / sizeof(order[0]); k++) {
            cJSON_ArrayForEach(f, files) {
                const char *fn = cJSON_GetStringValue(cJSON_GetObjectItem(f, "name"));
                const char *size = cJSON_GetStringValue(cJSON_GetObjectItem(f, "size"));
                const char *ts = cJSON_GetStringValue(cJSON_GetObjectItem(f, "ts"));
                if (!fn || !size || !strstr(fn, order[k])) continue;
                if (ledger_seen(day, fn, size, ts)) continue;
                if (s_conn == BLE_HS_CONN_HANDLE_NONE) { cJSON_Delete(dres); return; }
                if (sb_get_file(day, fn, size) != ESP_OK) {
                    ESP_LOGW(TAG, "sync: %s/%s failed (%s)", day, fn, s_last_error);
                    if (strstr(s_last_error, "THERAPY")) { cJSON_Delete(dres); return; }
                    continue;
                }
                ledger_mark(day, fn, size, ts);
                ledger_save();
                s_sync_files_done++;
            }
        }

        /* Drift sidecar: the importer embeds it in every session manifest. */
        cJSON *ga = cJSON_CreateObject();
        char gdir[48];
        snprintf(gdir, sizeof(gdir), "/DATALOG/%s", day);
        cJSON_AddStringToObject(ga, "directory", gdir);
        cJSON *gres = NULL;
        if (json_op("GET_DRIFT", ga, &gres) == ESP_OK && gres) {
            char p[200];
            snprintf(p, sizeof(p), SD_IMPORT_DIR "/DATALOG/%s/_bridge_drift.json", day);
            char *txt = cJSON_PrintUnformatted(gres);
            if (txt) {
                FILE *df = fopen(p, "wb");
                if (df) { fputs(txt, df); fclose(df); }
                free(txt);
            }
        }
        cJSON_Delete(gres);
        cJSON_Delete(dres);

        s_sync_days_done++;
        ESP_LOGI(TAG, "sync: %s staged (%d day(s) done)", day, s_sync_days_done);
    }

    cJSON_Delete(res);
    s_last_sync_ms = esp_timer_get_time() / 1000;

    /* Hand the staged files to the Phase-1 importer (it deletes them after a
     * durable commit and rebuilds/upload the day). */
    if (s_sync_files_done > 0 && s_import_kick) s_import_kick();
}

/* ── Status / sync task ─────────────────────────────────────────────── */

static void poll_status(void)
{
    cJSON *res = NULL;
    if (json_op("GET_STATUS", NULL, &res) != ESP_OK || !res) { cJSON_Delete(res); return; }
    cJSON *th = cJSON_GetObjectItem(res, "therapy");
    if (th) {
        cJSON *st = cJSON_GetObjectItem(th, "state");
        cJSON *ta = cJSON_GetObjectItem(th, "transfer_allowed");
        s_therapy_active = cJSON_IsNumber(st) && st->valueint == 1;
        s_transfer_allowed = cJSON_IsTrue(ta);
    }
    cJSON_Delete(res);
}

static void sb_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Auto-reconnect to a bonded bridge. */
    if (peer_load(&s_peer)) {
        s_paired = true;
        ble_gap_connect(s_own_addr_type, &s_peer, 30000, NULL, sb_gap_event, NULL);
        set_state(SB_STATUS_CONNECTING);
    }

    int64_t last_poll = 0, last_sync = 0;
    int64_t link_up_ms = 0;
    for (;;) {
        const int64_t now = esp_timer_get_time() / 1000;
        /* Inject a queued PIN from this task, never from httpd. */
        if (xSemaphoreTake(s_pin_mbox, 0) == pdTRUE) {
            struct ble_gap_conn_desc pin_desc;
            if (s_conn == BLE_HS_CONN_HANDLE_NONE ||
                ble_gap_conn_find(s_conn, &pin_desc) != 0) {
                ESP_LOGI(TAG, "PIN ignored: no live link");
            } else if (pin_desc.sec_state.encrypted) {
                ESP_LOGI(TAG, "PIN ignored: link already encrypted");
            } else {
                struct ble_sm_io io = {
                    .action = BLE_SM_IOACT_INPUT,
                    .passkey = (uint32_t)strtoul(s_pin_mbox_val, NULL, 10),
                };
                int rc = ble_sm_inject_io(s_conn, &io);
                if (rc != 0) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "passkey not accepted (rc=%d)", rc);
                    ESP_LOGW(TAG, "inject_io rc=%d", rc);
                    set_error(msg);
                } else {
                    s_pin_pending = true;
                    ESP_LOGI(TAG, "passkey injected, waiting for encryption");
                }
            }
        }

        /* A link that never reaches encryption is a dead end (bond mismatch,
         * window closed): drop it.  The budget is re-derived every pass from
         * the *current* state and anchored to when the link came up, so it
         * cannot be shortened by arming it during the brief "connecting"
         * phase (the previous version froze the 20 s figure and cut the
         * user off mid-PIN, which also wedged the BT controller by tearing
         * the link down in the middle of its PHY/SM procedures). */
        bool link_bonded = false;
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
            struct ble_gap_conn_desc d;
            link_bonded = ble_gap_conn_find(s_conn, &d) == 0 && d.sec_state.bonded;
        }
        if (s_conn != BLE_HS_CONN_HANDLE_NONE && !link_bonded &&
            strcmp(s_state, SB_STATUS_PAIRED) != 0) {
            if (link_up_ms == 0) link_up_ms = now;
            const int64_t budget = (strcmp(s_state, SB_STATUS_WAIT_PASSKEY) == 0 ||
                                    s_pin_pending)
                                       ? SB_PIN_BUDGET_MS : 20000;
            if (now - link_up_ms > budget) {
                ESP_LOGW(TAG, "no encryption within %lld s — disconnecting",
                         (long long)(budget / 1000));
                link_up_ms = 0;
                sb_ble_disconnect();
                if (!s_paired) set_state(SB_STATUS_IDLE);
            }
        } else {
            link_up_ms = 0;
        }

        /* Bonded but not connected: keep trying.  Without this a single
         * failed attempt (bridge rebooted, link dropped) left the client
         * idle forever. */
        if (s_conn == BLE_HS_CONN_HANDLE_NONE && s_paired) {
            static int64_t last_try;
            if (now - last_try > 15000) {
                last_try = now;
                if (ble_gap_connect(s_own_addr_type, &s_peer, 30000, NULL,
                                    sb_gap_event, NULL) == 0)
                    set_state(SB_STATUS_CONNECTING);
            }
        }
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
            if (now - last_poll > SB_POLL_MS) {
                last_poll = now;
                poll_status();
                if (time_sync_is_synced() && now - s_time_last_ms > SB_TIME_PERIOD_MS)
                    time_sync_rounds();
            }
            /* Sync when the bridge says transfers are allowed and either a
             * manual request is queued or the idle window elapsed. */
            if (s_transfer_allowed &&
                (s_sync_running || now - last_sync > SB_IDLE_SYNC_MS)) {
                if (xSemaphoreTake(s_sync_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
                    s_sync_running = true;
                    s_sync_days_done = s_sync_files_done = 0;
                    sync_run(s_sync_full);
                    s_sync_full = false;
                    s_sync_running = false;
                    last_sync = esp_timer_get_time() / 1000;
                    xSemaphoreGive(s_sync_mtx);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ── GATT discovery ─────────────────────────────────────────────────── */

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t chr_val, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)conn; (void)arg;
    if (err->status == BLE_HS_EDONE) return 0;
    if (err->status != 0 || !dsc) return 0;
    if (chr_val == s_ctrl_val) s_ctrl_cccd = dsc->handle;
    else if (chr_val == s_data_val) s_data_cccd = dsc->handle;
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn; (void)arg;
    if (err->status == BLE_HS_EDONE) {
        if (s_ctrl_val && s_data_val) {
            ble_gattc_disc_all_dscs(s_conn, s_ctrl_val, s_svc_end, on_dsc, NULL);
            ble_gattc_disc_all_dscs(s_conn, s_data_val, s_svc_end, on_dsc, NULL);
            s_disc_done = true;
        }
        return 0;
    }
    if (err->status != 0 || !chr) return 0;
    if (ble_uuid_cmp(&chr->uuid.u, &UUID_CTRL.u) == 0) s_ctrl_val = chr->val_handle;
    if (ble_uuid_cmp(&chr->uuid.u, &UUID_DATA.u) == 0) s_data_val = chr->val_handle;
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn; (void)arg;
    if (err->status == BLE_HS_EDONE) return 0;
    if (err->status != 0 || !svc) return 0;
    if (ble_uuid_cmp(&svc->uuid.u, &UUID_SVC.u) == 0) {
        s_svc_start = svc->start_handle;
        s_svc_end = svc->end_handle;
        ble_gattc_disc_all_chrs(s_conn, s_svc_start, s_svc_end, on_chr, NULL);
    }
    return 0;
}

static void subscribe_chars(void)
{
    uint8_t ind[2] = { 0x02, 0x00 };      /* indicate */
    uint8_t ntf[2] = { 0x01, 0x00 };      /* notify   */
    if (s_ctrl_cccd) ble_gattc_write_flat(s_conn, s_ctrl_cccd, ind, 2, write_cb, NULL);
    if (s_data_cccd) ble_gattc_write_flat(s_conn, s_data_cccd, ntf, 2, write_cb, NULL);
}

/* ── GAP events ─────────────────────────────────────────────────────── */

static int sb_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            set_error("connect failed");
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            return 0;
        }
        s_conn = event->connect.conn_handle;
        set_state(SB_STATUS_CONNECTING);
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(s_conn, &desc) == 0) {
                s_peer = desc.peer_id_addr;
                s_mtu = 23;                 /* raised by BLE_GAP_EVENT_MTU */
            }
        }
        ble_gattc_exchange_mtu(s_conn, NULL, NULL);
        /* SomnoBridge requires LE Secure Connections + MITM (§3).  A bridge
         * that already holds a bond refuses this by design: replacing the
         * peer needs its physically confirmed FORGET_PEER action, so surface
         * the failure instead of sitting in "connecting" forever. */
        {
            int rc = ble_gap_security_initiate(s_conn);
            if (rc != 0) {
                ESP_LOGW(TAG, "security initiate failed: rc=%d (bond exists on "
                              "the bridge? FORGET_PEER is required)", rc);
                set_error("bridge refused pairing");
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        s_pin_pending = false;
        ESP_LOGW(TAG, "disconnected (reason=%d)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        if (s_stream_active) stream_fail("link lost");
        s_disc_done = false;
        if (s_paired) set_state(SB_STATUS_CONNECTING);
        else set_state(SB_STATUS_IDLE);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc desc;
        if (event->enc_change.status == 0 && ble_gap_conn_find(s_conn, &desc) == 0 &&
            desc.sec_state.encrypted && desc.sec_state.authenticated &&
            desc.sec_state.bonded) {
            ESP_LOGI(TAG, "link encrypted, authenticated and bonded");
            peer_save(&s_peer);
            s_paired = true;
            set_state(SB_STATUS_PAIRED);
            s_ctrl_val = s_data_val = s_ctrl_cccd = s_data_cccd = 0;
            s_disc_done = false;
            ble_gattc_disc_all_svcs(s_conn, on_svc, NULL);
        } else {
            ESP_LOGW(TAG, "encryption failed: status=%d encrypted=%d auth=%d bonded=%d",
                     event->enc_change.status,
                     desc.sec_state.encrypted, desc.sec_state.authenticated,
                     desc.sec_state.bonded);
            char msg[64];
            snprintf(msg, sizeof(msg), "encryption failed (status %d)",
                     event->enc_change.status);
            set_error(msg);
        }
        return 0;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_INPUT) {
            ESP_LOGI(TAG, "bridge displays a 6-digit PIN — waiting for the user");
            set_state(SB_STATUS_WAIT_PASSKEY);
            xSemaphoreGive(s_pin_sem);
        } else {
            ESP_LOGW(TAG, "unsupported passkey action %d", event->passkey.params.action);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU %u", (unsigned)s_mtu);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        const uint16_t h = event->notify_rx.attr_handle;
        const uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len < SB_HDR_LEN) return 0;
        uint8_t hdr[SB_HDR_LEN];
        uint16_t copied = 0;
        ble_hs_mbuf_to_flat(event->notify_rx.om, hdr, sizeof(hdr), &copied);
        uint8_t kind;
        uint16_t flags;
        uint32_t rid;
        uint64_t off;
        if (!sb_hdr_parse(hdr, copied, &kind, &flags, &rid, &off)) return 0;
        const size_t plen = len - SB_HDR_LEN;
        uint8_t *all = malloc(len);
        if (!all) return 0;
        uint16_t got = 0;
        if (ble_hs_mbuf_to_flat(event->notify_rx.om, all, len, &got) != 0 ||
            got != len) {
            free(all);
            return 0;
        }
        uint8_t *payload = all + SB_HDR_LEN;

        if (h == s_data_val) {
            stream_data(payload, plen, kind);
        } else if (kind == SB_KIND_TIME_RSP) {
            time_on_reply(payload, plen);
        } else if (kind == SB_KIND_JSON_RSP) {
            if (flags & SB_FLAG_START) { s_rx_len = 0; s_rx_next_off = 0; s_rx_rid = rid; }
            if (rid == s_rx_rid && off == s_rx_next_off && s_rx_len + plen < SB_JSON_MAX) {
                memcpy(s_rx_buf + s_rx_len, payload, plen);
                s_rx_len += plen;
                s_rx_next_off += plen;
                if (flags & SB_FLAG_END) {
                    s_rx_buf[s_rx_len] = '\0';
                    json_complete(rid, (const char *)s_rx_buf);
                }
            }
        }
        free(all);
        return 0;
    }

    default:
        return 0;
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t sb_ble_init(void)
{
    if (s_available) return ESP_OK;

    s_rx_buf = heap_caps_malloc(SB_JSON_MAX + 1, MALLOC_CAP_SPIRAM);
    if (!s_rx_buf) s_rx_buf = malloc(SB_JSON_MAX + 1);
    if (!s_rx_buf) return ESP_ERR_NO_MEM;

    s_req_mtx = xSemaphoreCreateMutex();
    s_write_mtx = xSemaphoreCreateMutex();
    s_resp_sem = xSemaphoreCreateBinary();
    s_write_done = xSemaphoreCreateBinary();
    s_pin_sem = xSemaphoreCreateBinary();
    s_pin_mbox = xSemaphoreCreateBinary();
    s_sync_mtx = xSemaphoreCreateMutex();
    if (!s_req_mtx || !s_write_mtx || !s_resp_sem || !s_write_done ||
        !s_pin_sem || !s_sync_mtx || !s_pin_mbox)
        return ESP_ERR_NO_MEM;

    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGW(TAG, "own address type not ready yet");
        s_own_addr_type = 0;
    }

    s_available = true;
    if (psram_task_create(sb_task, "sb_ble", 8192, NULL, 4, tskNO_AFFINITY,
                          NULL, NULL) == NULL) {
        s_available = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "SomnoBridge client started");
    return ESP_OK;
}

bool sb_ble_is_available(void) { return s_available; }
bool sb_ble_is_paired(void) { return s_paired; }
const char *sb_ble_get_status(void) { return state_str(); }
const char *sb_ble_get_error(void) { return s_error; }
bool sb_ble_therapy_active(void) { return s_therapy_active; }
void sb_ble_set_import_kick_fn(sb_import_kick_fn_t fn) { s_import_kick = fn; }

esp_err_t sb_ble_start_pair_ex(const char *addr_str, uint8_t addr_type)
{
    if (!s_available || !addr_str) return ESP_ERR_INVALID_ARG;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) sb_ble_disconnect();

    unsigned int a[6];
    if (sscanf(addr_str, "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6)
        return ESP_ERR_INVALID_ARG;
    s_peer.type = addr_type;      /* random-static peers reject PUBLIC */
    for (int i = 0; i < 6; i++) s_peer.val[i] = (uint8_t)a[5 - i];
    s_error[0] = '\0';
    set_state(SB_STATUS_CONNECTING);
    xSemaphoreTake(s_pin_sem, 0);
    /* A retry while an earlier attempt is still pending answers EALREADY
     * (rc=15), so drop any in-flight connect and any live link first. */
    /* Pair pressed while a link is already up (or a retry is in flight):
     * re-initiating security on it is what the user means, and dialling
     * again would just answer EALREADY (rc=15). */
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        int rcs = ble_gap_security_initiate(s_conn);
        if (rcs == 0) { set_state(SB_STATUS_CONNECTING); return ESP_OK; }
    }
    ble_gap_conn_cancel();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    int rc = ble_gap_connect(s_own_addr_type, &s_peer, 30000, NULL, sb_gap_event, NULL);
    if (rc != 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "connect start failed (rc=%d)", rc);
        set_error(msg);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sb_ble_start_pair(const char *addr_str)
{
    return sb_ble_start_pair_ex(addr_str, BLE_ADDR_PUBLIC);
}

esp_err_t sb_ble_confirm_pair(const char *pin6)
{
    /* BLE SM passkeys are six decimal digits (0..999999) — the bridge's
     * gen_passkey() is esp_random() % 1000000.  Anything longer is not a
     * passkey and must not be injected: it would be silently out of range. */
    if (!pin6 || strlen(pin6) != 6) return ESP_ERR_INVALID_ARG;
    for (const char *p = pin6; *p; p++)
        if (*p < '0' || *p > '9') return ESP_ERR_INVALID_ARG;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return ESP_ERR_INVALID_STATE;

    /* Hand the PIN to the bridge task (see s_pin_mbox): injecting from the
     * httpd worker panicked the device. */
    snprintf(s_pin_mbox_val, sizeof(s_pin_mbox_val), "%s", pin6);
    xSemaphoreGive(s_pin_mbox);
    ESP_LOGI(TAG, "passkey queued for injection");
    return ESP_OK;
}

esp_err_t sb_ble_forget(void)
{
    s_paired = false;
    memset(&s_peer, 0, sizeof(s_peer));
    nvs_handle_t h;
    if (nvs_open(SB_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, SB_NVS_ADDR);
        nvs_commit(h);
        nvs_close(h);
    }
    ble_store_clear();
    sb_ble_disconnect();
    set_state(SB_STATUS_IDLE);
    return ESP_OK;
}

esp_err_t sb_ble_disconnect(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return ESP_OK;
    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    return ESP_OK;
}

esp_err_t sb_ble_sync_now(bool full)
{
    if (!s_paired) return ESP_ERR_INVALID_STATE;
    s_sync_full = full;
    s_sync_running = true;
    return ESP_OK;
}

cJSON *sb_ble_status_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", s_state);
    cJSON_AddBoolToObject(o, "available", s_available);
    cJSON_AddBoolToObject(o, "paired", s_paired);
    cJSON_AddBoolToObject(o, "connected", s_conn != BLE_HS_CONN_HANDLE_NONE);
    cJSON_AddBoolToObject(o, "therapy_active", s_therapy_active);
    cJSON_AddBoolToObject(o, "transfer_allowed", s_transfer_allowed);
    cJSON_AddBoolToObject(o, "sync_running", s_sync_running);
    cJSON_AddNumberToObject(o, "mtu", s_mtu);
    if (s_error[0]) cJSON_AddStringToObject(o, "error", s_error);
    if (s_last_error[0]) cJSON_AddStringToObject(o, "last_error", s_last_error);
    cJSON_AddNumberToObject(o, "last_sync_s", s_last_sync_ms / 1000);
    cJSON_AddNumberToObject(o, "sync_days", s_sync_days_done);
    cJSON_AddNumberToObject(o, "sync_files", s_sync_files_done);
    return o;
}
