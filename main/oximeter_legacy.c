/*
 * SomnoTrace - O2 Ring (Legacy/Gen1) BLE protocol codec and session
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
 *
 * Clean-room Legacy BLE protocol for Wellue O2 Ring (Gen1) / ViaTom rings.
 * Protocol studied from published documentation; no third-party source
 * code copied.
 */

#include "oximeter.h"
#include "oximeter_internal.h"
#include "sd_storage.h"
#include "as11_ble.h"
#include "psram_task.h"
#include "nvs_writer.h"
#include "oximetry_canonical.h"
#include "time_sync.h"
#include "upload_sched.h"
#include "log_stream.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

static const char *TAG = "ox_legacy";

/* ── Store forward declarations (oximeter_store.c) ─────────────────── */
void ox_store_ensure_dirs(void);
bool ox_store_load_paired(char *serial, size_t serial_sz,
                          char *firmware, size_t fw_sz,
                          char *name_prefix, size_t prefix_sz,
                          char *last_addr, size_t addr_sz,
                          char *driver, size_t driver_sz,
                          char *ble_name, size_t ble_name_sz);
void ox_store_save_paired(const char *serial, const char *firmware,
                          const char *name_prefix, const char *last_addr,
                          const char *driver, const char *ble_name);
void ox_store_delete_paired(void);
int  ox_store_index_check(const char *serial, const char *name);
int  ox_store_index_conversion_check(const char *serial, const char *name);
void ox_store_index_add(const char *serial, const char *name,
                        uint32_t bytes, bool finalised);
void ox_store_index_mark_converted(const char *serial, const char *name,
                                   bool converted, const char *error);
long ox_store_part_size(const char *name);
esp_err_t ox_store_part_append(const char *name, const uint8_t *data, size_t len);
bool ox_store_promote_vld3(const char *serial, const char *name);
bool ox_store_finalize_native(const char *serial, const char *name,
                              long declared_size);
void ox_store_part_remove(const char *name);

/* ── Legacy protocol constants ──────────────────────────────────────── */
#define LEGACY_REQ_LEAD     0xAA
#define LEGACY_RSP_LEAD     0x55
#define LEGACY_HEADER_LEN   7   /* sync | cmd | cmd^0xFF | block(2) | len(2) */
#define LEGACY_MAX_FRAME    2048
#define LEGACY_BLE_CHUNK    20  /* max bytes per BLE write */

/* Command codes */
#define CMD_FILE_OPEN       0x03
#define CMD_FILE_READ       0x04
#define CMD_FILE_CLOSE      0x05
#define CMD_INFO            0x14
#define CMD_PING            0x15
#define CMD_CONFIG          0x16

/* VLD3 file format constants */
#define VLD3_HEADER_LEN     40
#define VLD3_RECORD_LEN     5
#define VLD3_NO_FINGER      0xFF

/* Legacy GATT UUIDs (128-bit, stored little-endian for NimBLE).
 * Service:  14839ac4-7d7e-415c-9a42-167340cf2339
 * Write:    8b00ace7-eb0b-49b0-bbe9-9aee0a26e1a3
 * Notify:   0734594a-a8e7-4b1a-a6b1-cd5243059a57 */
static const ble_uuid128_t LEGACY_SVC_UUID =
    BLE_UUID128_INIT(0x39, 0x23, 0xcf, 0x40, 0x73, 0x16, 0x42, 0x9a,
                     0x5c, 0x41, 0x7e, 0x7d, 0xc4, 0x9a, 0x83, 0x14);
static const ble_uuid128_t LEGACY_WRITE_UUID =
    BLE_UUID128_INIT(0xa3, 0xe1, 0x26, 0x0a, 0xee, 0x9a, 0xe9, 0xbb,
                     0xb0, 0x49, 0x0b, 0xeb, 0xe7, 0xac, 0x00, 0x8b);
static const ble_uuid128_t LEGACY_NOTIFY_UUID =
    BLE_UUID128_INIT(0x57, 0x9a, 0x05, 0x43, 0x52, 0xcd, 0xb1, 0xa6,
                     0x1a, 0x4b, 0xe7, 0xa8, 0x4a, 0x59, 0x34, 0x07);

/* ── CRC8 (poly=0x07, init=0) — same as OxyII ──────────────────────── */
static uint8_t legacy_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x07;
            else            crc <<= 1;
        }
    }
    return crc;
}

/* ── Frame codec ───────────────────────────────────────────────────── */
/* Encode a Legacy request frame into buf.  Returns total frame length.
 * Request: 0xAA | cmd | cmd^0xFF | block(LE16) | len(LE16) | data | crc8 */
static int legacy_encode(uint8_t *buf, int bufsz, uint8_t cmd,
                         uint16_t block,
                         const uint8_t *payload, int payload_len)
{
    int total = LEGACY_HEADER_LEN + payload_len + 1;
    if (total > bufsz) return -1;

    buf[0] = LEGACY_REQ_LEAD;
    buf[1] = cmd;
    buf[2] = cmd ^ 0xFF;
    buf[3] = block & 0xFF;
    buf[4] = (block >> 8) & 0xFF;
    buf[5] = payload_len & 0xFF;
    buf[6] = (payload_len >> 8) & 0xFF;
    if (payload && payload_len > 0)
        memcpy(buf + 7, payload, payload_len);
    buf[total - 1] = legacy_crc8(buf, total - 1);
    return total;
}

/* Try to decode a Legacy response frame from buf.
 * Response: 0x55 | status | status^0xFF | block(LE16) | len(LE16) | data | crc8
 * Returns total frame length on success, -1 if incomplete, -2 if invalid. */
static int legacy_try_decode(const uint8_t *buf, int len,
                              uint8_t *status, uint16_t *block,
                              uint8_t *payload, int *payload_len,
                              int payload_cap)
{
    if (len < LEGACY_HEADER_LEN) return -1;
    if (buf[0] != LEGACY_RSP_LEAD) return -2;
    if ((uint8_t)(buf[1] ^ 0xFF) != buf[2]) return -2;

    int plen = buf[5] | (buf[6] << 8);
    int total = LEGACY_HEADER_LEN + plen + 1;
    if (len < total) return -1;

    if (legacy_crc8(buf, total - 1) != buf[total - 1]) return -2;

    if (status) *status = buf[1];
    if (block)  *block = buf[3] | (buf[4] << 8);
    if (payload && payload_cap > 0) {
        int n = plen < payload_cap ? plen : payload_cap;
        memcpy(payload, buf + 7, n);
    }
    if (payload_len) *payload_len = plen;
    return total;
}

/* ── Module state ──────────────────────────────────────────────────── */
#define OX_SCAN_MAX 16

struct ox_scan_result {
    ble_addr_t addr;
    char name[32];
    int rssi;
};

/* Argument passed to pair_task (must be freed by the task). */
struct pair_arg {
    char addr_str[24];
    char ble_name[40];
};

static SemaphoreHandle_t s_state_mtx;
static SemaphoreHandle_t s_ops_mtx;
static SemaphoreHandle_t s_op_sem;
static SemaphoreHandle_t s_conn_sem;
static SemaphoreHandle_t s_resp_sem;
static SemaphoreHandle_t s_scan_done;
static volatile int s_op_status;
static volatile int s_conn_status;
static bool s_initialized;

static char s_status[24] = OX_STATUS_IDLE;
static char s_error[128];

static char s_serial[32];
static char s_firmware[16];
static char s_name_prefix[16];
static char s_ble_name[40];      /* BLE advertised name or constructed display name */
static char s_paired_addr[18];
static bool s_paired = false;
static bool s_presence_served = false;
static bool s_synced_this_idle = false;
static bool s_ring_present = false;
static TickType_t s_served_at;
static int s_pull_fail_count = 0;   /* consecutive failed pulls in this presence */
static int s_connect_fail_count = 0; /* consecutive connect failures in this presence */
#define OX_PULL_MAX_FAST_RETRIES 3  /* quick retries before applying curfew */
#define OX_CONNECT_MAX_RETRIES 3   /* connect failures before cooldown curfew */

/* BLE connection state */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_connect_event_handle = BLE_HS_CONN_HANDLE_NONE; /* saved in CONNECT event before DISCONNECT can overwrite */
static uint16_t s_write_handle;
static uint16_t s_notify_handle;
static uint16_t s_cccd_handle;
static uint16_t s_svc_start, s_svc_end;

