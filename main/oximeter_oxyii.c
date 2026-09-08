/*
 * SomnoTrace - O2 Ring (OxyII) BLE protocol codec and session
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
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
 * Clean-room OxyII BLE protocol for Wellue O2 Ring S / SleepHQ O2 Ring Pro.
 * See spec/0003-o2ring-ble-sync.md.
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
#include <mbedtls/aes.h>

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

static const char *TAG = "ox_oxyii";

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
bool ox_store_promote(const char *serial, const char *name);
void ox_store_part_remove(const char *name);

/* ── OxyII protocol constants ──────────────────────────────────────── */
#define OXYII_LEAD         0xA5
#define OXYII_HEADER_LEN   7
#define OXYII_MAX_FRAME    2048

#define OP_GET_CONFIG      0x00
#define OP_LIVE_B          0x04
#define OP_SETUP           0x10
#define OP_SET_UTC_TIME    0xC0
#define OP_GET_INFO        0xE1
#define OP_GET_BATTERY     0xE4
#define OP_GET_FILE_LIST   0xF1
#define OP_READ_FILE_START 0xF2
#define OP_READ_FILE_DATA  0xF3
#define OP_READ_FILE_END   0xF4
#define OP_AUTH            0xFF

#define MFG_OXYII          0xF34E
#define MFG_RECORDING      0x036F

/* MD5("lepucloud") = c2a7cf50dafed885a8f8f7eac44335f3 */
static const uint8_t LEPUCLOUD_MD5[16] = {
    0xc2, 0xa7, 0xcf, 0x50, 0xda, 0xfe, 0xd8, 0x85,
    0xa8, 0xf8, 0xf7, 0xea, 0xc4, 0x43, 0x35, 0xf3,
};

/* OxyII GATT UUIDs (128-bit, stored little-endian for NimBLE) */
static const ble_uuid128_t OXYII_SVC_UUID =
    BLE_UUID128_INIT(0x48, 0x12, 0xd0, 0x41, 0x29, 0x4e, 0x1b, 0x83,
                     0xf9, 0x98, 0x4b, 0xa1, 0x01, 0x00, 0xfb, 0xe8);
static const ble_uuid128_t OXYII_WRITE_UUID =
    BLE_UUID128_INIT(0x48, 0x12, 0xd0, 0x41, 0x29, 0x4e, 0x1b, 0x83,
                     0xf9, 0x98, 0x4b, 0xa1, 0x02, 0x00, 0xfb, 0xe8);
static const ble_uuid128_t OXYII_NOTIFY_UUID =
    BLE_UUID128_INIT(0x48, 0x12, 0xd0, 0x41, 0x29, 0x4e, 0x1b, 0x83,
                     0xf9, 0x98, 0x4b, 0xa1, 0x03, 0x00, 0xfb, 0xe8);

/* ── CRC8 (poly=0x07, init=0) ──────────────────────────────────────── */
static uint8_t oxyii_crc8(const uint8_t *data, int len)
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
/* Encode an OxyII frame into buf.  Returns total frame length. */
static int oxyii_encode(uint8_t *buf, int bufsz, uint8_t op,
                         uint8_t flag, uint8_t seq,
                         const uint8_t *payload, int payload_len)
{
    int total = OXYII_HEADER_LEN + payload_len + 1;
    if (total > bufsz) return -1;

    buf[0] = OXYII_LEAD;
    buf[1] = op;
    buf[2] = ~op;
    buf[3] = flag;
    buf[4] = seq;
    buf[5] = payload_len & 0xFF;
    buf[6] = (payload_len >> 8) & 0xFF;
    if (payload && payload_len > 0)
        memcpy(buf + 7, payload, payload_len);
    buf[total - 1] = oxyii_crc8(buf, total - 1);
    return total;
}

/* Try to decode a frame from buf.  Returns total frame length on success,
 * -1 if incomplete (need more data), -2 if invalid (bad lead/crc). */
static int oxyii_try_decode(const uint8_t *buf, int len,
                             uint8_t *op, uint8_t *flag, uint8_t *seq,
                             uint8_t *payload, int *payload_len,
                             int payload_cap)
{
    if (len < OXYII_HEADER_LEN) return -1;
    if (buf[0] != OXYII_LEAD) return -2;
    if ((uint8_t)(~buf[1]) != buf[2]) return -2;

    int plen = buf[5] | (buf[6] << 8);
    int total = OXYII_HEADER_LEN + plen + 1;
    if (len < total) return -1;

    if (oxyii_crc8(buf, total - 1) != buf[total - 1]) return -2;

    if (op)   *op = buf[1];
    if (flag) *flag = buf[3];
    if (seq)  *seq = buf[4];
    if (payload && payload_cap > 0) {
        int n = plen < payload_cap ? plen : payload_cap;
        memcpy(payload, buf + 7, n);
    }
    if (payload_len) *payload_len = plen;
    return total;
}

/* Forward declaration for auth payload derivation */
static char s_serial[32];

/* AES-128 session encryption state (OxyII Gen2 newer fw) */
static bool s_encrypted = false;
static uint8_t s_session_key[16];
static mbedtls_aes_context s_aes_enc;
static mbedtls_aes_context s_aes_dec;

static void oxyii_crypto_reset(void)
{
    if (s_encrypted) {
        mbedtls_aes_free(&s_aes_enc);
        mbedtls_aes_free(&s_aes_dec);
        s_encrypted = false;
    }
    memset(s_session_key, 0, sizeof(s_session_key));
}

/* Encrypt payload using AES-128-ECB with PKCS7 padding into out.
 * out_cap must be at least ((in_len / 16) + 1) * 16.
 * Returns padded ciphertext length on success, or -1 on error. */
static int oxyii_aes_encrypt(const uint8_t *in, int in_len,
                             uint8_t *out, int out_cap)
{
    if (!s_encrypted) return -1;
    int pad = 16 - (in_len % 16);
    int padded_len = in_len + pad;
    if (padded_len > out_cap) return -1;

    uint8_t block[16];
    for (int off = 0; off < padded_len; off += 16) {
        for (int i = 0; i < 16; i++) {
            int src_idx = off + i;
            if (src_idx < in_len)
                block[i] = in[src_idx];
            else
                block[i] = (uint8_t)pad;
        }
        mbedtls_aes_crypt_ecb(&s_aes_enc, MBEDTLS_AES_ENCRYPT, block, out + off);
    }
    return padded_len;
}

/* Decrypt payload in-place using AES-128-ECB with PKCS7 unpadding.
 * in_out buffer contains ciphertext of length in_len (multiple of 16).
 * Decrypts in-place block-by-block and validates PKCS7 padding.
 * Returns unpadded plaintext length on success, or -1 on error. */
static int oxyii_aes_decrypt_inplace(uint8_t *in_out, int in_len)
{
    if (!s_encrypted || in_len <= 0 || (in_len % 16) != 0)
        return -1;

    for (int off = 0; off < in_len; off += 16) {
        mbedtls_aes_crypt_ecb(&s_aes_dec, MBEDTLS_AES_DECRYPT,
                              in_out + off, in_out + off);
    }

    uint8_t pad = in_out[in_len - 1];
    if (pad < 1 || pad > 16 || pad > in_len)
        return -1;
    for (int i = 0; i < pad; i++) {
        if (in_out[in_len - 1 - i] != pad)
            return -1;
    }
    return in_len - pad;
}

