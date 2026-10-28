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
#include "upload_sched.h"
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
/* Per-file STREAM_END wait. The bridge reads each file over the card's
 * HTTP endpoint while it streams; EZShare reads are bursty and a
 * multi-MB file can legitimately take minutes even at full BLE speed.
 * Dead streams are still bounded by the bridge's own 10 s no-credit
 * abort, so a generous client wait only costs time on true failures.  */
#define SB_STREAM_TIMEOUT (300 * 1000)
/* Dead-air bound: the reference client's stream loop abandons a pull
 * after 30 s without progress; each card read is itself bounded by the
 * bridge's 30 s HTTP timeout, so ~45 s of silence means the fetch is
 * wedged — abort it rather than park the sync on dead air.          */
#define SB_STREAM_STALL_US (45LL * 1000 * 1000)
#define SB_TIME_ROUNDS    5
#define SB_TIME_PERIOD_MS (10 * 60 * 1000)
#define SB_POLL_MS        (30 * 1000)
#define SB_IDLE_SYNC_MS   (5 * 60 * 1000)
#define SB_PIN_BUDGET_MS  90000   /* > the bridge's 60 s attempt limit */
#define SB_SEC_DELAY_MS   700     /* see deferred security note below  */
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
static int64_t s_sec_due_ms;        /* deferred security_initiate (ms)  */

static uint16_t s_ctrl_val, s_ctrl_cccd, s_data_val, s_data_cccd;
static uint16_t s_svc_start, s_svc_end;
static bool s_subscribed;           /* ctrl indicate + data notify armed */

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
static volatile bool s_rx_armed;    /* DATA frames accepted (admitted)   */
static volatile bool s_stream_dead; /* first terminal cause latched       */
static char     s_stream_err[48];   /* that cause (sticky until reset)    */
static bool     s_stream_admitted;  /* remote stream exists → ABORT helps */
static uint8_t  s_stream_wire_ver;  /* negotiated wire (STREAM_BEGIN)     */
static uint32_t s_stream_rid;
static uint32_t s_stream_rsp_rid;   /* rid routed to the stream inbox     */
static char    *s_stream_rsp_json;  /* completed body for the stream op   */
static SemaphoreHandle_t s_stream_rsp_sem;
static FILE    *s_stream_f;
static char     s_stream_path[192];
static char     s_stream_tmp[200];
static int64_t  s_stream_expected;
static uint64_t s_stream_got;         /* decoded bytes written            */
static uint64_t s_stream_acked;
static uint64_t s_stream_wire;        /* cumulative stream bytes received  */
static bool     s_ack_pending;        /* due/failed ACK still to send     */
static int64_t  s_ack_last_us;
static mbedtls_sha256_context s_sha;
static bool     s_sha_active;
static char     s_stream_sha[72];
static uint8_t *s_v2_buf;             /* compact-frame accumulator        */
static size_t   s_v2_len;
static uint16_t s_v2_seq;
static int      s_v2_seq_errors;
static uint8_t *s_v2_out;             /* decode buffer (persistent 64KiB) */
static uint8_t  s_stream_unit_shift;  /* listing display unit, log2       */

/* SPSC receive ring: DATA notifications are flattened/copied here on the
 * NimBLE host task and consumed by stream_pump() on the bridge task —
 * inflate/hash/SD writes never run inside the BLE callback.  96 items
 * cover a full credit window (16384 B ≈ 69 × 240 B frames) plus headroom
 * for a consumer stall, so the ring can never be overrun while credits
 * are honoured. */
#define SB_DQ_N 96
typedef struct { uint16_t len; uint8_t data[252]; } sb_dq_item_t;
static sb_dq_item_t *s_dq;
static volatile uint16_t s_dq_head, s_dq_tail;
static portMUX_TYPE s_dq_mux = portMUX_INITIALIZER_UNLOCKED;

/* Stream diagnostics (metadata only — never file content) */
static uint32_t s_rx_frames, s_dup_frames, s_stale_frames, s_dq_drops;
static uint32_t s_ack_tries, s_ack_ok, s_ack_fail;
static uint32_t s_seq_gaps;
static uint16_t s_gap_expect, s_gap_got;
static uint32_t s_end_frames;         /* device-side notify count         */

/* Telemetry (read by the HTTP layer) */
static bool     s_therapy_active;
static bool     s_transfer_allowed;
static bool     s_card_wifi;    /* bridge STA joined the card        */
static char     s_card_mode[8]; /* bridge netw mode: STA/AP/DOWN     */
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

/* ACK path variant: never block the NimBLE host task on a contended
 * writer mutex — a busy holder is skipped and the next data frame
 * re-acks at a higher offset (ACKs are cumulative by design).        */
static esp_err_t ctrl_write_try(const uint8_t *data, size_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_ctrl_val)
        return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_write_mtx, 0) != pdTRUE)
        return ESP_ERR_INVALID_STATE;
    esp_err_t ret = ESP_OK;
    if (ble_gattc_write_no_rsp_flat(s_conn, s_ctrl_val, data, len) != 0)
        ret = ESP_FAIL;
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

/* A complete JSON body arrived: hand it to the owning waiter.  Stream
 * ops register their rid in s_stream_rsp_rid so a late STREAM_END can
 * never be mistaken for a portal answer (and vice versa). */