/* Scan state */
static struct ox_scan_result *s_scan;
static int s_scan_count;

/* Notification accumulation buffer — PSRAM-allocated at init */
static uint8_t *s_resp_buf;
static int s_resp_len;
static uint8_t s_resp_status;
static uint16_t s_resp_block;
static uint8_t *s_resp_payload;
static int s_resp_payload_len;

/* ── Helpers ───────────────────────────────────────────────────────── */
static void set_state(const char *st)
{
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    strlcpy(s_status, st, sizeof(s_status));
    xSemaphoreGive(s_state_mtx);
    log_stream_request_ox_push();
}

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    vsnprintf(s_error, sizeof(s_error), fmt, ap);
    strlcpy(s_status, OX_STATUS_ERROR, sizeof(s_status));
    xSemaphoreGive(s_state_mtx);
    va_end(ap);
}

static void clear_op_sem(void)
{
    while (xSemaphoreTake(s_op_sem, 0) == pdTRUE) { }
}

static int wait_op(int timeout_ms)
{
    if (xSemaphoreTake(s_op_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return BLE_HS_ETIMEOUT;
    return s_op_status;
}

/* Check if raw BLE advertisement data contains the Gen1 Legacy service UUID
 * 14839ac4-7d7e-415c-9a42-167340cf2339.
 * In BLE AD data, 128-bit service UUIDs are stored as 16-byte LE sequences
 * under AD type 0x06 (incomplete) or 0x07 (complete). */
static const uint8_t LEGACY_SVC_UUID_LE[16] = {
    0xc4, 0x9a, 0x83, 0x14, 0x7e, 0x7d, 0x5c, 0x41,
    0x42, 0x9a, 0x73, 0x16, 0xcf, 0x40, 0x23, 0x39
};

static bool adv_has_legacy_service_uuid(const uint8_t *raw, int raw_len)
{
    for (int off = 0; off + 1 < raw_len; ) {
        uint8_t ad_len = raw[off];
        if (ad_len == 0 || off + 1 + ad_len > raw_len) break;
        uint8_t ad_type = raw[off + 1];
        const uint8_t *ad_data = raw + off + 2;
        int ad_data_len = ad_len - 1;

        if ((ad_type == 0x06 || ad_type == 0x07) && ad_data_len >= 16) {
            for (int i = 0; i + 16 <= ad_data_len; i += 16) {
                if (memcmp(ad_data + i, LEGACY_SVC_UUID_LE, 16) == 0)
                    return true;
            }
        }
        off += 1 + ad_len;
    }
    return false;
}

static bool name_is_legacy(const char *name)
{
    if (!name || !name[0]) return false;
    char up[32];
    int i;
    for (i = 0; i < 31 && name[i]; i++)
        up[i] = toupper((unsigned char)name[i]);
    up[i] = '\0';

    /* Explicitly exclude Gen2 OxyII names */
    if (strncmp(up, "S8-AW", 5) == 0 ||
        strncmp(up, "SHQO2PRO", 8) == 0 ||
        strncmp(up, "T8520", 5) == 0)
        return false;

    if (strstr(up, "O2") != NULL ||
        strstr(up, "CMRING") != NULL ||
        strstr(up, "KIDSO2") != NULL ||
        strstr(up, "CHECKME") != NULL ||
        strstr(up, "SLEEPU") != NULL)
        return true;

    return false;
}

static void addr_to_str(const ble_addr_t *a, char *out, size_t outsz)
{
    snprintf(out, outsz, "%02x:%02x:%02x:%02x:%02x:%02x",
             a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
}

static bool parse_addr(const char *str, ble_addr_t *out)
{
    unsigned int v[6];
    int n = sscanf(str, "%x:%x:%x:%x:%x:%x",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    if (n != 6) return false;
    out->val[5] = v[0]; out->val[4] = v[1]; out->val[3] = v[2];
    out->val[2] = v[3]; out->val[1] = v[4]; out->val[0] = v[5];
    out->type = (v[0] & 0xC0) == 0xC0 ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
    return true;
}

/* Find the scan result whose address matches s_paired_addr.
 * Returns the index, or -1 if not found. */
static int find_paired_in_scan(void)
{
    for (int i = 0; i < s_scan_count; i++) {
        char addr_str[18];
        addr_to_str(&s_scan[i].addr, addr_str, sizeof(addr_str));
        if (strcmp(addr_str, s_paired_addr) == 0)
            return i;
    }
    return -1;
}

/* ── GAP event handler ─────────────────────────────────────────────── */
static int gap_event(struct ble_gap_event *event, void *arg);

static void handle_notify_rx(const uint8_t *data, int len)
{
    if (s_resp_len + len > LEGACY_MAX_FRAME) {
        ESP_LOGW(TAG, "notify overflow: resp_len=%d + %d > %d",
                 s_resp_len, len, LEGACY_MAX_FRAME);
        s_resp_len = 0;
    }
    memcpy(s_resp_buf + s_resp_len, data, len);
    s_resp_len += len;

    /* Try to decode as many complete frames as are in the buffer. */
    while (s_resp_len > 0) {
        uint8_t status;
        uint16_t block;
        int plen;
        int rc = legacy_try_decode(s_resp_buf, s_resp_len, &status, &block,
                                   s_resp_payload, &plen, LEGACY_MAX_FRAME);
        if (rc > 0) {
            s_resp_status = status;
            s_resp_block = block;
            s_resp_payload_len = plen;
            /* Shift leftover bytes to front */
            int remaining = s_resp_len - rc;
            if (remaining > 0)
                memmove(s_resp_buf, s_resp_buf + rc, remaining);
            s_resp_len = remaining;
            xSemaphoreGive(s_resp_sem);
            return;
        }
        if (rc == -1) {
            /* Incomplete frame — wait for more data */
            return;
        }
        /* rc == -2: invalid frame — drop one byte and try to resync
         * on the next 0x55 lead byte */
        ESP_LOGW(TAG, "notify decode error at offset 0, resyncing");
        int remaining = s_resp_len - 1;
        if (remaining > 0)
            memmove(s_resp_buf, s_resp_buf + 1, remaining);
        s_resp_len = remaining;
    }
}

/* Decode HCI disconnect reason to a human-readable string for logging.
 * NimBLE wraps HCI error codes with BLE_HS_EHCI (0x200), so we mask. */
static const char *hci_err_str(int reason)
{
    switch (reason & 0xFF) {
    case 0x08: return "Conn Already Exists";
    case 0x13: return "Remote User Terminated";
    case 0x16: return "Supervision Timeout";
    case 0x22: return "LMP Response Timeout";
    case 0x28: return "Instant Passed";
    case 0x3B: return "Unacceptable Connection Parameters";
    case 0x44: return "Conn Fail to Be Established";
    default:   return "Unknown";
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        char addr_str[18];
        addr_to_str(&event->disc.addr, addr_str, sizeof(addr_str));

        struct ble_hs_adv_fields f;
        char name[32] = {0};
        const uint8_t *raw = event->disc.data;
        int raw_len = event->disc.length_data;

        memset(&f, 0, sizeof(f));
        if (ble_hs_adv_parse_fields(&f, raw, raw_len) != 0) {
            for (int off = 0; off + 1 < raw_len; ) {
                uint8_t ad_len = raw[off];
                if (ad_len == 0 || off + 1 + ad_len > raw_len) break;
                uint8_t ad_type = raw[off + 1];
                const uint8_t *ad_data = raw + off + 2;
                int ad_data_len = ad_len - 1;
                if (ad_type == 0x09 || ad_type == 0x08) {
                    int nl = ad_data_len < 31 ? ad_data_len : 31;
                    memcpy(name, ad_data, nl);
                    name[nl] = '\0';
                } else if (ad_type == 0xFF && ad_data_len >= 2) {
                    f.mfg_data = ad_data;
                    f.mfg_data_len = ad_data_len;
                }
                off += 1 + ad_len;
            }
        } else {
            if (f.name && f.name_len > 0) {
                int nl = f.name_len < 31 ? f.name_len : 31;
                memcpy(name, f.name, nl);
                name[nl] = '\0';
            }
        }

        uint16_t cid = 0;
        if (f.mfg_data && f.mfg_data_len >= 2)
            cid = f.mfg_data[0] | (f.mfg_data[1] << 8);

        /* Explicitly exclude Gen2 OxyII sync advertisements (0xF34E) */
        if (cid == 0xF34E)
            return 0;

        /* Match Gen1 rings using multi-criteria matching:
         * 1. 128-bit Legacy Service UUID in advertisement data (14839ac4-...)
         * 2. Known paired MAC address match (resilient across name/UUID variations)
         * 3. Gen1 device name pattern (with Gen2 name/mfg exclusions) */
        bool is_legacy = adv_has_legacy_service_uuid(raw, raw_len) ||
                         (s_paired && s_paired_addr[0] != '\0' && strcmp(addr_str, s_paired_addr) == 0) ||
                         name_is_legacy(name);
        if (!is_legacy) return 0;
        if (name[0] == '\0')
            strlcpy(name, "O2Ring", sizeof(name));

        /* Dedupe by address */
        for (int i = 0; i < s_scan_count; i++) {
            if (memcmp(&s_scan[i].addr, &event->disc.addr,
                       sizeof(ble_addr_t)) == 0) {
                s_scan[i].rssi = event->disc.rssi;
                if (name[0])
                    strlcpy(s_scan[i].name, name, sizeof(s_scan[i].name));
                return 0;
            }
        }
        if (s_scan_count < OX_SCAN_MAX) {
            s_scan[s_scan_count].addr = event->disc.addr;
            strlcpy(s_scan[s_scan_count].name, name,
                    sizeof(s_scan[s_scan_count].name));
            s_scan[s_scan_count].rssi = event->disc.rssi;
            s_scan_count++;
            ESP_LOGD(TAG, "scan: '%s' rssi=%d addr=%s",
                     name, event->disc.rssi, addr_str);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        xSemaphoreGive(s_scan_done);
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        s_connect_event_handle = event->connect.conn_handle;
        s_conn_handle = event->connect.conn_handle;
        s_conn_status = event->connect.status;
        ESP_LOGD(TAG, "gap CONNECT: handle=%d status=%d",
                 event->connect.conn_handle, event->connect.status);
        xSemaphoreGive(s_conn_sem);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason=%d / 0x%02X %s)",
                 event->disconnect.reason,
                 event->disconnect.reason & 0xFF,
                 hci_err_str(event->disconnect.reason));
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        xSemaphoreGive(s_resp_sem);
        xSemaphoreGive(s_conn_sem);
        return 0;

    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
        ESP_LOGD(TAG, "accepting L2CAP connection parameter update request");
        return 0;  /* 0 = accept */

    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "conn params updated: itvl=%d (%.1fms) latency=%d timeout=%d (%dms)",
                     desc.conn_itvl, desc.conn_itvl * 1.25f,
                     desc.conn_latency,
                     desc.supervision_timeout, desc.supervision_timeout * 10);
        }
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (event->notify_rx.conn_handle != s_conn_handle)
            return 0;
        int len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len <= 0) return 0;
        uint8_t *data = malloc(len);
        if (!data) return 0;
        os_mbuf_copydata(event->notify_rx.om, 0, len, data);
        handle_notify_rx(data, len);
        free(data);
        return 0;
    }
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU: %d", event->mtu.value);
        return 0;
    default:
        return 0;
    }
}