/* ── Auth payload ──────────────────────────────────────────────────── */
/* Derive session key and XOR with LEPUCLOUD_MD5 to produce auth payload. */
static void oxyii_auth_payload(uint8_t *out16)
{
    uint8_t key[16];
    /* key[0..7] = LEPUCLOUD_MD5 even-indexed bytes */
    for (int i = 0; i < 8; i++)
        key[i] = LEPUCLOUD_MD5[i * 2];
    /* key[8..11] = first 4 chars of device SN if known, else "0000" */
    if (s_serial[0] >= '0' && s_serial[0] <= '9' && strlen(s_serial) >= 4) {
        memcpy(key + 8, s_serial, 4);
    } else {
        memcpy(key + 8, "0000", 4);
    }
    /* key[12..15] = little-endian uint32 Unix timestamp (now >> (i * 8)) */
    time_t now = time(NULL);
    for (int i = 0; i < 4; i++)
        key[12 + i] = (now >> (i * 8)) & 0xFF;
    /* auth = key XOR LEPUCLOUD_MD5 */
    for (int i = 0; i < 16; i++)
        out16[i] = key[i] ^ LEPUCLOUD_MD5[i];
}

/* ── SET_UTC_TIME payload (8 bytes) ────────────────────────────────── */
static void oxyii_time_payload(uint8_t *out8)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    out8[0] = (tm.tm_year + 1900) & 0xFF;
    out8[1] = ((tm.tm_year + 1900) >> 8) & 0xFF;
    out8[2] = tm.tm_mon + 1;
    out8[3] = tm.tm_mday;
    out8[4] = tm.tm_hour;
    out8[5] = tm.tm_min;
    out8[6] = tm.tm_sec;
    out8[7] = 0x00;
}

static int64_t oxyii_filename_epoch_ms(const char *name)
{
    if (!name || strlen(name) < 14) return 0;
    for (int i = 0; i < 14; i++)
        if (name[i] < '0' || name[i] > '9') return 0;
    struct tm tm = {0};
    int year, mon, day, hour, min, sec;
    if (sscanf(name, "%4d%2d%2d%2d%2d%2d", &year, &mon, &day,
               &hour, &min, &sec) != 6)
        return 0;
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    return t == (time_t)-1 ? 0 : (int64_t)t * 1000;
}

/* ── READ_FILE_START payload (20 bytes) ────────────────────────────── */
static void oxyii_file_start_payload(uint8_t *out20, const char *name)
{
    memset(out20, 0, 20);
    size_t n = strlen(name);
    if (n > 16) n = 16;
    memcpy(out20, name, n);
    /* bytes 16..19: file type = 0 */
}

/* ── READ_FILE_DATA payload (4 bytes) ──────────────────────────────── */
static void oxyii_file_data_payload(uint8_t *out4, uint32_t offset)
{
    out4[0] = offset & 0xFF;
    out4[1] = (offset >> 8) & 0xFF;
    out4[2] = (offset >> 16) & 0xFF;
    out4[3] = (offset >> 24) & 0xFF;
}

/* ── Module state ──────────────────────────────────────────────────── */
#define OX_SCAN_MAX 16

struct ox_scan_result {
    ble_addr_t addr;
    char name[32];
    int rssi;
    uint16_t mfg;
};

/* Argument passed to pair_task (must be freed by the task). */
struct pair_arg {
    char addr_str[24];
    char ble_name[40];
};

static SemaphoreHandle_t s_state_mtx;
static SemaphoreHandle_t s_ops_mtx;     /* serialise BLE ops (scan/pair/pull) */
static SemaphoreHandle_t s_op_sem;      /* GATT op completion */
static SemaphoreHandle_t s_conn_sem;    /* connect completion */
static SemaphoreHandle_t s_resp_sem;    /* notification response */
static SemaphoreHandle_t s_scan_done;
static volatile int s_op_status;
static volatile int s_conn_status;
static bool s_initialized;

static char s_status[24] = OX_STATUS_IDLE;
static char s_error[128];

/* Paired ring info (loaded from NVS at init) */
static char s_firmware[16];
static char s_name_prefix[16];
static char s_ble_name[40];      /* BLE advertised name (e.g. "SHQO2Pro 0897") */
static char s_paired_addr[18];
static bool s_paired = false;
static bool s_presence_served = false;
static bool s_synced_this_idle = false;
static bool s_ring_present = false;
static TickType_t s_served_at;
static int s_f1_fail_count = 0;  /* consecutive F1 timeouts in this sync window */
static int s_pull_fail_count = 0;   /* consecutive failed pulls in this presence */
static int s_connect_fail_count = 0; /* consecutive connect failures in this presence */
#define OX_PULL_MAX_FAST_RETRIES 3  /* quick retries before applying curfew */
#define OX_CONNECT_MAX_RETRIES 3   /* connect failures before cooldown curfew */
static ox_probe_mode_t s_probe_mode = OX_PROBE_PERSISTENT;

/* Measured: END powers off ~120s after take-off IF no one connects.
 * Any GATT connection resets that timer. Pull happens inside the
 * window, so after a pull: never reconnect while the advert lasts.
 * Still advertising at pull+130s can only mean re-worn. */
#define OX_END_WINDOW_MS  130000
#define OX_WORN_PROBE_MS  60000  /* LIVE_B interval while worn; END lasts ~120s */
#define OX_F1_MAX_RETRIES 3      /* give up on F1 after N consecutive timeouts */

/* Persistent-mode polling: one held connection, unauthenticated LIVE_B.
 * See .ai/OXIMETRY2.md for the experiment that established this value. */
#define OX_PERSISTENT_POLL_MS 30000

/* BLE connection state */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_connect_event_handle = BLE_HS_CONN_HANDLE_NONE; /* saved in CONNECT event before DISCONNECT can overwrite */
static uint16_t s_negotiated_mtu = 23;  /* updated by on_mtu / BLE_GAP_EVENT_MTU */
static uint16_t s_write_handle;
static uint16_t s_notify_handle;
static uint16_t s_cccd_handle;
static uint16_t s_svc_start, s_svc_end;
static uint8_t s_seq = 0;

/* Scan state */
static struct ox_scan_result *s_scan;
static int s_scan_count;

/* Notification accumulation buffer — PSRAM-allocated at init */
static uint8_t *s_resp_buf;
static int s_resp_len;
static uint8_t s_resp_opcode;
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
    ESP_LOGE(TAG, "error: %s", s_error);
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

static bool name_is_oxyii(const char *name)
{
    if (!name || !name[0]) return false;
    char up[32];
    int i;
    for (i = 0; i < 31 && name[i]; i++)
        up[i] = toupper((unsigned char)name[i]);
    up[i] = '\0';
    return strncmp(up, "S8-AW", 5) == 0 ||
           strncmp(up, "SHQO2PRO", 8) == 0;
}