static void json_complete(uint32_t rid, const char *body)
{
    if (rid == s_stream_rsp_rid) {
        free(s_stream_rsp_json);
        s_stream_rsp_json = strdup(body);
        xSemaphoreGive(s_stream_rsp_sem);
    } else if (rid == s_pending_rid) {
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

/* Consumer: bridge task only.  The producer (the NimBLE host task's
 * notify path) writes s_dq[s_dq_head] directly so the mbuf can be
 * flattened straight into the slot without a second copy. */
static bool dq_pop(sb_dq_item_t *out)
{
    if (s_dq_tail == s_dq_head) return false;
    *out = s_dq[s_dq_tail];
    portENTER_CRITICAL(&s_dq_mux);
    s_dq_tail = (uint16_t)((s_dq_tail + 1) % SB_DQ_N);
    portEXIT_CRITICAL(&s_dq_mux);
    return true;
}

/* Drop ring residue between attempts (task context). */
static void dq_reset(void)
{
    portENTER_CRITICAL(&s_dq_mux);
    s_dq_head = s_dq_tail = 0;
    portEXIT_CRITICAL(&s_dq_mux);
}

static void stream_ack(uint64_t off, uint32_t window)
{
    /* §10.3: the credit offset travels in the frame header's u64 field
     * (next contiguous received stream byte), the window as a u32 LE
     * payload — ctrl_send() only packs fragment offsets, so the ACK
     * frame is built by hand.                                        */
    uint8_t pkt[SB_HDR_LEN + 4];
    sb_hdr_pack(pkt, SB_KIND_ACK, SB_FLAG_START | SB_FLAG_END,
                s_stream_rid, off);
    wr_u32(pkt + SB_HDR_LEN, window);
    /* Write-no-response on the hot path.  Offsets are cumulative so a
     * failed write is retried at the next cadence point (s_ack_pending)
     * and counted — not silently subsumed.                            */
    s_ack_tries++;
    if (ctrl_write_try(pkt, sizeof(pkt)) == ESP_OK) {
        s_stream_acked = off;
        s_ack_last_us = esp_timer_get_time();
        s_ack_pending = false;
        s_ack_ok++;
    } else {
        s_ack_pending = true;
        s_ack_fail++;
    }
}

/* §10.3 credit cadence: ≥ every 16 frames or 250 ms.  The byte-count
 * cadence is covered in stream_pump(); this tick closes the right edge
 * (the sender may be waiting on exactly the tail bytes) and retries a
 * write that lost to an in-flight control op. */
static void stream_ack_tick(void)
{
    if (s_ack_pending ||
        (s_rx_armed && s_stream_wire > s_stream_acked &&
         esp_timer_get_time() - s_ack_last_us >= 250000))
        stream_ack(s_stream_wire, SB_WINDOW);
}

/* Initial credit grant, sent from the sync task before the first byte:
 * this one must land or nothing ever arrives, so it is confirmed — and
 * its failure must fail the stream instead of burning the stall window. */
static esp_err_t stream_ack_initial(uint64_t off, uint32_t window)
{
    uint8_t pkt[SB_HDR_LEN + 4];
    sb_hdr_pack(pkt, SB_KIND_ACK, SB_FLAG_START | SB_FLAG_END,
                s_stream_rid, off);
    wr_u32(pkt + SB_HDR_LEN, window);
    if (ctrl_write(pkt, sizeof(pkt), true) != ESP_OK)
        return ESP_FAIL;
    s_stream_acked = off;
    s_ack_last_us = esp_timer_get_time();
    s_ack_pending = false;
    s_ack_ok++;
    s_ack_tries++;
    return ESP_OK;
}

/* Tell the bridge to drop the in-flight stream (§10.3 ABORT frame): a
 * client that simply walks away leaves the bridge fetching a stream
 * nobody is reading, and the card stays busy for its whole duration.
 * `confirmed` waits for the write response — only safe off the NimBLE
 * host task; on it we fire-and-forget like the ACK path.            */
static void stream_abort(bool confirmed)
{
    if (!s_stream_admitted) return;
    uint8_t pkt[SB_HDR_LEN];
    sb_hdr_pack(pkt, SB_KIND_ABORT, SB_FLAG_START | SB_FLAG_END,
                s_stream_rid, 0);
    if (confirmed)
        (void)ctrl_write(pkt, sizeof(pkt), true);
    else
        (void)ctrl_write_try(pkt, sizeof(pkt));
}

/* Terminal latch — safe in ANY context (NimBLE host task included): it
 * only sets flags so the wait loop notices; cleanup runs later on the
 * bridge task in stream_terminate().  The FIRST cause wins: later
 * stall/timeout/link-loss fallout must not overwrite what actually
 * killed the stream. */
static void stream_fail(const char *why)
{
    if (!s_stream_dead) {
        snprintf(s_stream_err, sizeof(s_stream_err), "%s",
                 (why && why[0]) ? why : "stream failed");
        s_stream_dead = true;
    }
    s_rx_armed = false;
    s_stream_active = false;
}

/* Task-context finaliser: abort the remote fetch (only when the stream
 * was admitted and the link is up), drop the staged file, free the hash,
 * and surface the latched cause with the counters that distinguish the
 * possible loss points.  Safe to call repeatedly. */
static void stream_terminate(void)
{
    if (s_stream_admitted && s_conn != BLE_HS_CONN_HANDLE_NONE)
        stream_abort(true);
    s_stream_admitted = false;
    s_rx_armed = false;
    s_stream_active = false;
    if (s_stream_f) { fclose(s_stream_f); s_stream_f = NULL; }
    if (s_stream_tmp[0]) unlink(s_stream_tmp);
    s_stream_tmp[0] = '\0';
    if (s_sha_active) { mbedtls_sha256_free(&s_sha); s_sha_active = false; }
    dq_reset();
    if (s_stream_err[0])
        snprintf(s_last_error, sizeof(s_last_error), "%s", s_stream_err);
    ESP_LOGW(TAG, "stream %s: %s (rx=%u dup=%u stale=%u drop=%u "
             "gap=%u@%u->%u ack=%u/%u/%u wire=%llu raw=%llu)",
             s_stream_path, s_stream_err[0] ? s_stream_err : "failed",
             (unsigned)s_rx_frames, (unsigned)s_dup_frames,
             (unsigned)s_stale_frames, (unsigned)s_dq_drops,
             (unsigned)s_seq_gaps, (unsigned)s_gap_expect,
             (unsigned)s_gap_got,
             (unsigned)s_ack_ok, (unsigned)s_ack_fail,
             (unsigned)s_ack_tries,
             (unsigned long long)s_stream_wire,
             (unsigned long long)s_stream_got);
}

/* Transient stream errors worth a bounded wait + retry inside the attempt
 * budget: STALE — the bridge queued a day rescan (~10 s) and refused
 * rather than serve stale metadata; BUSY — the previous stream is still
 * tearing down.  The wait times live in sb_stream_retry_ms so the policy
 * stays host-testable; true means the caller should retry.            */
static bool stream_retry_wait(void)
{
    unsigned wait = sb_stream_retry_ms(s_last_error);
    if (!wait) return false;
    vTaskDelay(pdMS_TO_TICKS(wait));
    return true;
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
    s_stream_unit_shift = 0;
    s_stream_wire_ver = 0;
    s_stream_expected = -1;
    uint64_t exp;
    if (expected_size && sb_parse_u64_str(expected_size, &exp) &&
        exp <= (uint64_t)INT64_MAX)
        s_stream_expected = (int64_t)exp;
    /* Fresh attempt state: the sticky error, admission flags, ring, and
     * all diagnostic counters reset — nothing may survive from a torn
     * down stream. */
    s_stream_err[0] = '\0';
    s_stream_dead = false;
    s_stream_admitted = false;
    s_rx_armed = false;
    s_ack_pending = false;
    s_rx_frames = s_dup_frames = s_stale_frames = s_dq_drops = 0;
    s_ack_tries = s_ack_ok = s_ack_fail = 0;
    s_seq_gaps = 0;
    s_gap_expect = s_gap_got = 0;
    s_end_frames = 0;
    dq_reset();
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

/* v2 block stream: 12-byte self-describing headers, codecs 0/1/2.  Runs
 * on the bridge task inside stream_pump() — never in the BLE callback. */
static bool stream_v2_decode(const uint8_t *p, size_t n)
{
    if (!s_v2_buf) s_v2_buf = heap_caps_malloc(96 * 1024, MALLOC_CAP_SPIRAM);
    if (!s_v2_buf) return false;
    if (!s_v2_out)
        s_v2_out = heap_caps_malloc(64 * 1024,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_v2_out) return false;
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

        if (!sb_v2_block(codec, s_v2_buf + pos + 12, enc_len,
                         s_v2_out, raw_len)) {
            ESP_LOGW(TAG, "v2 decode fail: blk=%02x raw=%lu enc=%lu "
                     "buflen=%u seq=%u",
                     codec, (unsigned long)raw_len, (unsigned long)enc_len,
                     (unsigned)(s_v2_len - pos), (unsigned)s_v2_seq);
            return false;
        }
        if (!stream_write(s_v2_out, raw_len)) return false;
        pos += 12 + enc_len;
    }
    if (pos) {
        memmove(s_v2_buf, s_v2_buf + pos, s_v2_len - pos);
        s_v2_len -= pos;
    }
    return true;
}

/* Receive worker — the only place stream frames become file bytes.
 * Consumes the ring the NimBLE callback fills: frame classification,
 * sequence validation, inflate, SHA-256, SD writes, credit ACKs.  A
 * forward sequence gap is terminal the moment it is seen: continuing
 * would splice the deflate input and corrupt every later block. */
static void stream_pump(void)
{
    sb_dq_item_t it;
    while (!s_stream_dead && dq_pop(&it)) {
        const uint8_t *b = it.data;
        const uint8_t *payload = NULL;
        size_t plen = 0;
        if (it.len >= 4 && b[0] == SB_VER && b[1] == SB_KIND_STREAM2) {
            const uint16_t seq = rd_u16(b + 2);
            s_rx_frames++;
            const sb_seq_kind_t sc = sb_seq_class(seq, s_v2_seq);
            if (sc == SB_SEQ_DUP) { s_dup_frames++; continue; }
            if (sc == SB_SEQ_GAP) {
                if (!s_seq_gaps++) { s_gap_expect = s_v2_seq; s_gap_got = seq; }
                stream_fail("seq gap");
                return;
            }
            s_v2_seq = (uint16_t)(seq + 1);
            payload = b + 4;
            plen = it.len - 4;
            if (!stream_v2_decode(payload, plen)) {
                stream_fail("v2 decode");
                return;
            }
        } else if (it.len >= SB_HDR_LEN) {
            uint8_t kind; uint16_t flags; uint32_t rid; uint64_t off;
            if (!sb_hdr_parse(it.data, SB_HDR_LEN, &kind, &flags, &rid, &off) ||
                (kind != SB_KIND_FILE && kind != SB_KIND_EVIDENCE)) {
                s_dq_drops++;
                continue;
            }
            s_rx_frames++;
            payload = it.data + SB_HDR_LEN;
            plen = it.len - SB_HDR_LEN;
            if (!stream_write(payload, plen)) {
                stream_fail("write");
                return;
            }
        } else {
            s_dq_drops++;
            continue;
        }

        /* The credit offset counts contiguous stream payload bytes (the
         * same unit the sender charges against `sent`), not ATT/notify
         * bytes — frame headers never enter the total.               */
        s_stream_wire += plen;
        if (s_stream_wire - s_stream_acked >= SB_ACK_EVERY || s_ack_pending)
            stream_ack(s_stream_wire, SB_WINDOW);
    }
}

/* STREAM_END: every field the bridge promises is REQUIRED — a missing
 * or malformed field is a failure, never a skipped check.  Publication
 * (rename) happens only after the byte count, wire count, residual
 * deflate state, and SHA-256 all prove the file.  Failures route through
 * stream_fail() so the first cause stays latched for stream_terminate().
 */
static bool stream_end(cJSON *result)
{
    bool ok = false;
    const char *sha    = cJSON_GetStringValue(cJSON_GetObjectItem(result, "sha256"));
    const char *actual = cJSON_GetStringValue(cJSON_GetObjectItem(result, "actual_bytes"));
    const char *enc    = cJSON_GetStringValue(cJSON_GetObjectItem(result, "encoded_bytes"));
    const char *err    = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error"));
    const bool  t_ok      = cJSON_IsTrue(cJSON_GetObjectItem(result, "ok"));
    const bool  allow     = cJSON_IsTrue(cJSON_GetObjectItem(result, "completion_allowed"));
    const bool  unchanged = cJSON_IsTrue(cJSON_GetObjectItem(result, "source_unchanged"));
    const bool  checked   = cJSON_IsTrue(cJSON_GetObjectItem(result, "monitoring_checked"));
    const cJSON *clen     = cJSON_GetObjectItem(result, "content_length");
    const cJSON *fr       = cJSON_GetObjectItem(result, "frames");
    uint64_t actual_n = 0, enc_n = 0;

    if (s_stream_f) {
        /* Durability before publication: a corrupt tail rename is the
         * worst failure mode — surface a close error instead. */
        if (fflush(s_stream_f) != 0 || fsync(fileno(s_stream_f)) != 0 ||
            fclose(s_stream_f) != 0)
            stream_fail("write close");
        s_stream_f = NULL;
    }
    if (s_sha_active) {
        uint8_t dig[32];
        mbedtls_sha256_finish(&s_sha, dig);
        mbedtls_sha256_free(&s_sha);
        s_sha_active = false;
        for (int i = 0; i < 32; i++)
            snprintf(s_stream_sha + i * 2, 3, "%02x", dig[i]);
        s_stream_sha[64] = '\0';
    }
    if (cJSON_IsNumber(fr) && fr->valuedouble >= 0)
        s_end_frames = (uint32_t)fr->valuedouble;

    if (s_stream_dead) {
        /* an earlier terminal cause wins (e.g. write close) */
    } else if (!t_ok) stream_fail(err && err[0] ? err : "stream not ok");
    else if (!sha || !sb_hex_str_ok(sha, 64))
        stream_fail("missing sha256");
    else if (!actual || !sb_parse_u64_str(actual, &actual_n))
        stream_fail("missing actual_bytes");
    else if (!allow)   stream_fail(err && err[0] ? err : "completion not allowed");
    else if (!unchanged) stream_fail(err && err[0] ? err : "source changed");
    else if (!checked) stream_fail(err && err[0] ? err : "monitoring not checked");
    else if (s_v2_len) stream_fail("truncated block");
    else if (s_v2_seq_errors) stream_fail("stream seq errors");
    else if (actual_n != s_stream_got) stream_fail("size mismatch");
    else if (enc && sb_parse_u64_str(enc, &enc_n) && enc_n != s_stream_wire)
        stream_fail("wire mismatch");
    else if (cJSON_IsNumber(clen) && clen->valuedouble >= 0 &&
             (uint64_t)clen->valuedouble != actual_n)
        stream_fail("content-length mismatch");
    /* The LIST token is a ceil-rounded display size, not a byte count:
     * exact <= listed && listed - exact < unit (ezs_size_within_listing). */
    else if (s_stream_expected > 0 &&
             !(s_stream_got <= (uint64_t)s_stream_expected &&
               (uint64_t)s_stream_expected - s_stream_got <
               (s_stream_unit_shift ? (1ULL << s_stream_unit_shift) : 1)))
        stream_fail("expected size mismatch");
    else if (strcmp(sha, s_stream_sha) != 0)
        stream_fail("sha256 mismatch");
    else if (s_stream_tmp[0] && rename(s_stream_tmp, s_stream_path) == 0) ok = true;
    else stream_fail("publish failed");

    if (!ok && s_stream_tmp[0]) unlink(s_stream_tmp);
    s_stream_tmp[0] = '\0';
    s_stream_active = false;
    s_rx_armed = false;          /* post-END residue is stale, not data */
    if (!ok && s_stream_err[0])
        snprintf(s_last_error, sizeof(s_last_error), "%s", s_stream_err);
    return ok;
}

/* GET_FILE with v2 framing, retried on the bridge's transient errors. */
static esp_err_t sb_get_file(const char *directory, const char *name,
                             const char *expected_size)
{
    /* `directory` is already bridge-canonical ("A:\", "A:\SETTINGS",
     * "A:\DATALOG\<day>") — ezp_logical_to_canon requires the drive
     * prefix; a bare "/DATALOG/…" is PATH_INVALID. */
    char dir[64];
    snprintf(dir, sizeof(dir), "%s", directory);

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
        if (!body) {
            xSemaphoreGive(s_req_mtx);
            stream_fail("no mem");
            stream_terminate();
            return ESP_ERR_NO_MEM;
        }

        s_stream_rsp_rid = rid;
        s_rid = rid;
        free(s_stream_rsp_json);
        s_stream_rsp_json = NULL;
        xSemaphoreTake(s_stream_rsp_sem, 0);
        json_reset_rx();
        esp_err_t ret = ctrl_send(SB_KIND_JSON_REQ, rid, (const uint8_t *)body,
                                  strlen(body));
        free(body);
        if (ret != ESP_OK) {
            s_stream_rsp_rid = 0;
            xSemaphoreGive(s_req_mtx);
            stream_fail("send");
            stream_terminate();
            return ret;
        }

        /* The first response on this rid decides admission: the bridge
         * replies {"op":"STREAM_BEGIN","ok":true} and starts streaming,
         * or {"op":"GET_FILE","ok":false,"error":{"code":...}} when it
         * refuses (STALE day listing, BUSY teardown, therapy gates).
         * Treating the refusal as a BEGIN used to grant credit to a
         * stream that never started, then burn the whole attempt in the
         * dead-air wait below and misreport it as a stalled card fetch.
         * The wait wakes early on a terminal latch (e.g. link loss).   */
        cJSON *broot = NULL;
        const int64_t adm_end = esp_timer_get_time() +
                                (int64_t)SB_JSON_TIMEOUT * 1000;
        while (!s_stream_dead && esp_timer_get_time() < adm_end) {
            if (xSemaphoreTake(s_stream_rsp_sem, pdMS_TO_TICKS(250)) ==
                    pdTRUE && s_stream_rsp_json) {
                broot = cJSON_Parse(s_stream_rsp_json);
                free(s_stream_rsp_json);
                s_stream_rsp_json = NULL;
                break;
            }
        }
        if (s_stream_dead) {
            s_stream_rsp_rid = 0;
            xSemaphoreGive(s_req_mtx);
            stream_terminate();
            if (attempt < 2 && stream_retry_wait()) continue;
            return ESP_FAIL;
        }
        if (!broot) {
            s_stream_rsp_rid = 0;
            xSemaphoreGive(s_req_mtx);
            stream_fail("no STREAM_BEGIN");
            stream_terminate();
            if (attempt < 2 && stream_retry_wait()) goto retry;
            return ESP_ERR_TIMEOUT;
        }
        cJSON *bres = NULL;
        char bcode[32];
        const sb_rsp_kind_t bkind = sb_rsp_classify(broot, &bres, bcode,
                                                  sizeof(bcode));
        const char *bop = cJSON_GetStringValue(cJSON_GetObjectItem(broot, "op"));
        const bool admitted = bkind == SB_RSP_OK && bop &&
                              !strcmp(bop, "STREAM_BEGIN");
        /* Strict BEGIN fields: wire version 1|2 and a well-formed
         * size_unit_shift (0..24, the display unit the listing tuple was
         * ceil-rounded into — stream_end applies the within-listing
         * rule: listed == ceil(exact/unit)*unit, not an exact count).   */
        bool fields_ok = false;
        if (admitted) {
            const cJSON *wire = cJSON_GetObjectItem(bres, "wire");
            const cJSON *us = cJSON_GetObjectItem(bres, "size_unit_shift");
            if (cJSON_IsNumber(wire) &&
                (wire->valuedouble == 1 || wire->valuedouble == 2) &&
                cJSON_IsNumber(us) && us->valuedouble >= 0 &&
                us->valuedouble <= 24 &&
                us->valuedouble == (double)(int)us->valuedouble) {
                s_stream_wire_ver = (uint8_t)wire->valuedouble;
                s_stream_unit_shift = (uint8_t)us->valuedouble;
                fields_ok = true;
            }
        }
        cJSON_Delete(broot);
        if (!admitted || !fields_ok) {
            const char *why = !admitted
                ? ((bkind == SB_RSP_ERROR) ? bcode : "bad STREAM_BEGIN")
                : "bad STREAM_BEGIN";
            s_stream_rsp_rid = 0;
            xSemaphoreGive(s_req_mtx);
            stream_fail(why);
            stream_terminate();
            if (attempt < 2 && stream_retry_wait()) continue;
            return ESP_FAIL;
        }
        s_stream_admitted = true;
        if (stream_ack_initial(0, SB_WINDOW) != ESP_OK) {
            s_stream_rsp_rid = 0;
            xSemaphoreGive(s_req_mtx);
            stream_fail("initial credit");
            stream_terminate();
            if (attempt < 2 && stream_retry_wait()) goto retry;
            return ESP_FAIL;
        }
        /* Admission complete: data frames are accepted from here on.
         * Frames that arrived earlier are stale residue, not stream
         * data — the producer counted and dropped them.            */
        s_rx_armed = true;
        xSemaphoreGive(s_req_mtx);

        /* END arrives on the same rid once the body is complete. Wait
         * on wire progress, not a flat wall-clock: a wedged card read
         * emits no data at all, while a slow-but-alive fetch emits a
         * block per ~64 KB of raw input. Dead air kills the stream
         * early (and aborts the bridge fetch so it stops hogging the
         * card); overall SB_STREAM_TIMEOUT still caps total time.   */
        uint64_t last_wire = 0;
        int64_t last_prog = esp_timer_get_time();
        const int64_t deadline = last_prog + (int64_t)SB_STREAM_TIMEOUT * 1000;
        cJSON *root = NULL;
        for (;;) {
            /* Receive worker: decode/hash/write everything the callback
             * queued, then check the terminal latch BEFORE sleeping.   */
            stream_pump();
            if (s_stream_dead) {
                stream_terminate();
                s_stream_rsp_rid = 0;
                free(s_stream_rsp_json);
                s_stream_rsp_json = NULL;
                if (attempt < 2 && stream_retry_wait()) goto retry;
                return ESP_FAIL;
            }
            stream_ack_tick();
            if (xSemaphoreTake(s_stream_rsp_sem, pdMS_TO_TICKS(25)) ==
                    pdTRUE && s_stream_rsp_json) {
                root = cJSON_Parse(s_stream_rsp_json);
                free(s_stream_rsp_json);
                s_stream_rsp_json = NULL;
                /* A re-sent STREAM_BEGIN is reassembly armor, not the
                 * verdict — drop it and keep waiting (same rule as the
                 * reference client's collect loop).                  */
                const char *op = cJSON_GetStringValue(
                    cJSON_GetObjectItem(root, "op"));
                if (op && !strcmp(op, "STREAM_BEGIN")) {
                    cJSON_Delete(root);
                    root = NULL;
                    continue;
                }
                break;
            }
            int64_t now = esp_timer_get_time();
            if (s_stream_wire != last_wire) {
                last_wire = s_stream_wire;
                last_prog = now;
            } else if (now - last_prog >= SB_STREAM_STALL_US ||
                       now >= deadline) {
                stream_fail(now >= deadline ? "no STREAM_END"
                                            : "stream stalled");
                stream_terminate();
                s_stream_rsp_rid = 0;
                if (attempt < 2 && stream_retry_wait()) goto retry;
                return ESP_ERR_TIMEOUT;
            }
        }
        s_stream_rsp_rid = 0;
        /* The remote stream answered: no ABORT needed on failure paths
         * below — it is already over on the bridge side.  Drain the
         * ring tail the END raced ahead of, then judge.              */
        s_stream_admitted = false;
        stream_pump();
        if (s_stream_dead) {
            stream_terminate();
            if (attempt < 2 && stream_retry_wait()) goto retry;
            return ESP_FAIL;
        }
        /* Final right-edge credit: confirm everything the body carried
         * before the verdict is evaluated.                           */
        if (s_stream_wire > s_stream_acked)
            stream_ack(s_stream_wire, SB_WINDOW);
        if (!root) {
            stream_fail("bad STREAM_END");
            stream_terminate();
            return ESP_ERR_INVALID_RESPONSE;
        }

        cJSON *res = NULL;
        char ecode[32];
        const sb_rsp_kind_t ekind = sb_rsp_classify(root, &res, ecode,
                                                  sizeof(ecode));
        bool ok = false;
        if (ekind == SB_RSP_OK) {
            ok = stream_end(res);
        } else {
            stream_fail(ekind == SB_RSP_ERROR ? ecode : "bad STREAM_END");
        }
        cJSON_Delete(root);
        if (ok) {
            ESP_LOGI(TAG, "stream done %s wire=%llu raw=%llu "
                     "(rx=%u dup=%u stale=%u drop=%u end_frames=%lu)",
                     name, (unsigned long long)s_stream_wire,
                     (unsigned long long)s_stream_got,
                     (unsigned)s_rx_frames, (unsigned)s_dup_frames,
                     (unsigned)s_stale_frames, (unsigned)s_dq_drops,
                     (unsigned long)s_end_frames);
            return ESP_OK;
        }
        stream_terminate();
        if (attempt < 2 && stream_retry_wait()) continue;
        return ESP_FAIL;

    retry:
        continue;
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
    /* normal sync follows the configured "days to keep in sync" window
     * so bridge pull and upload depth can't diverge; full stays 365  */
    const int days = full ? SB_SYNC_DAYS_FULL : uploader_max_days();
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
        int day_fail = 0;
        for (size_t k = 0; k < sizeof(order) / sizeof(order[0]); k++) {
            cJSON_ArrayForEach(f, files) {
                const char *fn = cJSON_GetStringValue(cJSON_GetObjectItem(f, "name"));
                const char *size = cJSON_GetStringValue(cJSON_GetObjectItem(f, "size"));
                const char *ts = cJSON_GetStringValue(cJSON_GetObjectItem(f, "ts"));
                if (!fn || !size || !strstr(fn, order[k])) continue;
                if (ledger_seen(day, fn, size, ts)) continue;
                if (s_conn == BLE_HS_CONN_HANDLE_NONE) { cJSON_Delete(dres); return; }
                char fdir[48];
                snprintf(fdir, sizeof(fdir), "A:\\DATALOG\\%s", day);
                if (sb_get_file(fdir, fn, size) != ESP_OK) {
                    day_fail++;
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
        if (day_fail)
            ESP_LOGW(TAG, "sync: %s staged PARTIAL (%d day(s) done, %d file(s) failed)",
                     day, s_sync_days_done, day_fail);
        else
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
    s_card_wifi = cJSON_IsTrue(cJSON_GetObjectItem(res, "sta_connected"));
    const char *mode = cJSON_GetStringValue(cJSON_GetObjectItem(res, "mode"));
    if (mode) snprintf(s_card_mode, sizeof(s_card_mode), "%s", mode);
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

        /* Deferred security_initiate (see CONNECT handler): give the peer's
         * own connect-time HCI procedures a moment to finish first.
         * SomnoBridge requires LE Secure Connections + MITM (§3); a bridge
         * that already holds a *different* bond refuses this by design —
         * replacing the peer needs its physically confirmed FORGET_PEER
         * action, so surface the failure instead of sitting in "connecting"
         * forever. */
        if (s_sec_due_ms && now >= s_sec_due_ms) {
            s_sec_due_ms = 0;
            if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
                int rc = ble_gap_security_initiate(s_conn);
                if (rc != 0) {
                    ESP_LOGW(TAG, "security initiate failed: rc=%d (bond "
                                  "exists on the bridge? FORGET_PEER is "
                                  "required)", rc);
                    set_error("bridge refused pairing");
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
        /* GATT ops only once the link is encrypted and CCCDs are armed —
         * poll_status() blocks up to the JSON timeout when the response
         * can't come, which used to stall this loop (delaying the deferred
         * security_initiate past the link budget) and could mask the PIN
         * mailbox during pairing. */
        if (s_conn != BLE_HS_CONN_HANDLE_NONE && s_subscribed) {
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

static void subscribe_chars(void);

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t chr_val, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)conn; (void)arg;
    if (err->status == BLE_HS_EDONE) {
        if (s_ctrl_cccd || s_data_cccd) subscribe_chars();
        return 0;
    }
    if (err->status != 0 || !dsc) return 0;
    /* Find-Information returns declarations as well as descriptors, and
     * chr_val is just the discovery's start handle (NimBLE passes it
     * through verbatim) — so only entries whose UUID is the CCCD
     * (0x2902) count, classified by which characteristic's range they
     * sit in.  Anything else (the second char's declaration landed in
     * s_ctrl_cccd once) makes the subscribe write fail. */
    static const ble_uuid16_t UUID_CCCD = BLE_UUID16_INIT(0x2902);
    if (ble_uuid_cmp(&dsc->uuid.u, &UUID_CCCD.u) != 0) return 0;
    if (dsc->handle > s_ctrl_val && dsc->handle < s_data_val)
        s_ctrl_cccd = dsc->handle;
    else if (dsc->handle > s_data_val)
        s_data_cccd = dsc->handle;
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn; (void)arg;
    if (err->status == BLE_HS_EDONE) {
        if (s_ctrl_val && s_data_val) {
            /* One discovery over the whole tail of the service finds both
             * chars' CCCDs — a second call here always answered EBUSY
             * (NimBLE allows a single GATT client procedure per conn). */
            ble_gattc_disc_all_dscs(s_conn, s_ctrl_val, s_svc_end, on_dsc, NULL);
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

static int cccd_data_cb(uint16_t conn, const struct ble_gatt_error *err,
                        struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (err && err->status != 0) {
        ESP_LOGW(TAG, "data cccd write status=%d", err->status);
        /* Half-armed CCCDs are a dead link: reconnect retries cleanly. */
        sb_ble_disconnect();
    } else {
        s_subscribed = true;
        ESP_LOGI(TAG, "subscribed: ctrl indicate + data notify");
    }
    return 0;
}

static int cccd_ctrl_cb(uint16_t conn, const struct ble_gatt_error *err,
                        struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (err && err->status != 0) {
        ESP_LOGW(TAG, "ctrl cccd write status=%d", err->status);
        sb_ble_disconnect();
        return 0;
    }
    uint8_t ntf[2] = { 0x01, 0x00 };
    if (s_data_cccd)
        ble_gattc_write_flat(s_conn, s_data_cccd, ntf, 2, cccd_data_cb, NULL);
    return 0;
}

static void subscribe_chars(void)
{
    /* CCCD writes are themselves GATT procedures — chain them, issuing
     * both at once answers EBUSY on the second and data notifications
     * would never arrive. */
    uint8_t ind[2] = { 0x02, 0x00 };      /* indicate */
    uint8_t ntf[2] = { 0x01, 0x00 };      /* notify   */
    if (s_ctrl_cccd)
        ble_gattc_write_flat(s_conn, s_ctrl_cccd, ind, 2, cccd_ctrl_cb, NULL);
    else if (s_data_cccd)
        ble_gattc_write_flat(s_conn, s_data_cccd, ntf, 2, cccd_data_cb, NULL);
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
        /* Do NOT start encryption inside the CONNECT event.  The bridge
         * pushes its own connect-time HCI burst (LE Set Data Length, Set
         * PHY, conn-param update) in this same instant, and an LL_ENC_REQ
         * arriving while its controller is still processing those commands
         * wedges it — observed as "HCI wait for ack returned 19" on the
         * bridge followed by a NimBLE reset, which our side saw as
         * "encryption failed (status 520)" on every bonded reconnect.
         * BlueZ (and every peer that used to work) starts security lazily,
         * after its own discovery, so the wedge never showed.  Defer the
         * request to the task loop instead. */
        s_sec_due_ms = esp_timer_get_time() / 1000 + SB_SEC_DELAY_MS;
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        s_pin_pending = false;
        s_sec_due_ms = 0;
        /* reason 573 = HCI 0x3d "Connection Terminated due to MIC
         * Failure" — a detected link-integrity termination, not a plain
         * timeout; keep it distinct in the log. */
        struct ble_gap_conn_desc dd;
        const bool have_dd = ble_gap_conn_find(s_conn, &dd) == 0;
        ESP_LOGW(TAG, "disconnected (reason=%d%s enc=%d auth=%d bonded=%d)",
                 event->disconnect.reason,
                 event->disconnect.reason == 573 ? "/MIC" : "",
                 have_dd ? (int)dd.sec_state.encrypted : -1,
                 have_dd ? (int)dd.sec_state.authenticated : -1,
                 have_dd ? (int)dd.sec_state.bonded : -1);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        if (s_stream_active || s_rx_armed) stream_fail("link lost");
        s_subscribed = false;
        if (s_paired) set_state(SB_STATUS_CONNECTING);
        else set_state(SB_STATUS_IDLE);
        return 0;
    }

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
            s_subscribed = false;
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

        if (h == s_data_val) {
            /* Stream data: copy the (possibly chained) mbuf into the
             * receive ring and return — decode/hash/SD writes run on the
             * bridge task via stream_pump(), never here.  Only frames
             * for an admitted stream are accepted; anything else is
             * residue of a torn-down stream: counted, not processed.  A
             * loss point inside this path (ring full, flatten failure,
             * malformed frame) is terminal — the bytes are already gone
             * and a spliced stream must not continue.              */
            if (!s_rx_armed) { s_stale_frames++; return 0; }
            if (len < 4 || len > (uint16_t)sizeof(s_dq[0].data)) {
                s_dq_drops++;
                stream_fail("rx drop");
                return 0;
            }
            /* The armed check lives inside the lock so a frame can never
             * commit after dq_reset() cleared the ring: armed is cleared
             * first, then reset — under the same critical section an
             * in-flight copy sees disarmed and bails.  The flatten is a
             * bounded memcpy of ≤252 B, safe inside the spinlock.     */
            portENTER_CRITICAL(&s_dq_mux);
            const uint16_t next = (uint16_t)((s_dq_head + 1) % SB_DQ_N);
            if (!s_rx_armed) {
                portEXIT_CRITICAL(&s_dq_mux);
                s_stale_frames++;
                return 0;
            }
            if (next == s_dq_tail) {
                portEXIT_CRITICAL(&s_dq_mux);
                s_dq_drops++;
                stream_fail("rx overflow");
                return 0;
            }
            uint16_t got = 0;
            if (ble_hs_mbuf_to_flat(event->notify_rx.om,
                                    s_dq[s_dq_head].data, len, &got) != 0 ||
                got != len) {
                portEXIT_CRITICAL(&s_dq_mux);
                s_dq_drops++;
                stream_fail("rx flatten");
                return 0;
            }
            s_dq[s_dq_head].len = len;
            s_dq_head = next;
            portEXIT_CRITICAL(&s_dq_mux);
            return 0;
        }

        /* Control characteristic: the 16-byte header is mandatory. */
        if (len < SB_HDR_LEN) return 0;
        uint8_t *all = malloc(len);
        if (!all) return 0;
        uint16_t got = 0;
        if (ble_hs_mbuf_to_flat(event->notify_rx.om, all, len, &got) != 0 ||
            got != len) {
            free(all);
            return 0;
        }
        uint8_t kind;
        uint16_t flags;
        uint32_t rid;
        uint64_t off;
        if (!sb_hdr_parse(all, SB_HDR_LEN, &kind, &flags, &rid, &off)) {
            free(all);
            return 0;
        }
        const size_t plen = len - SB_HDR_LEN;
        uint8_t *payload = all + SB_HDR_LEN;

        if (kind == SB_KIND_TIME_RSP) {
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

    /* Receive ring: covers a full credit window plus headroom (~24 KiB
     * PSRAM) so the bridge can never overrun the receiver while credits
     * are honoured — a full ring is a local-loss event, not backpressure. */
    s_dq = heap_caps_malloc(sizeof(sb_dq_item_t) * SB_DQ_N,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_dq) s_dq = malloc(sizeof(sb_dq_item_t) * SB_DQ_N);
    if (!s_dq) return ESP_ERR_NO_MEM;

    s_req_mtx = xSemaphoreCreateMutex();
    s_write_mtx = xSemaphoreCreateMutex();
    s_resp_sem = xSemaphoreCreateBinary();
    s_stream_rsp_sem = xSemaphoreCreateBinary();
    s_write_done = xSemaphoreCreateBinary();
    s_pin_sem = xSemaphoreCreateBinary();
    s_pin_mbox = xSemaphoreCreateBinary();
    s_sync_mtx = xSemaphoreCreateMutex();
    if (!s_req_mtx || !s_write_mtx || !s_resp_sem || !s_write_done ||
        !s_pin_sem || !s_sync_mtx || !s_pin_mbox || !s_stream_rsp_sem)
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

static esp_err_t forget_nvs_op(void *arg)
{
    (void)arg;
    nvs_handle_t h;
    esp_err_t err = nvs_open(SB_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, SB_NVS_ADDR);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t forget_bond_op(void *arg)
{
    const ble_addr_t *peer = arg;
    if (peer->type != BLE_ADDR_PUBLIC && peer->type != BLE_ADDR_RANDOM)
        return ESP_OK;
    ble_store_util_delete_peer(peer);
    return ESP_OK;
}

esp_err_t sb_ble_forget(void)
{
    /* NVS + bond-store writes must run on an internal-RAM task: this API is
     * invoked by the httpd worker, which lives on a PSRAM stack — writing
     * flash from there panics (observed as reset reason PANIC on Unpair).
     * Delete only *this* peer's bond too — ble_store_clear() would wipe the
     * AS11's bond along with it. */
    s_paired = false;
    ble_addr_t peer = s_peer;
    memset(&s_peer, 0, sizeof(s_peer));
    nvs_writer_run(forget_nvs_op, NULL);
    nvs_writer_run(forget_bond_op, &peer);
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

/* Generic JSON op passthrough for the portal's bridge-config dialog
 * (GET_CONFIG/SET_CONFIG/WIFI_SCAN/EZSHARE_CHECK). Builds
 * {"ok":true,"result":{...}} / {"ok":false,"error":{...}} so the UI can
 * render the bridge reply verbatim. Caller frees *out_json. */
esp_err_t sb_ble_rpc(const char *op, const cJSON *args, uint32_t timeout_ms,
                     char **out_json)
{
    if (!op || !op[0] || !out_json) return ESP_ERR_INVALID_ARG;
    *out_json = NULL;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *res = NULL, *err = NULL;
    esp_err_t ret = json_request(op, args ? cJSON_Duplicate(args, 1) : NULL,
                                 NULL, &res, &err, timeout_ms);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", ret == ESP_OK);
    if (ret == ESP_OK) {
        if (res) cJSON_AddItemToObject(out, "result", res);
        else cJSON_AddObjectToObject(out, "result");
    } else {
        if (err) cJSON_AddItemToObject(out, "error", err);
        else {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "code",
                                    ret == ESP_ERR_TIMEOUT ? "TIMEOUT"
                                                           : "LINK_ERROR");
            cJSON_AddItemToObject(out, "error", e);
        }
        cJSON_Delete(res);
    }
    *out_json = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (!*out_json) return ESP_ERR_NO_MEM;
    return ret;
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
    cJSON_AddBoolToObject(o, "card_wifi", s_card_wifi);
    if (s_card_mode[0]) cJSON_AddStringToObject(o, "card_mode", s_card_mode);
    cJSON_AddBoolToObject(o, "sync_running", s_sync_running);
    cJSON_AddNumberToObject(o, "mtu", s_mtu);
    if (s_error[0]) cJSON_AddStringToObject(o, "error", s_error);
    if (s_last_error[0]) cJSON_AddStringToObject(o, "last_error", s_last_error);
    cJSON_AddNumberToObject(o, "last_sync_s", s_last_sync_ms / 1000);
    cJSON_AddNumberToObject(o, "sync_days", s_sync_days_done);
    cJSON_AddNumberToObject(o, "sync_files", s_sync_files_done);
    /* live stream counters — lets the UI/diag tell a slow fetch from
     * a dead wire while a pull is in flight                        */
    if (s_stream_f || s_stream_wire || s_stream_got) {
        cJSON_AddNumberToObject(o, "stream_wire", (double)s_stream_wire);
        cJSON_AddNumberToObject(o, "stream_raw", (double)s_stream_got);
    }
    return o;
}