/* ── GATT discovery callbacks ──────────────────────────────────────── */
static int on_disc_svc(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn; (void)arg;
    if (err && err->status == 0 && svc) {
        s_svc_start = svc->start_handle;
        s_svc_end = svc->end_handle;
    }
    if (err && (err->status == BLE_HS_EDONE || err->status != 0)) {
        s_op_status = (err->status == BLE_HS_EDONE) ? 0 : err->status;
        xSemaphoreGive(s_op_sem);
    }
    return 0;
}

static int on_disc_chr(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn; (void)arg;
    if (err && err->status == 0 && chr) {
        if (ble_uuid_cmp(&chr->uuid.u, &LEGACY_WRITE_UUID.u) == 0)
            s_write_handle = chr->val_handle;
        else if (ble_uuid_cmp(&chr->uuid.u, &LEGACY_NOTIFY_UUID.u) == 0)
            s_notify_handle = chr->val_handle;
    }
    if (err && (err->status == BLE_HS_EDONE || err->status != 0)) {
        s_op_status = (err->status == BLE_HS_EDONE) ? 0 : err->status;
        xSemaphoreGive(s_op_sem);
    }
    return 0;
}

static int on_disc_dsc(uint16_t conn, const struct ble_gatt_error *err,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                       void *arg)
{
    (void)conn; (void)chr_val_handle; (void)arg;
    if (err && err->status == 0 && dsc) {
        const ble_uuid16_t cccd = BLE_UUID16_INIT(0x2902);
        if (ble_uuid_cmp(&dsc->uuid.u, &cccd.u) == 0 && s_cccd_handle == 0)
            s_cccd_handle = dsc->handle;
    }
    if (err && (err->status == BLE_HS_EDONE || err->status != 0)) {
        s_op_status = (err->status == BLE_HS_EDONE) ? 0 : err->status;
        xSemaphoreGive(s_op_sem);
    }
    return 0;
}

static int on_write_done(uint16_t conn, const struct ble_gatt_error *err,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    s_op_status = err ? err->status : 0;
    xSemaphoreGive(s_op_sem);
    return 0;
}

/* ── Connect and discover GATT services ────────────────────────────── */
static esp_err_t do_connect_and_discover(ble_addr_t *target)
{
    s_write_handle = s_notify_handle = s_cccd_handle = 0;
    s_svc_start = s_svc_end = 0;
    s_resp_len = 0;

    /* Drain any stale s_conn_sem token left by a previous remote
     * disconnect.  Without this, xSemaphoreTake below returns
     * immediately on the stale token and we proceed with a dead
     * handle (BLE_HS_CONN_HANDLE_NONE), causing every subsequent
     * GATT operation to fail with "Legacy service not found". */
    while (xSemaphoreTake(s_conn_sem, 0) == pdTRUE) { }
    s_conn_status = -1;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_connect_event_handle = BLE_HS_CONN_HANDLE_NONE;

    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(target->type, &own_addr_type);
    if (rc != 0) {
        set_error("addr infer failed: %d", rc);
        return ESP_FAIL;
    }
    clear_op_sem();
    rc = ble_gap_connect(own_addr_type, target,
                         15000, NULL, gap_event, NULL);
    if (rc != 0) { set_error("connect start failed: %d", rc); return ESP_FAIL; }
    if (xSemaphoreTake(s_conn_sem, pdMS_TO_TICKS(16000)) != pdTRUE) {
        set_error("connect timeout"); return ESP_FAIL;
    }
    if (s_conn_status != 0 || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        /* Race: the ring connected and immediately disconnected (e.g.
         * 0x3B "Unacceptable Connection Parameters") before we woke up.
         * s_conn_handle was overwritten to NONE by the DISCONNECT event.
         * Terminate the orphaned connection using the handle saved in
         * the CONNECT event, so it doesn't linger for ~98s until the
         * ring's internal timeout fires. */
        if (s_conn_status == 0 && s_connect_event_handle != BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGW(TAG, "connect/disconnect race — terminating orphaned handle=%d",
                     s_connect_event_handle);
            ble_gap_terminate(s_connect_event_handle, BLE_ERR_REM_USER_CONN_TERM);
            xSemaphoreTake(s_conn_sem, pdMS_TO_TICKS(2000));
        }
        set_error("connect failed: status=%d handle=%d",
                  s_conn_status, s_conn_handle);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "connected, handle=%d", s_conn_handle);

    /* Log negotiated connection parameters for diagnostics. */
    struct ble_gap_conn_desc cdesc;
    if (ble_gap_conn_find(s_conn_handle, &cdesc) == 0) {
        ESP_LOGI(TAG, "conn params: itvl=%d (%.1fms) latency=%d timeout=%d (%dms)",
                 cdesc.conn_itvl, cdesc.conn_itvl * 1.25f,
                 cdesc.conn_latency,
                 cdesc.supervision_timeout, cdesc.supervision_timeout * 10);
    }

    /* No MTU exchange needed for Legacy (20-byte writes are default). */

    /* Discover Legacy service by UUID */
    clear_op_sem();
    rc = ble_gattc_disc_svc_by_uuid(s_conn_handle, &LEGACY_SVC_UUID.u,
                                     on_disc_svc, NULL);
    if (rc != 0 || wait_op(10000) != 0) {
        set_error("Legacy service not found"); return ESP_FAIL;
    }
    if (s_svc_start == 0) { set_error("service range empty"); return ESP_FAIL; }
    ESP_LOGI(TAG, "service: 0x%04x-0x%04x", s_svc_start, s_svc_end);

    /* Discover characteristics */
    clear_op_sem();
    rc = ble_gattc_disc_all_chrs(s_conn_handle, s_svc_start, s_svc_end,
                                 on_disc_chr, NULL);
    if (rc != 0 || wait_op(10000) != 0) {
        set_error("characteristic discovery failed"); return ESP_FAIL;
    }
    if (s_write_handle == 0 || s_notify_handle == 0) {
        set_error("write/notify char not found"); return ESP_FAIL;
    }
    ESP_LOGI(TAG, "write=%d notify=%d", s_write_handle, s_notify_handle);

    /* Discover CCCD for notify characteristic */
    clear_op_sem();
    rc = ble_gattc_disc_all_dscs(s_conn_handle, s_notify_handle, s_svc_end,
                                 on_disc_dsc, NULL);
    if (rc != 0 || wait_op(10000) != 0) {
        set_error("CCCD discovery failed"); return ESP_FAIL;
    }
    if (s_cccd_handle == 0) { set_error("CCCD not found"); return ESP_FAIL; }

    /* Enable notifications */
    uint8_t cccd_val[2] = { 0x01, 0x00 };
    clear_op_sem();
    rc = ble_gattc_write_flat(s_conn_handle, s_cccd_handle,
                              cccd_val, 2, on_write_done, NULL);
    if (rc != 0 || wait_op(5000) != 0) {
        set_error("enable notify failed"); return ESP_FAIL;
    }
    ESP_LOGI(TAG, "notifications enabled (cccd=%d)", s_cccd_handle);
    return ESP_OK;
}