static void addr_to_str(const ble_addr_t *a, char *out, size_t outsz)
{
    static const char hex[] = "0123456789abcdef";
    if (!a || !out || outsz < 18) return;
    for (int i = 5, p = 0; i >= 0; i--) {
        out[p++] = hex[(a->val[i] >> 4) & 0x0F];
        out[p++] = hex[a->val[i] & 0x0F];
        if (i > 0) out[p++] = ':';
        else       out[p]   = '\0';
    }
}

static bool parse_addr(const char *str, ble_addr_t *out)
{
    unsigned int v[6];
    int n = sscanf(str, "%x:%x:%x:%x:%x:%x",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    if (n != 6) return false;
    out->val[5] = v[0]; out->val[4] = v[1]; out->val[3] = v[2];
    out->val[2] = v[3]; out->val[1] = v[4]; out->val[0] = v[5];
    /* Static random addresses have the top two bits of the most-significant
     * byte set (0xC0 mask).  O2 Ring uses a static random address. */
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

/* Handle notification: accumulate and try to decode. */
static void handle_notify_rx(const uint8_t *data, int len)
{
    if (s_resp_len + len > OXYII_MAX_FRAME) {
        ESP_LOGW(TAG, "notify overflow: resp_len=%d + %d > %d",
                 s_resp_len, len, OXYII_MAX_FRAME);
        s_resp_len = 0;
    }
    memcpy(s_resp_buf + s_resp_len, data, len);
    s_resp_len += len;

    uint8_t op, flag, seq;
    int plen;
    int rc = oxyii_try_decode(s_resp_buf, s_resp_len, &op, &flag, &seq,
                               s_resp_payload, &plen, OXYII_MAX_FRAME);
    if (rc > 0) {
        s_resp_opcode = op;
        s_resp_payload_len = plen;
        s_resp_len = 0;

        /* Decrypt payload in-place if session encryption is active and opcode is encrypted */
        if (s_encrypted && op != OP_AUTH && plen > 0 && (plen % 16) == 0) {
            int plain_len = oxyii_aes_decrypt_inplace(s_resp_payload, plen);
            if (plain_len >= 0) {
                s_resp_payload_len = plain_len;
            } else {
                ESP_LOGW(TAG, "response decrypt failed op=0x%02x (len=%d)", op, plen);
            }
        }

        xSemaphoreGive(s_resp_sem);
    } else if (rc == -2) {
        ESP_LOGW(TAG, "notify decode error, resetting buffer");
        s_resp_len = 0;
    }
    /* rc == -1: incomplete, wait for more data */
}

/* Decode HCI disconnect reason to a human-readable string for logging.
 * NimBLE wraps HCI error codes with BLE_HS_EHCI (0x200), so we mask. */
static const char *hci_err_str(int reason)
{
    switch (reason & 0xFF) {
    case 0x08: return "Connection Timeout";
    case 0x0B: return "Conn Already Exists";
    case 0x13: return "Remote User Terminated";
    case 0x16: return "Terminated by Local Host";
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

        /* Parse name from adv data (with manual fallback) */
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

        /* Recording-mode advert (on-finger): never a pull candidate. */
        if (cid == MFG_RECORDING)
            return 0;

        /* Match Gen2 rings by:
         * 1. Manufacturer ID 0xF34E (OxyII sync/idle advertisement)
         * 2. Known paired MAC address match (non-recording)
         * 3. Gen2 device name pattern (non-recording) */
        bool match = (cid == MFG_OXYII) ||
                     (name_is_oxyii(name) && cid != MFG_RECORDING) ||
                     (s_paired && s_paired_addr[0] != '\0' && strcmp(addr_str, s_paired_addr) == 0 && cid != MFG_RECORDING);
        if (!match) return 0;
        if (name[0] == '\0')
            strlcpy(name, "O2Ring", sizeof(name));

        /* Dedupe by address */
        for (int i = 0; i < s_scan_count; i++) {
            if (memcmp(&s_scan[i].addr, &event->disc.addr,
                       sizeof(ble_addr_t)) == 0) {
                s_scan[i].rssi = event->disc.rssi;
                s_scan[i].mfg = cid;
                if (name[0] && strncmp(name, "O2Ring", 6) != 0)
                    strlcpy(s_scan[i].name, name, sizeof(s_scan[i].name));
                return 0;
            }
        }
        if (s_scan_count < OX_SCAN_MAX) {
            s_scan[s_scan_count].addr = event->disc.addr;
            strlcpy(s_scan[s_scan_count].name, name,
                    sizeof(s_scan[s_scan_count].name));
            s_scan[s_scan_count].rssi = event->disc.rssi;
            s_scan[s_scan_count].mfg = cid;
            s_scan_count++;
            ESP_LOGD(TAG, "scan: '%s' rssi=%d addr=%s mfg=0x%04x",
                     name, event->disc.rssi, addr_str, cid);
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
        oxyii_crypto_reset();
        /* Unblock any waiting request */
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
        s_negotiated_mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU: %d", event->mtu.value);
        return 0;
    default:
        return 0;
    }
}

/* ── GATT discovery callbacks ──────────────────────────────────────── */
static int on_mtu(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t mtu, void *arg)
{
    (void)conn; (void)arg;
    s_op_status = err ? err->status : 0;
    if (!err || err->status == 0)
        s_negotiated_mtu = mtu;
    xSemaphoreGive(s_op_sem);
    return 0;
}

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
        if (ble_uuid_cmp(&chr->uuid.u, &OXYII_WRITE_UUID.u) == 0)
            s_write_handle = chr->val_handle;
        else if (ble_uuid_cmp(&chr->uuid.u, &OXYII_NOTIFY_UUID.u) == 0)
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
/* do_mtu=false skips MTU exchange (persistent-mode contact polling uses
 * only tiny LIVE_B frames that fit in the default 23-byte ATT MTU). */
static esp_err_t do_connect_and_discover(ble_addr_t *target, bool do_mtu)
{
    s_write_handle = s_notify_handle = s_cccd_handle = 0;
    s_svc_start = s_svc_end = 0;
    s_resp_len = 0;

    /* Drain any stale s_conn_sem token left by a previous remote
     * disconnect.  Without this, xSemaphoreTake below returns
     * immediately on the stale token and we proceed with a dead
     * handle (BLE_HS_CONN_HANDLE_NONE), causing every subsequent
     * GATT operation to fail. */
    while (xSemaphoreTake(s_conn_sem, 0) == pdTRUE) { }
    s_conn_status = -1;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_connect_event_handle = BLE_HS_CONN_HANDLE_NONE;

    /* Connect — infer own address type based on peer address type
     * (random peer requires random own address). */
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

    /* MTU exchange (skipped in persistent contact-polling mode) */
    if (do_mtu) {
        s_negotiated_mtu = 23;  /* reset before exchange */
        clear_op_sem();
        ble_gattc_exchange_mtu(s_conn_handle, on_mtu, NULL);
        int mtu_rc = wait_op(2000);
        if (mtu_rc != 0) {
            /* MTU exchange timed out or failed — the ring didn't respond
             * to the ATT Exchange MTU Request.  This is distinct from
             * recording mode (where the exchange succeeds but returns 23). */
            ESP_LOGW(TAG, "MTU exchange failed (rc=%d)", mtu_rc);
            set_error("MTU exchange failed: %d", mtu_rc);
            return ESP_FAIL;
        }
        /* MTU=23 (default) after a successful exchange suggests the ring
         * is in recording mode and its GATT layout is stripped — the OxyII
         * service won't be discoverable.  It can also mean this is a Gen1
         * O2 Ring (which does not support the OxyII protocol and never
         * negotiates a larger MTU); Gen1 rings must be paired with the
         * legacy driver.  Abort early with a clear message instead of
         * failing later with "service range empty". */
        if (s_negotiated_mtu <= 23) {
            ESP_LOGW(TAG, "MTU=%d after exchange — ring may be in recording mode",
                     s_negotiated_mtu);
            set_error("MTU=%d — ring may be in recording mode; remove from finger and retry. "
                      "If this is a Gen1 O2 Ring, pair as Gen1 (legacy)",
                      s_negotiated_mtu);
            return ESP_FAIL;
        }
    }

    /* Discover OxyII service by UUID */
    clear_op_sem();
    rc = ble_gattc_disc_svc_by_uuid(s_conn_handle, &OXYII_SVC_UUID.u,
                                     on_disc_svc, NULL);
    if (rc != 0 || wait_op(10000) != 0) {
        set_error("OxyII service not found"); return ESP_FAIL;
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
    oxyii_crypto_reset();
}

/* ── Protocol request/response ─────────────────────────────────────── */
static esp_err_t oxyii_request(uint8_t op, const uint8_t *payload, int plen,
                                bool expect_reply, int timeout_ms)
{
    uint8_t enc_buf[OXYII_MAX_FRAME];
    const uint8_t *tx_payload = payload;
    int tx_plen = plen;

    if (s_encrypted && op != OP_AUTH) {
        int enc_len = oxyii_aes_encrypt(payload ? payload : (const uint8_t *)"",
                                        plen, enc_buf, sizeof(enc_buf));
        if (enc_len < 0) {
            ESP_LOGE(TAG, "payload encrypt failed op=0x%02x", op);
            return ESP_FAIL;
        }
        tx_payload = enc_buf;
        tx_plen = enc_len;
    }

    uint8_t frame[OXYII_MAX_FRAME];
    int flen = oxyii_encode(frame, sizeof(frame), op, 0, s_seq++,
                             tx_payload, tx_plen);
    if (flen < 0) return ESP_FAIL;

    if (expect_reply) {
        while (xSemaphoreTake(s_resp_sem, 0) == pdTRUE) { }
        s_resp_len = 0;
    }

    /* Use write-without-response (OxyII protocol uses WRITE_CMD, not WRITE_REQ).
     * This eliminates one BLE round-trip per request, roughly doubling throughput. */
    int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_write_handle,
                                          frame, flen);
    if (rc != 0) {
        ESP_LOGE(TAG, "write failed op=0x%02x rc=%d", op, rc);
        return ESP_FAIL;
    }

    if (!expect_reply) return ESP_OK;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (true) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            if (op == OP_AUTH)
                ESP_LOGD(TAG, "response timeout op=0x%02x (auth probe)", op);
            else
                ESP_LOGE(TAG, "response timeout op=0x%02x", op);
            return ESP_ERR_TIMEOUT;
        }
        if (xSemaphoreTake(s_resp_sem, deadline - now) != pdTRUE) {
            if (op == OP_AUTH)
                ESP_LOGD(TAG, "response timeout op=0x%02x (auth probe)", op);
            else
                ESP_LOGE(TAG, "response timeout op=0x%02x", op);
            return ESP_ERR_TIMEOUT;
        }
        if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE)
            return ESP_FAIL;
        if (s_resp_opcode == op)
            return ESP_OK;
        ESP_LOGW(TAG, "ignoring leftover notify op=0x%02x (want 0x%02x, %d B)",
                 s_resp_opcode, op, s_resp_payload_len);
    }
}

/* AUTH + SETUP only. Enough for GET_INFO / LIVE_B. Do not send F4
 * here — that closes a recording handle. Measured (SHQO2Pro 2D010003):
 * LIVE_B replies even with no AUTH; GET_INFO needs this prefix. */
static esp_err_t oxyii_session_open(void)
{
    s_seq = 0;
    oxyii_crypto_reset();

    uint8_t auth[16];
    oxyii_auth_payload(auth);

    /* Step 1: send OP_AUTH.
     * On newer firmware (1.13.1.0 / 2D010001), the ring replies with a 20-byte
     * payload containing the AES-128 session key XOR-encrypted with LEPUCLOUD_MD5.
     * On older firmware (2D010002 / 2D010003), the ring never replies to OP_AUTH.
     * The vendor SDK waits with a 1000 ms timer; if no reply, we fall back to plaintext. */
    esp_err_t rc = oxyii_request(OP_AUTH, auth, 16, true, 1000);
    if (rc == ESP_OK && s_resp_payload_len >= 20) {
        uint8_t dec[20];
        for (int i = 0; i < 20; i++)
            dec[i] = s_resp_payload[i] ^ LEPUCLOUD_MD5[i % 16];
        /* dec[0] = type (0x01 AES), dec[1] = key_len (16), dec[4..19] = AES key */
        if (dec[0] == 0x01 && dec[1] == 16) {
            memcpy(s_session_key, dec + 4, 16);
            mbedtls_aes_init(&s_aes_enc);
            mbedtls_aes_init(&s_aes_dec);
            mbedtls_aes_setkey_enc(&s_aes_enc, s_session_key, 128);
            mbedtls_aes_setkey_dec(&s_aes_dec, s_session_key, 128);
            s_encrypted = true;
            ESP_LOGI(TAG, "auth: session key negotiated (AES-128-ECB enabled)");
        } else {
            ESP_LOGW(TAG, "auth: unexpected key format (type=0x%02x, len=%d, payload_len=%d)",
                     dec[0], dec[1], s_resp_payload_len);
        }
    } else if (rc == ESP_OK) {
        ESP_LOGW(TAG, "auth: OP_AUTH response too short (%d bytes, want >= 20)",
                 s_resp_payload_len);
    }

    if (!s_encrypted) {
        ESP_LOGI(TAG, "auth: no OP_AUTH reply — proceeding in plaintext mode (Gen2 legacy)");
        vTaskDelay(pdMS_TO_TICKS(100));
        uint8_t setup = 0x00;
        if (oxyii_request(OP_SETUP, &setup, 1, true, 5000) != ESP_OK)
            return ESP_FAIL;
    }
    return ESP_OK;
}

/* File-session prep. Only after LIVE_B says off-finger.
 * Documented order: SET_UTC → GET_CONFIG → F4. F4 clears a leftover
 * recording handle; without it, F1 is silently dropped (the F1 wedge). */