static void do_disconnect(void)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        xSemaphoreTake(s_conn_sem, pdMS_TO_TICKS(3000));
    }
}

/* ── Protocol request/response ─────────────────────────────────────── */
/* Send a Legacy request and optionally wait for a response.
 * Uses write-with-response (Legacy protocol uses WRITE_REQ, not WRITE_CMD).
 * Frames larger than 20 bytes are split into multiple writes. */
static esp_err_t legacy_request(uint8_t cmd, uint16_t block,
                                 const uint8_t *payload, int plen,
                                 bool expect_reply, int timeout_ms)
{
    uint8_t frame[LEGACY_MAX_FRAME];
    int flen = legacy_encode(frame, sizeof(frame), cmd, block,
                              payload, plen);
    if (flen < 0) return ESP_FAIL;

    if (expect_reply) {
        while (xSemaphoreTake(s_resp_sem, 0) == pdTRUE) { }
        s_resp_len = 0;
    }

    /* Write the frame in 20-byte chunks using write-with-response for
     * every chunk.  The Legacy protocol requires WRITE_REQUEST for each
     * chunk; using WRITE_CMD for subsequent chunks can cause drops or
     * ordering issues on rings with small ATT buffers. */
    int offset = 0;
    while (offset < flen) {
        int chunk = flen - offset;
        if (chunk > LEGACY_BLE_CHUNK) chunk = LEGACY_BLE_CHUNK;

        clear_op_sem();
        int rc = ble_gattc_write_flat(s_conn_handle, s_write_handle,
                                       frame + offset, chunk,
                                       on_write_done, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "write failed cmd=0x%02x offset=%d rc=%d",
                     cmd, offset, rc);
            return ESP_FAIL;
        }
        if (wait_op(5000) != 0) {
            ESP_LOGE(TAG, "write timeout cmd=0x%02x offset=%d", cmd, offset);
            return ESP_FAIL;
        }
        offset += chunk;
    }

    if (!expect_reply) return ESP_OK;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (true) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            ESP_LOGE(TAG, "response timeout cmd=0x%02x", cmd);
            return ESP_ERR_TIMEOUT;
        }
        if (xSemaphoreTake(s_resp_sem, deadline - now) != pdTRUE) {
            ESP_LOGE(TAG, "response timeout cmd=0x%02x", cmd);
            return ESP_ERR_TIMEOUT;
        }
        if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE)
            return ESP_FAIL;
        /* Legacy protocol: any response with status 0 is accepted.
         * The status byte indicates success (0) or failure. */
        if (s_resp_status == 0)
            return ESP_OK;
        ESP_LOGW(TAG, "response status=%d for cmd=0x%02x", s_resp_status, cmd);
        return ESP_FAIL;
    }
}

/* ── CMD_INFO: get device info as JSON ─────────────────────────────── */
static esp_err_t legacy_get_info(char *serial, size_t serial_sz,
                                  char *firmware, size_t fw_sz,
                                  char *file_list, size_t file_list_sz,
                                  char *current_time, size_t current_time_sz)
{
    if (legacy_request(CMD_INFO, 0, NULL, 0, true, 5000) != ESP_OK)
        return ESP_FAIL;

    /* Response is ASCII JSON.  Parse it. */
    if (s_resp_payload_len <= 0) return ESP_FAIL;

    /* Ensure null-terminated */
    char *json_str = heap_caps_malloc(s_resp_payload_len + 1, MALLOC_CAP_SPIRAM);
    if (!json_str) json_str = malloc(s_resp_payload_len + 1);
    if (!json_str) return ESP_ERR_NO_MEM;
    memcpy(json_str, s_resp_payload, s_resp_payload_len);
    json_str[s_resp_payload_len] = '\0';

    cJSON *j = cJSON_Parse(json_str);
    free(json_str);
    if (!j) {
        ESP_LOGE(TAG, "CMD_INFO: JSON parse failed");
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    cJSON *sn = cJSON_GetObjectItem(j, "SN");
    if (sn && cJSON_IsString(sn) && serial) {
        strlcpy(serial, sn->valuestring, serial_sz);
        ret = ESP_OK;
    }
    cJSON *fw = cJSON_GetObjectItem(j, "Model");
    if (fw && cJSON_IsString(fw) && firmware)
        strlcpy(firmware, fw->valuestring, fw_sz);
    cJSON *fl = cJSON_GetObjectItem(j, "FileList");
    if (fl && cJSON_IsString(fl) && file_list)
        strlcpy(file_list, fl->valuestring, file_list_sz);
    cJSON *ct = cJSON_GetObjectItem(j, "CurTIME");
    if (ct && cJSON_IsString(ct) && current_time)
        strlcpy(current_time, ct->valuestring, current_time_sz);

    cJSON_Delete(j);
    return ret;
}

/* ── CMD_CONFIG: set device time ───────────────────────────────────── */
static esp_err_t legacy_set_time(void)
{
    if (!time_is_usable()) {
        ESP_LOGW(TAG, "not setting ring clock: host time is unusable");
        return ESP_ERR_INVALID_STATE;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char time_str[80];
    snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d,%02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    /* Build JSON config: {"SetTIME":"YYYY-MM-DD,HH:MM:SS"} */
    char json[128];
    snprintf(json, sizeof(json), "{\"SetTIME\":\"%s\"}", time_str);

    return legacy_request(CMD_CONFIG, 0, (uint8_t *)json, strlen(json),
                          true, 5000);
}

static time_t legacy_time_value(const char *value)
{
    if (!value) return (time_t)-1;
    int year, month, day, hour, minute, second;
    if (sscanf(value, "%d-%d-%d,%d:%d:%d", &year, &month, &day,
               &hour, &minute, &second) != 6)
        return (time_t)-1;
    struct tm tm = {0};
    tm.tm_year = year - 1900; tm.tm_mon = month - 1; tm.tm_mday = day;
    tm.tm_hour = hour; tm.tm_min = minute; tm.tm_sec = second; tm.tm_isdst = -1;
    time_t result = mktime(&tm);
    struct tm check;
    if (result == (time_t)-1 || !localtime_r(&result, &check) ||
        check.tm_year != year - 1900 || check.tm_mon != month - 1 ||
        check.tm_mday != day || check.tm_hour != hour ||
        check.tm_min != minute || check.tm_sec != second)
        return (time_t)-1;
    return result;
}

static esp_err_t legacy_sync_time_if_needed(const char *ring_time,
                                             const char *expected_serial)
{
    if (!time_is_usable()) return ESP_ERR_INVALID_STATE;
    time_t now = time(NULL);
    time_t device = legacy_time_value(ring_time);
    if (device != (time_t)-1 && llabs((long long)(now - device)) <= 2)
        return ESP_OK;
    if (legacy_set_time() != ESP_OK) return ESP_FAIL;

    char serial[32] = {0}, verify_time[32] = {0};
    if (legacy_get_info(serial, sizeof(serial), NULL, 0, NULL, 0,
                        verify_time, sizeof(verify_time)) != ESP_OK ||
        !expected_serial || strcmp(serial, expected_serial) != 0)
        return ESP_FAIL;
    device = legacy_time_value(verify_time);
    now = time(NULL);
    if (device == (time_t)-1 || llabs((long long)(now - device)) > 5) {
        ESP_LOGW(TAG, "ring clock verification failed: '%s'", verify_time);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ring clock synchronized and verified");
    return ESP_OK;
}

/* ── File download: FILE_OPEN / FILE_READ / FILE_CLOSE ─────────────── */

/* Parse the FileList from CMD_INFO into individual filenames.
 * Returns the number of filenames parsed. */
static int parse_file_list(const char *file_list,
                            char names[][32], int max_count)
{
    if (!file_list || !file_list[0]) return 0;
    int count = 0;
    const char *p = file_list;
    while (count < max_count && *p) {
        const char *comma = strchr(p, ',');
        int len = comma ? (int)(comma - p) : (int)strlen(p);
        if (len > 0 && len < 32) {
            memcpy(names[count], p, len);
            names[count][len] = '\0';
            count++;
        }
        if (!comma) break;
        p = comma + 1;
    }
    return count;
}

static esp_err_t legacy_convert_stored(const char *name)
{
    char source_path[640];
    char recording_id[32];
    strlcpy(recording_id, name, sizeof(recording_id));
    char *dot = strrchr(recording_id, '.');
    if (dot) *dot = '\0';
    snprintf(source_path, sizeof(source_path), SD_OXYMETRY_DIR "/files/%s/%s",
             s_serial, name);
    esp_err_t conversion = oximetry_canonical_convert_vld3(s_serial, recording_id, source_path);
    if (conversion != ESP_OK) {
        ESP_LOGW(TAG, "canonical conversion pending for '%s': %s", name,
                 esp_err_to_name(conversion));
        ox_store_index_mark_converted(s_serial, name, false, esp_err_to_name(conversion));
        return ESP_ERR_INVALID_STATE;
    }
    ox_store_index_mark_converted(s_serial, name, true, NULL);
    upload_sched_request_scan();
    return ESP_OK;
}

static esp_err_t legacy_pull_file(const char *name)
{
    /* FILE_OPEN: payload = filename + null terminator */
    uint8_t open_pl[32];
    int name_len = strlen(name);
    if (name_len > 30) return ESP_FAIL;
    memcpy(open_pl, name, name_len);
    open_pl[name_len] = '\0';
    int open_pl_len = name_len + 1;

    if (legacy_request(CMD_FILE_OPEN, 0, open_pl, open_pl_len, true, 5000) != ESP_OK)
        return ESP_FAIL;

    uint32_t file_size = 0;
    if (s_resp_payload_len >= 4)
        file_size = s_resp_payload[0] | (s_resp_payload[1] << 8) |
                    (s_resp_payload[2] << 16) | (s_resp_payload[3] << 24);
    ESP_LOGI(TAG, "pulling '%s' (%lu bytes)", name, (unsigned long)file_size);

    if (file_size == 0) {
        ESP_LOGW(TAG, "file '%s' has zero size", name);
        legacy_request(CMD_FILE_CLOSE, 0, NULL, 0, true, 2000);
        return ESP_FAIL;
    }

    /* Gen1 FILE_READ is sequential (no random access by offset).
     * If a .part exists, we must re-download from scratch.
     * Files are small (~10-15KB for 8h at 4s/sample), so this is acceptable. */
    long prior = ox_store_part_size(name);
    if (prior > 0) {
        ESP_LOGI(TAG, "discarding %ld bytes of .part (Gen1 has no resume)", prior);
        ox_store_part_remove(name);
    }

    uint32_t offset = 0;
    uint16_t block = 0;
    int empty_count = 0;

    while (offset < file_size) {
        if (legacy_request(CMD_FILE_READ, block, NULL, 0, true, 10000) != ESP_OK) {
            ESP_LOGW(TAG, "file read timeout at block=%u offset=%lu",
                     block, (unsigned long)offset);
            break;
        }

        /* Verify block number matches (documented framing collision issue) */
        if (s_resp_block != block) {
            ESP_LOGW(TAG, "block mismatch: got %u, expected %u — retrying",
                     s_resp_block, block);
            if (++empty_count > 3) break;
            continue;
        }

        int chunk_len = s_resp_payload_len;
        if (chunk_len <= 0) {
            if (++empty_count > 3) break;
            continue;
        }
        empty_count = 0;

        if ((uint64_t)offset + chunk_len > file_size) {
            ESP_LOGW(TAG, "data past file size at offset=%lu",
                     (unsigned long)offset);
            /* Truncate to file_size */
            chunk_len = file_size - offset;
        }

        if (ox_store_part_append(name, s_resp_payload, chunk_len) != ESP_OK) {
            ESP_LOGE(TAG, "SD write failed at offset=%lu", (unsigned long)offset);
            break;
        }
        offset += chunk_len;
        block++;
    }

    /* FILE_CLOSE — always, even on error */
    legacy_request(CMD_FILE_CLOSE, 0, NULL, 0, true, 2000);

    if (offset != file_size) {
        ESP_LOGW(TAG, "incomplete '%s': %lu/%lu bytes; retaining .part",
                 name, (unsigned long)offset, (unsigned long)file_size);
        return ESP_FAIL;
    }

    bool finalised = ox_store_finalize_native(s_serial, name, (long)file_size);
    ESP_LOGI(TAG, "pulled '%s': %lu bytes, finalised=%d",
             name, (unsigned long)offset, finalised);
    if (!finalised) return ESP_FAIL;

    /* Convert to canonical SNT v3 format */
    return legacy_convert_stored(name);
}

/* ── NVS persistence ───────────────────────────────────────────────── */
#define OX_NVS_NS "oximeter"

struct ox_nvs_arg {
    char serial[32];
    char firmware[16];
    char name_prefix[16];
    char ble_name[40];
    char last_addr[18];
};

static esp_err_t do_save_nvs(void *arg)
{
    const struct ox_nvs_arg *a = arg;
    struct ox_nvs_arg local = *a;
    nvs_handle_t h;
    esp_err_t e = nvs_open(OX_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_str(h, "serial", local.serial);
    nvs_set_str(h, "firmware", local.firmware);
    nvs_set_str(h, "name_prefix", local.name_prefix);
    nvs_set_str(h, "ble_name", local.ble_name);
    nvs_set_str(h, "last_addr", local.last_addr);
    nvs_set_u8(h, "driver", (uint8_t)OX_DRIVER_LEGACY);
    e = nvs_commit(h);
    nvs_close(h);
    return e;
}

static esp_err_t do_erase_nvs(void *arg)
{
    (void)arg;
    nvs_handle_t h;
    esp_err_t e = nvs_open(OX_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_erase_key(h, "serial");
    nvs_erase_key(h, "firmware");
    nvs_erase_key(h, "name_prefix");
    nvs_erase_key(h, "ble_name");
    nvs_erase_key(h, "last_addr");
    nvs_erase_key(h, "driver");
    nvs_erase_key(h, "probe_mode");
    e = nvs_commit(h);
    nvs_close(h);
    return e;
}

static esp_err_t do_save_probe_mode(void *arg)
{
    uint8_t mode = (uint8_t)(intptr_t)arg;
    nvs_handle_t h;
    esp_err_t e = nvs_open(OX_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_u8(h, "probe_mode", mode);
    e = nvs_commit(h);
    nvs_close(h);
    return e;
}

static ox_probe_mode_t s_probe_mode = OX_PROBE_PERSISTENT;

static void load_paired_from_nvs(void)
{
    nvs_handle_t h;
    nvs_writer_lock();
    if (nvs_open(OX_NVS_NS, NVS_READONLY, &h) != ESP_OK) { nvs_writer_unlock(); return; }
    size_t len;

    uint8_t drv;
    bool driver_matches = nvs_get_u8(h, "driver", &drv) == ESP_OK && drv == OX_DRIVER_LEGACY;
    len = sizeof(s_serial);
    if (driver_matches && nvs_get_str(h, "serial", s_serial, &len) == ESP_OK && s_serial[0]) {
        s_paired = true;
        len = sizeof(s_firmware);
        nvs_get_str(h, "firmware", s_firmware, &len);
        len = sizeof(s_name_prefix);
        nvs_get_str(h, "name_prefix", s_name_prefix, &len);
        len = sizeof(s_ble_name);
        nvs_get_str(h, "ble_name", s_ble_name, &len);
        len = sizeof(s_paired_addr);
        nvs_get_str(h, "last_addr", s_paired_addr, &len);
    } else {
        s_serial[0] = '\0';
    }
    uint8_t pm;
    if (nvs_get_u8(h, "probe_mode", &pm) == ESP_OK && pm <= 1)
        s_probe_mode = (ox_probe_mode_t)pm;
    nvs_close(h);
    nvs_writer_unlock();

    /* Also try loading from paired.json (SD) as fallback */
    if (!s_paired) {
        char serial[32], fw[16], prefix[16], addr[18], drv[16], bname[40];
        if (ox_store_load_paired(serial, sizeof(serial),
                                 fw, sizeof(fw),
                                 prefix, sizeof(prefix),
                                 addr, sizeof(addr),
                                 drv, sizeof(drv),
                                 bname, sizeof(bname))) {
            if (strcmp(drv, "wellue_legacy") == 0) {
                strlcpy(s_serial, serial, sizeof(s_serial));
                strlcpy(s_firmware, fw, sizeof(s_firmware));
                strlcpy(s_name_prefix, prefix, sizeof(s_name_prefix));
                strlcpy(s_ble_name, bname, sizeof(s_ble_name));
                strlcpy(s_paired_addr, addr, sizeof(s_paired_addr));
                s_paired = true;
            }
        }
    }
}

/* ── Pair task ─────────────────────────────────────────────────────── */
static void pair_task(void *arg)
{
    struct pair_arg *pa = (struct pair_arg *)arg;
    const char *addr_str = pa->addr_str;
    const char *ble_name = pa->ble_name;
    ble_addr_t target;

    xSemaphoreTake(s_ops_mtx, portMAX_DELAY);
    set_state(OX_STATUS_CONNECTING);

    if (!parse_addr(addr_str, &target)) {
        set_error("invalid address: %s", addr_str);
        free(pa);
        xSemaphoreGive(s_ops_mtx);
        vTaskDelete(NULL);
        return;
    }

    if (do_connect_and_discover(&target) != ESP_OK) {
        do_disconnect();
        free(pa);
        xSemaphoreGive(s_ops_mtx);
        vTaskDelete(NULL);
        return;
    }

    /* No AUTH/SETUP needed for Legacy — just CMD_INFO */
    char serial[32] = {0}, firmware[16] = {0}, file_list[512] = {0}, ring_time[32] = {0};
    if (legacy_get_info(serial, sizeof(serial), firmware, sizeof(firmware),
                        file_list, sizeof(file_list), ring_time, sizeof(ring_time)) != ESP_OK) {
        set_error("get_info failed");
        do_disconnect();
        free(pa);
        xSemaphoreGive(s_ops_mtx);
        vTaskDelete(NULL);
        return;
    }

    if (serial[0] == '\0') {
        set_error("empty serial");
        do_disconnect();
        free(pa);
        xSemaphoreGive(s_ops_mtx);
        vTaskDelete(NULL);
        return;
    }

    /* Validate serial format: must be at least 4 chars and printable ASCII */
    size_t slen = strlen(serial);
    bool valid_serial = (slen >= 4);
    for (size_t i = 0; i < slen; i++) {
        if ((unsigned char)serial[i] < 0x20 || (unsigned char)serial[i] > 0x7E) {
            valid_serial = false;
            break;
        }
    }
    if (!valid_serial) {
        set_error("invalid serial format");
        do_disconnect();
        free(pa);
        xSemaphoreGive(s_ops_mtx);
        vTaskDelete(NULL);
        return;
    }

    /* Sanitize firmware string */
    for (int i = 0; firmware[i] != '\0'; i++) {
        if ((unsigned char)firmware[i] < 0x20 || (unsigned char)firmware[i] > 0x7E) {
            firmware[i] = '\0';
            break;
        }
    }

    if (legacy_sync_time_if_needed(ring_time, serial) != ESP_OK)
        ESP_LOGW(TAG, "pairing completed but ring clock could not be verified");

    /* Derive name_prefix from model name in serial (first 6 chars typically) */
    char prefix[16];
    /* Use the model name from the serial if possible (e.g. "O2Ring") */
    strlcpy(prefix, serial, sizeof(prefix));
    /* If serial starts with a known pattern, use a shorter prefix */
    if (strncmp(serial, "O2Ring", 6) == 0)
        strlcpy(prefix, "O2Ring", sizeof(prefix));
    else if (strncmp(serial, "KidsO2", 6) == 0)
        strlcpy(prefix, "KidsO2", sizeof(prefix));
    else
        strlcpy(prefix, "O2Ring", sizeof(prefix));

    /* Build a human-readable display name.  For Gen1 rings the advertised
     * name is typically just "O2Ring" or "CMRing" without a serial suffix,
     * so we append the last 4 digits of the serial for uniqueness. */
    char display_name[40];
    if (ble_name && ble_name[0] && strstr(ble_name, serial) != NULL) {
        /* Advertised name already contains the serial — use it as-is */
        strlcpy(display_name, ble_name, sizeof(display_name));
    } else {
        int slen = (int)strlen(serial);
        char last4[5];
        if (slen >= 4) {
            memcpy(last4, serial + slen - 4, 4);
            last4[4] = '\0';
        } else {
            strlcpy(last4, serial, sizeof(last4));
        }
        snprintf(display_name, sizeof(display_name), "%s %s", prefix, last4);
    }

    /* Save to NVS */
    struct ox_nvs_arg nvs_arg;
    strlcpy(nvs_arg.serial, serial, sizeof(nvs_arg.serial));
    strlcpy(nvs_arg.firmware, firmware, sizeof(nvs_arg.firmware));
    strlcpy(nvs_arg.name_prefix, prefix, sizeof(nvs_arg.name_prefix));
    strlcpy(nvs_arg.ble_name, display_name, sizeof(nvs_arg.ble_name));
    strlcpy(nvs_arg.last_addr, addr_str, sizeof(nvs_arg.last_addr));
    nvs_writer_run(do_save_nvs, &nvs_arg);

    /* Save to paired.json on SD */
    ox_store_save_paired(serial, firmware, prefix, addr_str, "wellue_legacy", display_name);

    /* Update in-RAM state */
    strlcpy(s_serial, serial, sizeof(s_serial));
    strlcpy(s_firmware, firmware, sizeof(s_firmware));
    strlcpy(s_name_prefix, prefix, sizeof(s_name_prefix));
    strlcpy(s_ble_name, display_name, sizeof(s_ble_name));
    strlcpy(s_paired_addr, addr_str, sizeof(s_paired_addr));
    s_paired = true;
    s_presence_served = false;
    s_synced_this_idle = false;
    s_pull_fail_count = 0;
    s_connect_fail_count = 0;

    do_disconnect();
    set_state(OX_STATUS_PAIRED);
    ESP_LOGI(TAG, "paired: serial=%s fw=%s name=%s", serial, firmware, display_name);

    free(pa);
    xSemaphoreGive(s_ops_mtx);
    vTaskDelete(NULL);
}

/* ── Low-duty scan (caller holds s_ops_mtx) ─────────────────────────── */
static esp_err_t do_scan(int timeout_sec)
{
    s_scan_count = 0;

    struct ble_gap_disc_params dp = {
        .itvl = 160,
        .window = 48,
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,  /* Active scan: requests SCAN_RSP for complete UUID/name */
    };

    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(BLE_ADDR_RANDOM, &own_addr_type);
    if (rc != 0) own_addr_type = as11_ble_get_own_addr_type();

    while (xSemaphoreTake(s_scan_done, 0) == pdTRUE) { }

    rc = ble_gap_disc(own_addr_type,
                      timeout_sec * 1000, &dp, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan start failed: %d", rc);
        return ESP_FAIL;
    }

    xSemaphoreTake(s_scan_done, pdMS_TO_TICKS((timeout_sec + 2) * 1000));
    return ESP_OK;
}

/* ── File pull helper ──────────────────────────────────────────────── */
static bool do_pull_and_mark(bool *pulled_any)
{
    if (pulled_any) *pulled_any = false;
    set_state(OX_STATUS_PULLING);

    /* Get device info with file list */
    char serial[32] = {0}, firmware[16] = {0}, file_list[512] = {0}, ring_time[32] = {0};
    if (legacy_get_info(serial, sizeof(serial), firmware, sizeof(firmware),
                        file_list, sizeof(file_list), ring_time, sizeof(ring_time)) != ESP_OK) {
        ESP_LOGW(TAG, "CMD_INFO failed during pull");
        return false;
    }

    /* Verify serial matches paired device */
    if (serial[0] == '\0' || strcmp(serial, s_serial) != 0) {
        ESP_LOGW(TAG, "serial mismatch (got '%s', want '%s')",
                 serial, s_serial);
        return false;
    }
    if (legacy_sync_time_if_needed(ring_time, serial) != ESP_OK)
        ESP_LOGW(TAG, "ring clock sync unavailable — files will retain source time provenance");

    /* Parse file list */
    char names[32][32];
    int count = parse_file_list(file_list, names, 32);
    ESP_LOGI(TAG, "file list: %d files", count);

    bool pull_ok = true;
    for (int i = 0; i < count; i++) {
        if (names[i][0] == '\0') continue;

        /* Strip .vld extension for index check */
        char base_name[32];
        strlcpy(base_name, names[i], sizeof(base_name));
        char *dot = strrchr(base_name, '.');
        if (dot) *dot = '\0';

        int idx = ox_store_index_check(s_serial, base_name);
        if (idx != 1) idx = ox_store_index_check(s_serial, names[i]);
        if (idx == 1) {
            if (ox_store_index_conversion_check(s_serial, names[i]) != 1 &&
                legacy_convert_stored(names[i]) != ESP_OK) {
                ESP_LOGW(TAG, "conversion still pending for '%s'", names[i]);
                pull_ok = false;
            } else {
                ESP_LOGD(TAG, "skip '%s' (already finalised and converted)", names[i]);
            }
            continue;
        }

        ESP_LOGI(TAG, "pulling file %d/%d: '%s'", i + 1, count, names[i]);
        esp_err_t result = legacy_pull_file(names[i]);
        if (result != ESP_OK) {
            pull_ok = false;
            if (result == ESP_ERR_INVALID_STATE) {
                if (pulled_any) *pulled_any = true;
                ESP_LOGW(TAG, "downloaded '%s'; conversion deferred", names[i]);
                continue;
            }
            ESP_LOGW(TAG, "transfer failed for '%s' — ending this sync attempt", names[i]);
            break;
        }
        if (pulled_any) *pulled_any = true;
    }
    return pull_ok;
}

/* ── Background watch task ─────────────────────────────────────────── */
#define OX_END_WINDOW_MS  130000

static void pull_task(void *arg)
{
    (void)arg;

    int wait_ms = 0;
    while (!as11_ble_is_host_ready() && wait_ms < 15000) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait_ms += 500;
    }
    if (!as11_ble_is_host_ready()) {
        ESP_LOGW(TAG, "watch: host not ready, aborting");
        vTaskDelete(NULL);
        return;
    }

    ox_store_ensure_dirs();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(15000));

        if (!s_paired || s_serial[0] == '\0')
            continue;
        if (xSemaphoreTake(s_ops_mtx, 0) != pdTRUE)
            continue;

        if (!sd_storage_is_ready()) {
            xSemaphoreGive(s_ops_mtx);
            continue;
        }

        if (do_scan(4) != ESP_OK) {
            xSemaphoreGive(s_ops_mtx);
            continue;
        }

        if (s_scan_count == 0) {
            if (s_ring_present)
                ESP_LOGI(TAG, "ring gone — next appearance is a new sync window");
            s_ring_present = false;
            if (s_presence_served)
                s_presence_served = false;
            s_synced_this_idle = false;
            s_pull_fail_count = 0;
            s_connect_fail_count = 0;
            xSemaphoreGive(s_ops_mtx);
            continue;
        }

        /* Select the paired device from scan results.  On first boot
         * after pairing, s_paired_addr holds the address from the pair
         * scan.  MAC can rotate on factory reset, so if the stored
         * address isn't found, fall back to the first result (the
         * serial check in do_pull_and_mark is the ultimate gate). */
        int idx = find_paired_in_scan();
        if (idx < 0) {
            if (s_paired_addr[0] != '\0') {
                ESP_LOGW(TAG, "paired addr %s not in scan results; "
                         "using first device (serial will be verified)",
                         s_paired_addr);
            }
            idx = 0;
        }

        if (!s_ring_present) {
            ESP_LOGI(TAG, "ring present: '%s' rssi=%d",
                     s_scan[idx].name, s_scan[idx].rssi);
            s_ring_present = true;
        }

        /* Update paired addr hint from the selected device. */
        addr_to_str(&s_scan[idx].addr, s_paired_addr, sizeof(s_paired_addr));

        if (s_presence_served) {
            if ((xTaskGetTickCount() - s_served_at) < pdMS_TO_TICKS(OX_END_WINDOW_MS)) {
                xSemaphoreGive(s_ops_mtx);
                continue;
            }
            ESP_LOGI(TAG, "watch: still advertising past END window — re-worn, resuming probes");
            s_presence_served = false;
            s_pull_fail_count = 0;
            s_connect_fail_count = 0;
        }

        /* Connect and probe */
        set_state(OX_STATUS_CONNECTING);

        if (do_connect_and_discover(&s_scan[idx].addr) != ESP_OK) {
            ESP_LOGW(TAG, "watch: connect failed: %s", s_error);
            do_disconnect();
            /* After repeated connect failures, apply a cooldown curfew
             * so the ring can power off and reset its BLE state.  Without
             * this, we hammer the ring every ~15s, keeping it awake and
             * stuck in a bad state indefinitely. */
            if (++s_connect_fail_count >= OX_CONNECT_MAX_RETRIES) {
                ESP_LOGW(TAG, "connect failed %d times — curfew %ds (let ring reset)",
                         s_connect_fail_count, OX_END_WINDOW_MS / 1000);
                s_presence_served = true;
                s_served_at = xTaskGetTickCount();
                s_connect_fail_count = 0;
            }
            set_state(OX_STATUS_PAIRED);
            xSemaphoreGive(s_ops_mtx);
            continue;
        }
        s_connect_fail_count = 0;

        /* Gen1 rings only advertise when in Standby Mode (off-finger,
         * post-recording).  If the ring is visible during scan, it is
         * already off-finger and ready for file download.  The curfew
         * mechanism (130s no-reconnect after successful pull) ensures
         * the ring can power off on its own (~2 min auto-off timeout).
         * CMD_READ_SENSORS (0x17) returns live SpO2/HR data, not a
         * reliable worn-state flag — its byte 11 interpretation is
         * inconsistent across reference projects, so we skip the
         * worn-state check entirely. */

        if (s_synced_this_idle) {
            ESP_LOGI(TAG, "watch: ring off-finger / charging (already synced) — standby");
            do_disconnect();
            s_presence_served = true;
            s_served_at = xTaskGetTickCount();
            set_state(OX_STATUS_PAIRED);
            xSemaphoreGive(s_ops_mtx);
            continue;
        }

        /* Give the ring time to flush the recording */
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            do_disconnect();
            set_state(OX_STATUS_PAIRED);
            xSemaphoreGive(s_ops_mtx);
            continue;
        }

        bool pulled_any = false;
        bool pull_ok = do_pull_and_mark(&pulled_any);

        /* If no new files, wait and retry once */
        if (pull_ok && !pulled_any &&
            s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGI(TAG, "watch: no new files — waiting 5s for ring to finalize");
            vTaskDelay(pdMS_TO_TICKS(5000));
            if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE)
                pull_ok = do_pull_and_mark(&pulled_any);
        }

        do_disconnect();

        if (pull_ok) {
            s_presence_served = true;
            s_synced_this_idle = true;
            s_served_at = xTaskGetTickCount();
            s_pull_fail_count = 0;
            ESP_LOGI(TAG, "sync window served — no reconnect; ring powers off on its own");
        } else if (!s_presence_served) {
            /* Pull failed.  Retry quickly while the ring is still advertising
             * (it advertises for ~19s per cycle).  After OX_PULL_MAX_FAST_RETRIES
             * consecutive failures, apply the 130-second curfew so the ring can
             * power off and conserve battery between advertising cycles. */
            if (++s_pull_fail_count < OX_PULL_MAX_FAST_RETRIES) {
                ESP_LOGW(TAG, "sync incomplete — fast retry %d/%d (ring still advertising)",
                         s_pull_fail_count, OX_PULL_MAX_FAST_RETRIES);
            } else {
                s_presence_served = true;
                s_served_at = xTaskGetTickCount();
                ESP_LOGW(TAG, "sync incomplete after %d retries — curfew %ds (let ring rest)",
                         s_pull_fail_count, OX_END_WINDOW_MS / 1000);
            }
        }
        set_state(OX_STATUS_PAIRED);

        xSemaphoreGive(s_ops_mtx);
    }
}

/* ── Public API (driver vtable) ────────────────────────────────────── */
static void legacy_init(void)
{
    if (s_initialized) return;
    s_initialized = true;
    s_state_mtx = xSemaphoreCreateMutex();
    s_ops_mtx   = xSemaphoreCreateMutex();
    s_op_sem    = xSemaphoreCreateBinary();
    s_conn_sem  = xSemaphoreCreateBinary();
    s_resp_sem  = xSemaphoreCreateBinary();
    s_scan_done = xSemaphoreCreateBinary();
    if (!s_state_mtx || !s_ops_mtx || !s_op_sem || !s_conn_sem ||
        !s_resp_sem || !s_scan_done)
        return;

    s_scan = heap_caps_malloc(sizeof(struct ox_scan_result) * OX_SCAN_MAX,
                              MALLOC_CAP_SPIRAM);
    s_resp_buf = heap_caps_malloc(LEGACY_MAX_FRAME, MALLOC_CAP_SPIRAM);
    s_resp_payload = heap_caps_malloc(LEGACY_MAX_FRAME, MALLOC_CAP_SPIRAM);
    if (!s_scan || !s_resp_buf || !s_resp_payload) {
        ESP_LOGE(TAG, "init: failed to allocate PSRAM buffers");
        return;
    }

    load_paired_from_nvs();
    ox_store_ensure_dirs();
    if (sd_storage_is_ready()) oximetry_canonical_ensure_dirs();

    if (s_paired)
        set_state(OX_STATUS_PAIRED);

    TaskHandle_t h = psram_task_create(pull_task, "ox_leg_pull", 8192, NULL, 3,
                                       tskNO_AFFINITY, NULL, NULL);
    if (!h) {
        ESP_LOGW(TAG, "failed to create pull task");
    }
}

static esp_err_t legacy_scan(int timeout_sec)
{
    if (!as11_ble_is_host_ready())
        return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_ops_mtx, 0) != pdTRUE)
        return ESP_ERR_INVALID_STATE;

    s_scan_count = 0;
    set_state(OX_STATUS_SCANNING);

    struct ble_gap_disc_params dp = {
        .itvl = 96,
        .window = 96,
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,
    };

    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(BLE_ADDR_RANDOM, &own_addr_type);
    if (rc != 0) own_addr_type = as11_ble_get_own_addr_type();

    rc = ble_gap_disc(own_addr_type,
                      timeout_sec * 1000, &dp, gap_event, NULL);
    if (rc != 0) {
        set_error("scan start failed: %d", rc);
        xSemaphoreGive(s_ops_mtx);
        return ESP_FAIL;
    }

    xSemaphoreTake(s_scan_done, pdMS_TO_TICKS((timeout_sec + 2) * 1000));

    if (s_paired)
        set_state(OX_STATUS_PAIRED);
    else
        set_state(OX_STATUS_IDLE);

    xSemaphoreGive(s_ops_mtx);
    return ESP_OK;
}