static esp_err_t oxyii_prepare_files(void)
{
    if (!time_is_usable()) {
        ESP_LOGW(TAG, "not setting ring clock: host time is unusable");
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t tp[8];
    oxyii_time_payload(tp);
    if (oxyii_request(OP_SET_UTC_TIME, tp, 8, true, 5000) != ESP_OK)
        return ESP_FAIL;

    if (oxyii_request(OP_GET_CONFIG, NULL, 0, true, 5000) != ESP_OK)
        return ESP_FAIL;

    /* F4 acks. Must consume it or the next F1 sees this frame. */
    if (oxyii_request(OP_READ_FILE_END, NULL, 0, true, 2000) != ESP_OK)
        ESP_LOGW(TAG, "F4 ack missing — continuing");
    return ESP_OK;
}

/* ── GET_INFO: extract firmware + serial ───────────────────────────── */
static esp_err_t oxyii_get_info(char *serial, size_t serial_sz,
                                 char *firmware, size_t fw_sz)
{
    if (oxyii_request(OP_GET_INFO, NULL, 0, true, 5000) != ESP_OK)
        return ESP_FAIL;

    /* Payload layout (from protocol study):
     *   bytes 9..16: firmware (8 chars, null-padded)
     *   byte 37: serial length
     *   bytes 38..: serial string */
    if (s_resp_payload_len < 38) {
        ESP_LOGW(TAG, "get_info: response payload too short (%d bytes, want >= 38, enc=%d)",
                 s_resp_payload_len, s_encrypted);
        return ESP_FAIL;
    }

    if (firmware && fw_sz > 0) {
        int fl = s_resp_payload_len - 9 < 8 ? s_resp_payload_len - 9 : 8;
        if (fl < 0) fl = 0;
        memcpy(firmware, s_resp_payload + 9, fl);
        firmware[fl] = '\0';
        /* Trim trailing nulls */
        for (int i = fl - 1; i >= 0 && firmware[i] == 0; i--)
            firmware[i] = '\0';
    }

    if (serial && serial_sz > 0) {
        int slen = s_resp_payload[37];
        if (slen > 18) slen = 18;
        if (slen > 0 && 38 + slen <= s_resp_payload_len) {
            int n = slen < (int)serial_sz - 1 ? slen : (int)serial_sz - 1;
            memcpy(serial, s_resp_payload + 38, n);
            serial[n] = '\0';
            while (n > 0 && ((unsigned char)serial[n - 1] <= ' ' || serial[n - 1] == 0))
                serial[--n] = '\0';
        } else {
            serial[0] = '\0';
        }
    }
    ESP_LOGI(TAG, "device info: serial='%s' fw='%s' (encrypted=%d)",
             serial ? serial : "", firmware ? firmware : "", s_encrypted);
    return ESP_OK;
}

/* LIVE_B contact. Returns:
 *   1  off-finger / END window (state 0x00) — pull is allowed
 *   0  on-finger or file handle open — do not pull
 *  -1  no usable reply — do not pull (fail closed)
 *
 * README: [5] 0x00 = no finger, 0x01 = finger present, 0x03 = file open. */
static int oxyii_off_finger(void)
{
    if (oxyii_request(OP_LIVE_B, NULL, 0, true, 2000) != ESP_OK)
        return -1;
    if (s_resp_payload_len < 9) return -1;
    uint8_t state = s_resp_payload[5];
    uint8_t spo2  = s_resp_payload[6];
    uint8_t hr    = s_resp_payload[8];
    ESP_LOGI(TAG, "live_b: state=0x%02x spo2=%u hr=%u", state, spo2, hr);

    /* 0x03 = leftover file handle from the ring's own recording
     * (README "F1 wedge": F1 silently hangs until F4). Closing it is
     * safe and lets END report its true contact state. */
    if (state == 0x03) {
        ESP_LOGI(TAG, "live_b: file handle open — clearing wedge with F4");
        oxyii_request(OP_READ_FILE_END, NULL, 0, true, 2000);
        if (oxyii_request(OP_LIVE_B, NULL, 0, true, 2000) != ESP_OK)
            return -1;
        if (s_resp_payload_len < 9) return -1;
        state = s_resp_payload[5];
        spo2  = s_resp_payload[6];
        hr    = s_resp_payload[8];
        ESP_LOGI(TAG, "live_b after F4: state=0x%02x spo2=%u hr=%u",
                 state, spo2, hr);
    }

    if (state == 0x00) return 1;
    return 0;
}

/* ── GET_FILE_LIST: return count + names ───────────────────────────── */
static int oxyii_get_file_list(char names[][17], int max_count)
{
    int rc = oxyii_request(OP_GET_FILE_LIST, NULL, 0, true, 5000);
    if (rc != ESP_OK) {
        /* Spec 4.7: F1 timeout → F4, retry once on this link, then fail. */
        ESP_LOGW(TAG, "F1 timed out (rc=%d) — F4 then retry once", rc);
        oxyii_request(OP_READ_FILE_END, NULL, 0, true, 2000);
        if (oxyii_request(OP_GET_FILE_LIST, NULL, 0, true, 5000) != ESP_OK)
            return -1;
    }

    if (s_resp_payload_len < 1) {
        ESP_LOGW(TAG, "file list: empty response payload (len=%d, enc=%d)",
                 s_resp_payload_len, s_encrypted);
        return -1;
    }
    int count = s_resp_payload[0];
    if (count > max_count) count = max_count;
    ESP_LOGI(TAG, "file list: %d files on ring (enc=%d)", count, s_encrypted);

    int pos = 1;
    for (int i = 0; i < count; i++) {
        if (pos + 16 > s_resp_payload_len) break;
        memcpy(names[i], s_resp_payload + pos, 16);
        names[i][16] = '\0';
        /* Trim trailing nulls */
        for (int j = 15; j >= 0 && names[i][j] == 0; j--)
            names[i][j] = '\0';
        pos += 16;
    }
    return count;
}

/* ── Pull a single file ────────────────────────────────────────────── */
static esp_err_t oxyii_convert_stored(const char *name)
{
    char source_path[640];
    snprintf(source_path, sizeof(source_path), SD_OXYMETRY_DIR "/files/%s/%s.bin",
             s_serial, name);
    esp_err_t conversion = oximetry_canonical_convert_format_a(
        s_serial, name, source_path, oxyii_filename_epoch_ms(name));
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

static esp_err_t oxyii_pull_file(const char *name)
{
    /* READ_FILE_START */
    uint8_t start_pl[20];
    oxyii_file_start_payload(start_pl, name);
    if (oxyii_request(OP_READ_FILE_START, start_pl, 20, true, 5000) != ESP_OK)
        return ESP_FAIL;

    uint32_t file_size = 0;
    if (s_resp_payload_len >= 4)
        file_size = s_resp_payload[0] | (s_resp_payload[1] << 8) |
                    (s_resp_payload[2] << 16) | (s_resp_payload[3] << 24);
    ESP_LOGI(TAG, "pulling '%s' (%lu bytes)", name, (unsigned long)file_size);

    /* Resume from the durable partial length.  The device's reported size is
     * not completion evidence; the decoder validates the trailer later. */
    long prior = ox_store_part_size(name);
    if (prior < 0 || (file_size > 0 && (uint32_t)prior > file_size)) {
        ox_store_part_remove(name);
        prior = 0;
    }
    uint32_t offset = (uint32_t)prior;
    int empty_count = 0;
    bool transfer_complete = (file_size > 0 && offset == file_size);
    while (!transfer_complete) {
        uint8_t off_pl[4];
        oxyii_file_data_payload(off_pl, offset);
        if (oxyii_request(OP_READ_FILE_DATA, off_pl, 4, true, 10000) != ESP_OK) {
            ESP_LOGW(TAG, "file data timeout at offset=%lu", (unsigned long)offset);
            break;
        }

        /* Payload is the file data chunk */
        int chunk_len = s_resp_payload_len;
        if (chunk_len <= 0) {
            if (++empty_count > 2) break;
            continue;
        }
        empty_count = 0;

        if (file_size > 0 && (uint64_t)offset + chunk_len > file_size) {
            ESP_LOGW(TAG, "device returned data past file size at offset=%lu",
                     (unsigned long)offset);
            break;
        }
        if (ox_store_part_append(name, s_resp_payload, chunk_len) != ESP_OK) {
            ESP_LOGE(TAG, "SD write failed at offset=%lu", (unsigned long)offset);
            break;
        }
        offset += chunk_len;

        if (file_size > 0 && offset == file_size) {
            transfer_complete = true;
        }
    }

    /* READ_FILE_END */
    if (oxyii_request(OP_READ_FILE_END, NULL, 0, true, 2000) != ESP_OK)
        ESP_LOGW(TAG, "F4 ack missing after '%s'", name);

    /* Completion requires both an exact transfer and the O2 Ring S trailer. */
    if (!transfer_complete || file_size == 0) {
        ESP_LOGW(TAG, "incomplete '%s': %lu/%lu bytes; retaining .part",
                 name, (unsigned long)offset, (unsigned long)file_size);
        return ESP_FAIL;
    }
    bool finalised = ox_store_promote(s_serial, name);
    ESP_LOGI(TAG, "pulled '%s': %lu bytes, finalised=%d",
             name, (unsigned long)offset, finalised);
    if (!finalised) return ESP_FAIL;

    return oxyii_convert_stored(name);
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
    nvs_set_u8(h, "driver", (uint8_t)OX_DRIVER_OXYII);
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

static void load_paired_from_nvs(void)
{
    nvs_handle_t h;
    nvs_writer_lock();
    if (nvs_open(OX_NVS_NS, NVS_READONLY, &h) != ESP_OK) { nvs_writer_unlock(); return; }
    size_t len;

    uint8_t drv;
    bool driver_matches = nvs_get_u8(h, "driver", &drv) == ESP_OK && drv == OX_DRIVER_OXYII;
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

        /* Validate loaded serial format: if corrupted from pre-AES firmware, clear */
        size_t slen = strlen(s_serial);
        bool valid = (slen >= 4);
        for (size_t i = 0; i < slen; i++) {
            if ((unsigned char)s_serial[i] < 0x20 || (unsigned char)s_serial[i] > 0x7E) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            ESP_LOGW(TAG, "invalid or corrupted serial in NVS ('%s') — clearing paired state", s_serial);
            s_paired = false;
            s_serial[0] = '\0';
        } else {
            ESP_LOGI(TAG, "paired ring loaded: serial='%s' name='%s' addr='%s'",
                     s_serial, s_ble_name, s_paired_addr);
        }
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
            if (strcmp(drv, "wellue_oxyii") == 0) {
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

    if (do_connect_and_discover(&target, true) != ESP_OK) {
        do_disconnect();
        free(pa);
        xSemaphoreGive(s_ops_mtx);
        vTaskDelete(NULL);
        return;
    }

    if (oxyii_session_open() != ESP_OK) {
        set_error("session open failed");
        do_disconnect();
        free(pa);
        xSemaphoreGive(s_ops_mtx);
        vTaskDelete(NULL);
        return;
    }

    char serial[32] = {0}, firmware[16] = {0};
    if (oxyii_get_info(serial, sizeof(serial), firmware, sizeof(firmware)) != ESP_OK) {
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

    /* Derive name_prefix from serial (first 4 chars) */
    char prefix[5];
    memcpy(prefix, serial, 4);
    prefix[4] = '\0';

    /* Build a human-readable display name from the BLE advertised name
     * (e.g. "SHQO2Pro 0897").  If the advertised name is unavailable,
     * fall back to "O2Ring" + last 4 digits of serial. */
    char display_name[40];
    if (ble_name && ble_name[0] && strncmp(ble_name, "O2Ring", 6) != 0) {
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
        snprintf(display_name, sizeof(display_name), "O2Ring %s", last4);
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
    ox_store_save_paired(serial, firmware, prefix, addr_str, "wellue_oxyii", display_name);

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

/* ── Low-duty OxyII scan (caller holds s_ops_mtx) ──────────────────── */
static esp_err_t do_scan(int timeout_sec)
{
    s_scan_count = 0;

    struct ble_gap_disc_params dp = {
        .itvl = 160,   /* 100 ms */
        .window = 48,  /* 30 ms  — low duty; pairing scan uses 96/96 */
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,  /* Active scan: requests SCAN_RSP for complete name */
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

static void canonical_migration_task(void *arg)
{
    (void)arg;
    /* Do not block app_main while AS11 and its notification worker start. */
    vTaskDelay(pdMS_TO_TICKS(30000));
    while (sd_storage_recording_active())
        vTaskDelay(pdMS_TO_TICKS(5000));
    if (sd_storage_is_ready()) {
        oximetry_canonical_reconcile();
        oximetry_canonical_migrate_all_legacy();
    }
    vTaskDelete(NULL);
}

/* ── File pull helper (shared by legacy and persistent paths) ─────── */
/* Caller must be connected with session opened and files prepared.
 * Sets OX_STATUS_PULLING, runs F1 → file list → per-file pull.
 * On F1 exhaustion, marks served so the watch stops reconnecting.
 * Returns true if all files pulled (or none to pull), false on failure.
 * *pulled_any is set to true if at least one file was actually downloaded
 * and finalised (as opposed to all files being already in the index).
 * s_presence_served may be set inside on F1 exhaustion. */
static bool do_pull_and_mark(bool *pulled_any)
{
    if (pulled_any) *pulled_any = false;
    set_state(OX_STATUS_PULLING);

    char names[32][17];
    int count = oxyii_get_file_list(names, 32);
    if (count < 0) {
        s_f1_fail_count++;
        ESP_LOGW(TAG, "file list failed (op=0x%02x len=%d), attempt %d/%d",
                 s_resp_opcode, s_resp_payload_len,
                 s_f1_fail_count, OX_F1_MAX_RETRIES);
        if (s_f1_fail_count >= OX_F1_MAX_RETRIES) {
            /* F1 never responds on this ring (firmware quirk).
             * Mark served so we stop reconnecting — each connection
             * resets the ring's power-off timer.  Next sync window
             * (next take-off) will try again fresh. */
            s_presence_served = true;
            s_served_at = xTaskGetTickCount();
            ESP_LOGW(TAG, "F1 unreachable after %d attempts — treating as served, ring can sleep",
                     OX_F1_MAX_RETRIES);
        }
        return false;
    }
    s_f1_fail_count = 0;
    ESP_LOGI(TAG, "file list: %d files", count);

    bool pull_ok = true;
    for (int i = 0; i < count; i++) {
        if (names[i][0] == '\0') continue;

        int idx = ox_store_index_check(s_serial, names[i]);
        if (idx == 1) {
            if (ox_store_index_conversion_check(s_serial, names[i]) != 1 &&
                oxyii_convert_stored(names[i]) != ESP_OK) {
                ESP_LOGW(TAG, "conversion still pending for '%s'", names[i]);
                pull_ok = false;
            } else {
                ESP_LOGD(TAG, "skip '%s' (already finalised and converted)", names[i]);
            }
            continue;
        }

        ESP_LOGI(TAG, "pulling file %d/%d: '%s'", i + 1, count, names[i]);
        esp_err_t result = oxyii_pull_file(names[i]);
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

/* ── Background watch: scan-only, connect only in the END window ──── */
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
            s_f1_fail_count = 0;
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

        /* Remember last seen addr (hint; MAC can rotate on factory reset). */
        addr_to_str(&s_scan[idx].addr, s_paired_addr, sizeof(s_paired_addr));

        if (s_presence_served) {
            /* Never reconnect during END — a connection resets the
             * ring's power-off timer (measured). Passive scans only.
             * Still advertising past the longest possible END window
             * (take-off + ~120s; pull ran inside it) ⇒ back on a
             * finger ⇒ clear served so the next take-off pulls. */
            if ((xTaskGetTickCount() - s_served_at) < pdMS_TO_TICKS(OX_END_WINDOW_MS)) {
                xSemaphoreGive(s_ops_mtx);
                continue;
            }
            ESP_LOGI(TAG, "watch: still advertising past END window — re-worn, resuming probes");
            s_presence_served = false;
            s_f1_fail_count = 0;
            s_pull_fail_count = 0;
            s_connect_fail_count = 0;
        }

        if (s_probe_mode == OX_PROBE_PERSISTENT) {
            /* ── Persistent mode ───────────────────────────────────────
             * Hold one GATT connection, poll unauthenticated LIVE_B
             * every OX_PERSISTENT_POLL_MS. AUTH/SETUP/GET_INFO and
             * file transfer are deferred until off-finger is detected.
             * See .ai/OXIMETRY2.md for the experiment that validated
             * this approach. */
            set_state(OX_STATUS_CONNECTING);

            if (do_connect_and_discover(&s_scan[idx].addr, false) != ESP_OK) {
                ESP_LOGW(TAG, "watch: connect failed: %s", s_error);
                do_disconnect();
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

            /* Poll loop: LIVE_B every 30 s while on-finger. */
            bool pulled = false;
            while (s_probe_mode == OX_PROBE_PERSISTENT && !pulled) {
                int off = oxyii_off_finger();
                if (off == 1) {
                    if (s_synced_this_idle) {
                        ESP_LOGI(TAG, "watch: ring off-finger / charging (already synced) — standby");
                        do_disconnect();
                        s_presence_served = true;
                        s_served_at = xTaskGetTickCount();
                        pulled = true;
                        break;
                    }

                    /* Off-finger: upgrade to full session for pull. */
                    ESP_LOGI(TAG, "watch: off-finger detected — upgrading to full session");
                    /* MTU exchange now for file-transfer throughput. */
                    clear_op_sem();
                    ble_gattc_exchange_mtu(s_conn_handle, on_mtu, NULL);
                    wait_op(2000);

                    if (oxyii_session_open() != ESP_OK) {
                        ESP_LOGW(TAG, "watch: session open failed after off-finger");
                        break;
                    }

                    char serial[32] = {0}, firmware[16] = {0};
                    if (oxyii_get_info(serial, sizeof(serial),
                                       firmware, sizeof(firmware)) != ESP_OK
                        || serial[0] == '\0'
                        || strcmp(serial, s_serial) != 0) {
                        ESP_LOGW(TAG, "watch: serial mismatch (got '%s', want '%s')",
                                 serial, s_serial);
                        break;
                    }

                    if (oxyii_prepare_files() != ESP_OK) {
                        ESP_LOGW(TAG, "watch: file prep failed");
                        break;
                    }

                    /* Give the ring time to flush the just-finished
                     * recording to its internal flash before we query
                     * the file list.  Without this delay the new file
                     * may not appear yet, causing a missed pull. */
                    ESP_LOGI(TAG, "watch: off-finger — waiting 3s for ring to finalize recording");
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE)
                        break;

                    bool pulled_any = false;
                    bool pull_ok = do_pull_and_mark(&pulled_any);

                    /* If the first check found no new files, the ring
                     * may still be finalizing.  Wait once more and retry
                     * before giving up on this sync window. */
                    if (pull_ok && !pulled_any &&
                        s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                        ESP_LOGI(TAG, "watch: no new files yet — waiting 5s for ring to finalize");
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
                        /* Pull failed.  Retry quickly while the ring is still
                         * advertising.  After OX_PULL_MAX_FAST_RETRIES, apply
                         * the curfew so the ring can power off and conserve
                         * battery between advertising cycles. */
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
                    pulled = true;
                } else if (off == 0) {
                    /* On-finger: hold connection, wait before next poll. */
                    s_synced_this_idle = false;
                    set_state(OX_STATUS_MONITORING);
                    vTaskDelay(pdMS_TO_TICKS(OX_PERSISTENT_POLL_MS));
                    /* Connection may have dropped during the delay. */
                    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
                        ESP_LOGI(TAG, "watch: persistent connection dropped — will reconnect");
                        break;
                    }
                } else {
                    /* LIVE_B error (-1): connection may be stale. */
                    ESP_LOGW(TAG, "watch: LIVE_B error — dropping connection");
                    break;
                }
            }

            /* If we broke out without pulling, clean up the connection. */
            if (!pulled) {
                do_disconnect();
            }
            /* Always return to PAIRED after the persistent cycle, whether
             * we pulled successfully or broke out early.  Without this,
             * the state stays PULLING forever after a successful pull,
             * and the web UI shows "Syncing…" until reboot. */
            set_state(OX_STATUS_PAIRED);
            xSemaphoreGive(s_ops_mtx);
            continue;
        }

        /* ── Legacy mode (reconnect + full handshake each cycle) ─────── */
        set_state(OX_STATUS_CONNECTING);

        if (do_connect_and_discover(&s_scan[idx].addr, true) != ESP_OK) {
            ESP_LOGW(TAG, "watch: connect failed: %s", s_error);
            do_disconnect();
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

        /* Probe only: AUTH+SETUP. Never F4 while we may still be recording. */
        if (oxyii_session_open() != ESP_OK) {
            ESP_LOGW(TAG, "watch: session open failed");
            do_disconnect();
            set_state(OX_STATUS_PAIRED);
            xSemaphoreGive(s_ops_mtx);
            continue;
        }

        char serial[32] = {0}, firmware[16] = {0};
        if (oxyii_get_info(serial, sizeof(serial), firmware, sizeof(firmware)) != ESP_OK
            || serial[0] == '\0' || strcmp(serial, s_serial) != 0) {
            ESP_LOGW(TAG, "watch: serial mismatch (got '%s', want '%s')",
                     serial, s_serial);
            do_disconnect();
            set_state(OX_STATUS_PAIRED);
            xSemaphoreGive(s_ops_mtx);
            continue;
        }

        /* Pull only in the documented no-contact END window
         * (LIVE_B [5] == 0x00). On-finger / unknown / F1-wedge: drop
         * the link immediately so the ring can keep recording or
         * finish countdown and sleep. Do not mark served. */
        int off = oxyii_off_finger();
        if (off != 1) {
            s_synced_this_idle = false;
            ESP_LOGI(TAG, "watch: not off-finger (live_b=%d) — disconnect, retry in %ds",
                     off, OX_WORN_PROBE_MS / 1000);
            do_disconnect();
            set_state(OX_STATUS_PAIRED);
            xSemaphoreGive(s_ops_mtx);
            vTaskDelay(pdMS_TO_TICKS(OX_WORN_PROBE_MS));
            continue;
        }

        if (s_synced_this_idle) {
            ESP_LOGI(TAG, "watch: ring off-finger / charging (already synced) — standby");
            do_disconnect();
            s_presence_served = true;
            s_served_at = xTaskGetTickCount();
            set_state(OX_STATUS_PAIRED);
            xSemaphoreGive(s_ops_mtx);
            continue;
        }

        if (oxyii_prepare_files() != ESP_OK) {
            ESP_LOGW(TAG, "watch: file prep failed");
            do_disconnect();
            set_state(OX_STATUS_PAIRED);
            xSemaphoreGive(s_ops_mtx);
            continue;
        }

        bool pull_ok = do_pull_and_mark(NULL);
        do_disconnect();
        if (pull_ok) {
            s_presence_served = true;
            s_synced_this_idle = true;
            s_served_at = xTaskGetTickCount();
            s_pull_fail_count = 0;
            ESP_LOGI(TAG, "sync window served — no reconnect; ring powers off on its own");
        } else if (!s_presence_served) {
            /* Pull failed.  Retry quickly while the ring is still
             * advertising.  After OX_PULL_MAX_FAST_RETRIES, apply
             * the curfew so the ring can power off and conserve
             * battery between advertising cycles. */
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

/* ── Public API (driver vtable) ───────────────────────────────────── */
static void oxyii_init(void)
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
    s_resp_buf = heap_caps_malloc(OXYII_MAX_FRAME, MALLOC_CAP_SPIRAM);
    s_resp_payload = heap_caps_malloc(OXYII_MAX_FRAME, MALLOC_CAP_SPIRAM);
    if (!s_scan || !s_resp_buf || !s_resp_payload) {
        ESP_LOGE(TAG, "init: failed to allocate PSRAM buffers");
        return;
    }

    load_paired_from_nvs();
    ox_store_ensure_dirs();
    if (sd_storage_is_ready()) oximetry_canonical_ensure_dirs();

    if (s_paired)
        set_state(OX_STATUS_PAIRED);

    TaskHandle_t h = psram_task_create(pull_task, "ox_pull", 8192, NULL, 3,
                                       tskNO_AFFINITY, NULL, NULL);
    if (!h) {
        ESP_LOGW(TAG, "failed to create pull task");
    }
    TaskHandle_t migration = psram_task_create(canonical_migration_task,
                                               "ox_migrate", 12288, NULL, 1,
                                               0, NULL, NULL);
    if (!migration) ESP_LOGW(TAG, "failed to create canonical migration task");
}

static esp_err_t oxyii_scan(int timeout_sec)
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

static cJSON *oxyii_get_scan_results(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_scan_count; i++) {
        cJSON *e = cJSON_CreateObject();
        char addr_str[18];
        addr_to_str(&s_scan[i].addr, addr_str, sizeof(addr_str));
        cJSON_AddStringToObject(e, "addr", addr_str);
        cJSON_AddStringToObject(e, "name", s_scan[i].name);
        cJSON_AddNumberToObject(e, "rssi", s_scan[i].rssi);
        cJSON_AddStringToObject(e, "type", "oxyii");
        cJSON_AddItemToArray(arr, e);
    }
    return arr;
}

static esp_err_t oxyii_pair(const char *addr_str)
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

    TaskHandle_t h = psram_task_create(pair_task, "ox_pair", 8192, pa, 5,
                                       tskNO_AFFINITY, NULL, NULL);
    if (!h) {
        free(pa);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void oxyii_forget(void)
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

static const char *oxyii_get_status(void)
{
    return s_status;
}

static const char *oxyii_get_error(void)
{
    return s_error;
}

static bool oxyii_is_paired(void)
{
    return s_paired;
}

static cJSON *oxyii_get_paired_info(void)
{
    if (!s_paired) return NULL;
    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "serial", s_serial);
    if (s_firmware[0]) cJSON_AddStringToObject(info, "firmware", s_firmware);
    if (s_name_prefix[0]) cJSON_AddStringToObject(info, "name_prefix", s_name_prefix);
    if (s_ble_name[0]) cJSON_AddStringToObject(info, "ble_name", s_ble_name);
    if (s_paired_addr[0]) cJSON_AddStringToObject(info, "addr", s_paired_addr);
    cJSON_AddStringToObject(info, "driver", "wellue_oxyii");
    return info;
}

static ox_probe_mode_t oxyii_get_probe_mode(void)
{
    return s_probe_mode;
}

static esp_err_t oxyii_set_probe_mode(ox_probe_mode_t mode)
{
    if (mode != OX_PROBE_LEGACY && mode != OX_PROBE_PERSISTENT)
        return ESP_ERR_INVALID_ARG;
    if (mode == s_probe_mode)
        return ESP_OK;
    s_probe_mode = mode;
    /* Persist to NVS (fire-and-forget; nvs_writer_run handles flash safety). */
    nvs_writer_run(do_save_probe_mode, (void *)(intptr_t)mode);
    ESP_LOGI(TAG, "probe mode set to %s",
             mode == OX_PROBE_PERSISTENT ? "persistent" : "legacy");
    return ESP_OK;
}

const ox_driver_ops_t oxyii_driver_ops = {
    .init             = oxyii_init,
    .scan             = oxyii_scan,
    .get_scan_results = oxyii_get_scan_results,
    .pair             = oxyii_pair,
    .forget           = oxyii_forget,
    .get_status       = oxyii_get_status,
    .get_error        = oxyii_get_error,
    .is_paired        = oxyii_is_paired,
    .get_paired_info  = oxyii_get_paired_info,
    .get_probe_mode   = oxyii_get_probe_mode,
    .set_probe_mode   = oxyii_set_probe_mode,
};