static cJSON *legacy_get_scan_results(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_scan_count; i++) {
        cJSON *e = cJSON_CreateObject();
        char addr_str[18];
        addr_to_str(&s_scan[i].addr, addr_str, sizeof(addr_str));
        cJSON_AddStringToObject(e, "addr", addr_str);
        cJSON_AddStringToObject(e, "name", s_scan[i].name);
        cJSON_AddNumberToObject(e, "rssi", s_scan[i].rssi);
        cJSON_AddStringToObject(e, "type", "legacy");
        cJSON_AddItemToArray(arr, e);
    }
    return arr;
}

static esp_err_t legacy_pair(const char *addr_str)
{
    if (!as11_ble_is_host_ready())
        return ESP_ERR_INVALID_STATE;

    struct pair_arg *pa = calloc(1, sizeof(*pa));
    if (!pa) return ESP_ERR_NO_MEM;
    strlcpy(pa->addr_str, addr_str, sizeof(pa->addr_str));

    /* Look up the BLE advertised name from the last scan results. */
    for (int i = 0; i < s_scan_count; i++) {
        char scan_addr[24];
        addr_to_str(&s_scan[i].addr, scan_addr, sizeof(scan_addr));
        if (strcmp(scan_addr, addr_str) == 0) {
            strlcpy(pa->ble_name, s_scan[i].name, sizeof(pa->ble_name));
            break;
        }
    }

    TaskHandle_t h = psram_task_create(pair_task, "ox_leg_pair", 8192, pa, 5,
                                       tskNO_AFFINITY, NULL, NULL);
    if (!h) {
        free(pa);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void legacy_forget(void)
{
    s_paired = false;
    s_presence_served = false;
    s_synced_this_idle = false;
    s_pull_fail_count = 0;
    s_connect_fail_count = 0;
    s_serial[0] = '\0';
    s_firmware[0] = '\0';
    s_name_prefix[0] = '\0';
    s_ble_name[0] = '\0';
    s_paired_addr[0] = '\0';

    nvs_writer_run(do_erase_nvs, NULL);
    ox_store_delete_paired();

    set_state(OX_STATUS_IDLE);
}

static const char *legacy_get_status(void)
{
    return s_status;
}

static const char *legacy_get_error(void)
{
    return s_error;
}

static bool legacy_is_paired(void)
{
    return s_paired;
}

static cJSON *legacy_get_paired_info(void)
{
    if (!s_paired) return NULL;
    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "serial", s_serial);
    if (s_firmware[0]) cJSON_AddStringToObject(info, "firmware", s_firmware);
    if (s_name_prefix[0]) cJSON_AddStringToObject(info, "name_prefix", s_name_prefix);
    if (s_ble_name[0]) cJSON_AddStringToObject(info, "ble_name", s_ble_name);
    if (s_paired_addr[0]) cJSON_AddStringToObject(info, "addr", s_paired_addr);
    cJSON_AddStringToObject(info, "driver", "wellue_legacy");
    return info;
}

static ox_probe_mode_t legacy_get_probe_mode(void)
{
    return s_probe_mode;
}

static esp_err_t legacy_set_probe_mode(ox_probe_mode_t mode)
{
    if (mode != OX_PROBE_LEGACY && mode != OX_PROBE_PERSISTENT)
        return ESP_ERR_INVALID_ARG;
    if (mode == s_probe_mode)
        return ESP_OK;
    s_probe_mode = mode;
    nvs_writer_run(do_save_probe_mode, (void *)(intptr_t)mode);
    ESP_LOGI(TAG, "probe mode set to %s",
             mode == OX_PROBE_PERSISTENT ? "persistent" : "legacy");
    return ESP_OK;
}

const ox_driver_ops_t legacy_driver_ops = {
    .init             = legacy_init,
    .scan             = legacy_scan,
    .get_scan_results = legacy_get_scan_results,
    .pair             = legacy_pair,
    .forget           = legacy_forget,
    .get_status       = legacy_get_status,
    .get_error        = legacy_get_error,
    .is_paired        = legacy_is_paired,
    .get_paired_info  = legacy_get_paired_info,
    .get_probe_mode   = legacy_get_probe_mode,
    .set_probe_mode   = legacy_set_probe_mode,
};
