/*
 * SomnoTrace - BLE transport for ResMed AirSense 11 pairing
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
 * Clean-room implementation of the AirSense 11 BLE pairing handshake. The
 * protocol (GATT layout, FIG framing, custom SRP-6a key exchange) was studied
 * from project documentation; no third-party source code was copied.
 *
 * ── NimBLE configuration note ──────────────────────────────────────────
 * CONFIG_BT_NIMBLE_ROLE_PERIPHERAL and CONFIG_BT_NIMBLE_GATT_SERVER must
 * both be set to y in sdkconfig.defaults, even though this device acts
 * only as a BLE central (client).  The reason: NimBLE's ATT dispatch table
 * only includes the incoming-notification handler (ble_att_svr_rx_notify,
 * which generates BLE_GAP_EVENT_NOTIFY_RX) when MYNEWT_VAL(BLE_GATTS) is
 * non-zero.  With GATT_SERVER disabled, notifications from the AS11 are
 * received at the HCI layer but silently dropped — no GAP event is ever
 * generated.  Enabling the peripheral role does not cause the device to
 * advertise; it simply compiles in the server-side ATT handlers we need.
 * ────────────────────────────────────────────────────────────────────────
 */

#include "as11_ble.h"
#include "sb_ble.h"
#include "as11_reconnect.h"
#include "as11_adv.h"
#include "session_writer.h"
#include "post_therapy.h"
#include "sd_storage.h"
#include "bsp_display.h"
#include "mqtt_manager.h"
#include "time_sync.h"
#include "therapy_alert.h"
#include "net_provision.h"
#include "oximeter.h"
#include "upload_sched.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_rom_crc.h"
#include "esp_heap_caps.h"
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
#include "esp_coexist.h"
#endif
#include "nvs_flash.h"
#include "nvs.h"
#include "psram_task.h"
#include "nvs_writer.h"
#include "log_stream.h"

#include "mbedtls/sha256.h"
#include "mbedtls/bignum.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"

#include <time.h>

static const char *TAG = "as11_ble";

/* ------------------------------------------------------------------ */
/*  GATT / FIG constants                                              */
/* ------------------------------------------------------------------ */
/* Service fd56, TX a6220002, RX a6220003 (128-bit), CCCD 0x2902. */
static const ble_uuid16_t UUID_SERVICE =
    BLE_UUID16_INIT(0xfd56);
/* SomnoBridge advertises its private service (somnopal §10.1). */
static const ble_uuid128_t UUID_SB_SVC =
    BLE_UUID128_INIT(0x50, 0x6f, 0x6e, 0x76, 0x06, 0x73, 0x1d, 0x9f,
                     0xb4, 0x4b, 0x4b, 0x6e, 0x00, 0xd1, 0xa7, 0x8b);

static const ble_uuid128_t UUID_TX =
    BLE_UUID128_INIT(0xaa, 0x44, 0x20, 0x9d, 0x08, 0xcb, 0xae, 0xaf,
                     0x20, 0x4b, 0xf1, 0x35, 0x02, 0x00, 0x22, 0xa6);
static const ble_uuid128_t UUID_RX =
    BLE_UUID128_INIT(0xaa, 0x44, 0x20, 0x9d, 0x08, 0xcb, 0xae, 0xaf,
                     0x20, 0x4b, 0xf1, 0x35, 0x03, 0x00, 0x22, 0xa6);

#define DEVICE_NAME_PREFIX  "ResMed"

#define FIG_SYNC            0xCAFEBABEu
#define FIG_HEADER_LEN      12               /* bytes 0x04..0x0f */
#define FIG_VCID_TX         0x0393           /* plaintext, key-exchange only */
#define FIG_VCID_TX_ENC     0x0397           /* encrypted TX (RPC requests) */
#define FIG_VCID_RX_ENC     0x0396           /* encrypted RX (notifications/responses) */

#define RX_BUF_MAX          16384
#define TX_PAYLOAD_MAX      1024

/* ------------------------------------------------------------------ */
/*  NVS                                                               */
/* ------------------------------------------------------------------ */
#define NVS_NS              "as11"
#define NVS_K_ADDR          "ble_addr"
#define NVS_K_NAME          "ble_name"
#define NVS_K_CLIENTID      "client_id"
#define NVS_K_PAIRKEY       "pair_key"      /* masterPairKey, 64 hex */

/* ------------------------------------------------------------------ */
/*  Scan cache                                                        */
/* ------------------------------------------------------------------ */
#define SCAN_MAX  12
struct scan_entry {
    ble_addr_t addr;
    char name[32];
    int8_t rssi;
    bool bridge;          /* SomnoBridge advertisement (Phase 2/3) */
};
static struct scan_entry s_scan[SCAN_MAX];
static int s_scan_count;
static SemaphoreHandle_t s_scan_done;

/* ------------------------------------------------------------------ */
/*  Connection / RPC state                                            */
/* ------------------------------------------------------------------ */
static volatile bool s_host_ready;
static uint8_t s_own_addr_type;

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle;          /* value handle of TX char */
static uint16_t s_rx_handle;          /* value handle of RX char */
static uint16_t s_svc_start, s_svc_end;
static uint16_t s_cccd_handle;
static uint16_t s_mtu = 23;

/* AS11 device clock captured before stream starts (epoch ms).
 * Used to compute clock_drift_ms at session stop without sending
 * RPCs during active streaming (which congests BLE buffers). */
static int64_t s_as11_clock_ms = 0;
static int64_t s_as11_clock_capture_ntp_ms = 0;

/* Handles for characteristics discovered during full GATT scan.
 * The AS11 btmon trace shows BlueZ reads these by handle (ATT Read Request
 * opcode 0x0a), not by UUID (Read By Type opcode 0x08).  We must match. */
static uint16_t s_devname_handle;    /* 0x2a00 Device Name */
static uint16_t s_appearance_handle; /* 0x2a01 Appearance */
static uint16_t s_steehl_handles[3]; /* 3 Steehl vendor chars */

static SemaphoreHandle_t s_op_sem;    /* generic GATT op completion */
static volatile int s_op_status;
static SemaphoreHandle_t s_connect_sem;
static volatile int s_connect_status;
static SemaphoreHandle_t s_resp_sem;  /* JSON RPC response arrived */
static cJSON *s_resp_json;            /* owned; freed by waiter */
static SemaphoreHandle_t s_cmd_mtx;   /* serialization mutex for RPC requests */

static uint8_t *s_rx_buf;          /* PSRAM-allocated, RX_BUF_MAX bytes */
static int s_rx_len;

/* Notification processing queue — offloads heavy work from NimBLE host task.
 * The host task just copies raw notification bytes and enqueues them;
 * a dedicated task does FIG parsing, AES decrypt, cJSON, and dispatch. */
#define NOTIF_QUEUE_LEN  64
typedef struct {
    uint8_t *data;
    int      len;
} notif_item_t;
static QueueHandle_t s_notif_queue = NULL;
/* Backlog metrics — see BLE_GAP_EVENT_NOTIFY_RX. */
static UBaseType_t s_notif_hwm = 0;
static uint32_t    s_notif_dropped = 0;
static uint32_t    s_notif_dropped_bytes = 0;
static uint32_t    s_notif_alloc_fail = 0;
/* Connection-scoped reception forensics: these discriminate "the AS11
 * stopped transmitting" (silence before the supervision timeout) from "our
 * radio starved" (steady traffic with a sudden stop).  Bounded counters and
 * one timestamp, reset on every new connection. */
static int64_t  s_notif_last_rx_us = 0;
static uint32_t s_notif_rx_total = 0;
static uint32_t s_notif_gap_events = 0;
static int64_t  s_notif_last_gap_us = 0;
#define NOTIF_GAP_THRESHOLD_US (500LL * 1000)
static TaskHandle_t  s_notif_task  = NULL;

/* ── Spool fragment collector ────────────────────────────────────────
 * When a spool pull is in progress, s_spool_collector is non-NULL and
 * handle_notify() routes SpoolFragment notifications to it instead of
 * the normal notification dispatch.  All arming callers hold s_cmd_mtx
 * for the whole request+collect cycle, so a second pull or a concurrent
 * passthrough RPC cannot overwrite the pointer mid-pull.  The pointer is
 * written (before PullSpoolFragments is sent) and read (by
 * notif_proc_task) as a single aligned word, which is sufficient on
 * Xtensa given the command mutex serializes arming. */
#define SPOOL_MAX_FRAGS   32
#define SPOOL_FRAG_MAX    2808          /* matches AS11 max fragment size */

typedef struct {
    int      seq;
    uint8_t *data;
    int      len;
} spool_frag_t;

typedef struct {
    spool_frag_t frags[SPOOL_MAX_FRAGS];
    int      frag_count;
    char     status[48];                /* SPOOL_INCOMPLETE / SPOOL_COMPLETE / ... */
    char     next_addr_json[256];       /* nextSpoolAddress for multi-round pulls */
    char     spool_hash_hex[65];        /* spoolHash on terminal fragment */
    bool     done;                      /* set when status != SPOOL_INCOMPLETE */
    bool     error;                     /* ERROR_* status seen */
    bool     timed_out;                 /* fragment wait hit the deadline */
    SemaphoreHandle_t sem;              /* given when done */
} spool_collector_t;

static spool_collector_t *s_spool_collector = NULL;

/* Encrypted session state (set after reconnect or pairing). */
static uint8_t s_session_key[32];       /* AES-256 key */
static bool s_session_encrypted = false;
static volatile uint32_t s_session_epoch = 0;

/* When true, the next BLE_GAP_EVENT_DISCONNECT will NOT trigger
 * auto-reconnect.  Set before intentional ble_gap_terminate() calls
 * (user disconnect, forget pairing, pairing-flow disconnect before
 * reconnect_task).  Cleared on successful connect and in the disconnect
 * handler after the auto-reconnect decision is made. */
static bool s_manual_disconnect = false;

/* In-RAM cache of paired device credentials.  Loaded once at init from NVS,
 * updated on pair/forget.  Eliminates NVS reads from the HTTP /api/status
 * hot path (as11_ble_is_paired / as11_ble_get_paired_info). */
static struct {
    char addr[24];
    char name[32];
    char client_id[64];
    char pair_key[65];
    bool valid;
} s_pair_cache;

/* ── Incident latch ───────────────────────────────────────────────────
 * Set on a supervision-timeout disconnect (HCI 0x08) or on an unfillable
 * long gap (marked by session_writer).  Consumed at the next post-therapy
 * pass that finds therapy fully stopped, where the deferred diagnostic
 * spool pull runs (see .ai/RECONNECT/PLAN.md Phase C).  RAM-only by
 * design: a reboot clears it so a stale flag can never trigger BLE work
 * long after the incident it refers to. */
static bool     s_incident_pending = false;
static char     s_incident_kind[16];
static int64_t  s_incident_time_ms;
static int      s_incident_attempts;

/* Set by the disconnect handler when reason==BLE_ERR_CONN_SPVN_TMO so the
 * reconnect task (which runs in its own context, off the GAP callback)
 * logs a bounded local-state context line. */
static bool s_supv_tmo_context_pending;

/* Once-per-connection flag so an experimental supervision-timeout update
 * is attempted at most once per link (see Phase D in the plan). */
static bool s_supv_update_sent;

/* Experimental flags, persisted in NVS (loaded lazily). */
#define EXP_NVS_NS         "as11exp"
#define EXP_KEY_SUPV_MS    "supv_ms"
#define EXP_KEY_COEX_FIX   "coexfix"
static int  s_exp_supv_timeout_ms = -1;   /* -1 = not yet loaded */
static int  s_exp_coex_fix = -1;

/* Decode a BLE disconnect/notify reason into a short name.  The reason
 * carries the HCI status code in its low byte (e.g. 520 = 0x208 = NimBLE
 * base | 0x08 supervision timeout).  Same pattern as oximeter_oxyii.c —
 * kept file-local like the existing copies; a shared helper belongs in a
 * common header if a third user appears. */
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

/* ── Incident latch (public API, see header) ──────────────────────────
 * Plain globals, no lock: writers are the GAP callback and
 * session_writer, readers run in post-therapy — they never race
 * destructively, and worst case a mark lands while a diagnostic pull is
 * already in flight (it is simply picked up at the next stop). */
void as11_ble_incident_mark(const char *kind)
{
    if (!s_incident_pending) {
        s_incident_pending = true;
        s_incident_time_ms = (int64_t)time(NULL) * 1000;
        s_incident_attempts = 0;
        strlcpy(s_incident_kind, kind ? kind : "unknown",
                sizeof(s_incident_kind));
        ESP_LOGI(TAG, "incident: marked (%s)", s_incident_kind);
    }
}

bool as11_ble_incident_pending(void)      { return s_incident_pending; }
int64_t as11_ble_incident_time_ms(void)   { return s_incident_time_ms; }
int as11_ble_incident_attempts(void)      { return s_incident_attempts; }

void as11_ble_incident_note_attempt(void) { s_incident_attempts++; }
void as11_ble_incident_clear(void)
{
    s_incident_pending = false;
    s_incident_kind[0] = '\0';
    s_incident_attempts = 0;
}

/* ── Experimental flags (Phase D) ─────────────────────────────────────
 * Read lazily from NVS, cached for the process lifetime.  Writes persist
 * via nvs_writer (the async NVS queue used everywhere else) so the HTTP
 * path never blocks on flash. */
static void exp_flags_load(void)
{
    if (s_exp_supv_timeout_ms >= 0 && s_exp_coex_fix >= 0) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(EXP_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint16_t v;
        uint8_t b;
        if (s_exp_supv_timeout_ms < 0) {
            s_exp_supv_timeout_ms = (nvs_get_u16(h, EXP_KEY_SUPV_MS, &v) == ESP_OK)
                                    ? (int)v : 0;
        }
        if (s_exp_coex_fix < 0) {
            s_exp_coex_fix = (nvs_get_u8(h, EXP_KEY_COEX_FIX, &b) == ESP_OK)
                             ? (int)b : 0;
        }
        nvs_close(h);
    } else {
        if (s_exp_supv_timeout_ms < 0) s_exp_supv_timeout_ms = 0;
        if (s_exp_coex_fix < 0)        s_exp_coex_fix = 0;
    }
}

uint16_t as11_ble_exp_supv_timeout_ms(void)
{
    exp_flags_load();
    return (uint16_t)s_exp_supv_timeout_ms;
}

bool as11_ble_exp_coex_fix(void)
{
    exp_flags_load();
    return s_exp_coex_fix != 0;
}

/* NVS write helper for the experimental flags.  Runs inside the
 * nvs_writer proxy (see nvs_writer_run) so it is safe to call from any
 * task including httpd workers with PSRAM stacks. */
typedef struct {
    const char *key;
    uint32_t    value;
    bool        is_u8;
} exp_nvs_write_t;

static esp_err_t do_exp_nvs_write(void *arg)
{
    const exp_nvs_write_t *w = arg;
    nvs_handle_t h;
    esp_err_t err = nvs_open(EXP_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = w->is_u8 ? nvs_set_u8(h, w->key, (uint8_t)w->value)
                   : nvs_set_u16(h, w->key, (uint16_t)w->value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t as11_ble_exp_supv_timeout_set(uint16_t ms)
{
    /* Validate against the BLE spec range before persisting: supervision
     * timeout is 100 ms units, 0x000A..0x0C80 => 1000..32000 ms.
     * 0 disables the experiment. */
    if (ms != 0 && (ms < 1000 || ms > 32000)) {
        return ESP_ERR_INVALID_ARG;
    }
    exp_nvs_write_t w = { .key = EXP_KEY_SUPV_MS, .value = ms, .is_u8 = false };
    esp_err_t err = nvs_writer_run(do_exp_nvs_write, &w);
    if (err == ESP_OK) {
        s_exp_supv_timeout_ms = (int)ms;
        ESP_LOGW(TAG, "experimental: supervision timeout target = %u ms "
                 "(applies to future connections)", ms);
    }
    return err;
}

esp_err_t as11_ble_exp_coex_fix_set(bool on)
{
    exp_nvs_write_t w = { .key = EXP_KEY_COEX_FIX, .value = on ? 1 : 0,
                          .is_u8 = true };
    esp_err_t err = nvs_writer_run(do_exp_nvs_write, &w);
    if (err == ESP_OK) {
        s_exp_coex_fix = on ? 1 : 0;
        ESP_LOGW(TAG, "experimental: coex status-bit fix %s "
                 "(applies to future connections)", on ? "ON" : "OFF");
    }
    return err;
}

static char s_target_name[32];
static ble_addr_t s_target_addr;
#define SERVER_PK_MAX  600
#define SALT_MAX       160
static char *s_server_pk;       /* PSRAM; serverPk from StartKeyExchange */
static char *s_salt;            /* PSRAM; salt from StartKeyExchange */
static bool s_kex_ready;        /* serverPk + salt captured */
static char s_passkey[16];

/* State machine + last error (guarded by s_state_mtx). */
static SemaphoreHandle_t s_state_mtx;
static const char *s_state = AS11_STATUS_IDLE;
static char s_error[96];

/* ------------------------------------------------------------------ */
/*  SRP-6a context (persists between start_pair and confirm_pair)     */
/* ------------------------------------------------------------------ */
#define SRP_PAD 256
/* Device-specific 2048-bit modulus N (verified against reference client). */
static const char SRP_N_HEX[] =
    "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050"
    "A37329CBB4A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50"
    "E8083969EDB767B0CF6095179A163AB3661A05FBD5FAAAE82918A9962F0B93B8"
    "55F97993EC975EEAA80D740ADBF4FF747359D041D5C33EA71D281E446B14773B"
    "CA97B43A23FB801676BD207A436C6481F1D2B9078717461A5B9D32E688F87748"
    "544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB3786160279004E57AE6"
    "AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DBFBB6"
    "94B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73";

struct srp_ctx {
    bool active;
    mbedtls_mpi N, g, a, A;
    uint8_t A_pad[SRP_PAD];   /* A serialized, 256 bytes BE */
    char client_id[64];
    char master_key_hex[65];  /* K hex (64 chars) */
};
static struct srp_ctx s_srp;

/* ------------------------------------------------------------------ */
/*  Small helpers                                                     */
/* ------------------------------------------------------------------ */
static void set_state(const char *st)
{
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    s_state = st;
    if (strcmp(st, AS11_STATUS_PAIRED) == 0) {
        s_error[0] = '\0';
    }
    xSemaphoreGive(s_state_mtx);
    bsp_display_set_as11_paired(strcmp(st, AS11_STATUS_PAIRED) == 0);
    mqtt_manager_publish_state(true);
    log_stream_request_ble_push();
}

static void set_error(const char *msg)
{
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    strlcpy(s_error, msg ? msg : "", sizeof(s_error));
    s_state = AS11_STATUS_ERROR;
    xSemaphoreGive(s_state_mtx);
    bsp_display_set_as11_paired(false);
    mqtt_manager_publish_state(true);
    ESP_LOGE(TAG, "error: %s", msg ? msg : "");
    log_stream_request_ble_push();
}

static void bytes_to_hex(const uint8_t *in, size_t n, char *out_upper)
{
    static const char H[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        out_upper[i * 2]     = H[in[i] >> 4];
        out_upper[i * 2 + 1] = H[in[i] & 0x0F];
    }
    out_upper[n * 2] = '\0';
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t out_max)
{
    size_t n = strlen(hex);
    if (n % 2) return -1;
    n /= 2;
    if (n > out_max) return -1;
    for (size_t i = 0; i < n; i++) {
        char hi = hex[i * 2], lo = hex[i * 2 + 1];
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = k ? lo : hi;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return -1;
            v = (v << 4) | d;
        }
        out[i] = (uint8_t)v;
    }
    return (int)n;
}

/* SHA-256 over a list of (ptr,len) segments. */
static void sha256_segs(const uint8_t *const *segs, const size_t *lens,
                        int n, uint8_t out[32])
{
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    for (int i = 0; i < n; i++) {
        mbedtls_sha256_update(&c, segs[i], lens[i]);
    }
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
}

/* HMAC-SHA256(key, data) -> out[32]. */
static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *data, size_t data_len,
                        uint8_t out[32])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(info, key, key_len, data, data_len, out);
}

/* AES-256-CBC encrypt with random IV.
 * Wire format: [IV (16 bytes)][ciphertext([u16 len][payload][zero pad to 16])]
 * Returns total output length (16 + padded_len) or -1 on error. */
static int aes_cbc_encrypt(const uint8_t *key, const uint8_t *plaintext,
                           int plen, uint8_t *out, int out_max)
{
    /* Frame: 2-byte LE length prefix + payload + zero padding to 16-byte boundary */
    int framed_len = 2 + plen;
    int pad_len = (16 - framed_len % 16) % 16;
    int padded_len = framed_len + pad_len;
    ESP_LOGD(TAG, "aes_cbc_encrypt: plen=%d framed=%d pad=%d padded=%d out_max=%d",
             plen, framed_len, pad_len, padded_len, out_max);
    if (16 + padded_len > out_max) {
        ESP_LOGE(TAG, "aes_cbc_encrypt: output too large: %d > %d", 16 + padded_len, out_max);
        return -1;
    }

    uint8_t *framed = out + 16;  /* ciphertext starts after IV */
    framed[0] = plen & 0xFF;
    framed[1] = (plen >> 8) & 0xFF;
    memcpy(framed + 2, plaintext, plen);
    memset(framed + 2 + plen, 0, pad_len);

    /* Random IV — must be in a separate buffer because mbedtls_aes_crypt_cbc
     * overwrites the IV in-place with the last ciphertext block. */
    uint8_t iv[16];
    esp_fill_random(iv, 16);
    /* Copy IV to output before encryption (it won't be modified there) */
    memcpy(out, iv, 16);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int rc_setkey = mbedtls_aes_setkey_enc(&aes, key, 256);
    if (rc_setkey != 0) {
        ESP_LOGE(TAG, "aes_cbc_encrypt: setkey_enc failed rc=%d", rc_setkey);
        mbedtls_aes_free(&aes);
        return -1;
    }
    int rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded_len,
                                   iv, framed, framed);
    mbedtls_aes_free(&aes);
    if (rc != 0) {
        ESP_LOGE(TAG, "aes_cbc_encrypt: crypt_cbc failed rc=%d", rc);
        return -1;
    }
    return 16 + padded_len;
}

/* AES-256-CBC decrypt.
 * Input: [IV (16 bytes)][ciphertext]
 * Output: plaintext (without the 2-byte length prefix), written to out.
 * Returns plaintext length or -1 on error. */
static int aes_cbc_decrypt(const uint8_t *key, const uint8_t *data,
                           int data_len, uint8_t *out, int out_max)
{
    if (data_len < 32 || (data_len - 16) % 16 != 0) return -1;

    const uint8_t *iv = data;
    int ct_len = data_len - 16;
    uint8_t *pt = out;  /* decrypt in-place into out */

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, key, 256);
    int rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, ct_len,
                                   (uint8_t *)iv, data + 16, pt);
    mbedtls_aes_free(&aes);
    if (rc != 0) return -1;

    /* Read the 2-byte LE length prefix to get actual payload length */
    if (ct_len < 2) return -1;
    int plen = pt[0] | (pt[1] << 8);
    if (plen < 0 || plen > ct_len - 2 || plen > out_max) return -1;

    /* Move payload to start of out buffer */
    memmove(out, pt + 2, plen);
    return plen;
}

static void addr_to_str(const ble_addr_t *a, char *out /* >=18 */)
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             a->val[5], a->val[4], a->val[3],
             a->val[2], a->val[1], a->val[0]);
}

static bool str_to_addr(const char *s, ble_addr_t *a)
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) a->val[5 - i] = (uint8_t)v[i];
    a->type = BLE_ADDR_PUBLIC;
    return true;
}

/* ------------------------------------------------------------------ */
/*  FIG framing                                                       */
/* ------------------------------------------------------------------ */
static inline uint32_t crc32_ieee(const uint8_t *d, size_t n)
{
    /* ESP32-S3 ROM CRC32 with init=0 produces standard IEEE CRC-32
     * (the ROM internally handles ~init and ~result). */
    return esp_rom_crc32_le(0, d, n);
}

/* Encode a FIG packet into out (caller supplies buffer >= 16+len). */
static int fig_encode(uint16_t vcid, const uint8_t *payload, uint16_t len,
                      uint8_t *out)
{
    out[0] = FIG_SYNC & 0xFF;
    out[1] = (FIG_SYNC >> 8) & 0xFF;
    out[2] = (FIG_SYNC >> 16) & 0xFF;
    out[3] = (FIG_SYNC >> 24) & 0xFF;
    /* header (8 bytes): vcid LE, len LE, payload_crc LE */
    out[4] = vcid & 0xFF;
    out[5] = (vcid >> 8) & 0xFF;
    out[6] = len & 0xFF;
    out[7] = (len >> 8) & 0xFF;
    uint32_t pcrc = crc32_ieee(payload, len);
    out[8]  = pcrc & 0xFF;
    out[9]  = (pcrc >> 8) & 0xFF;
    out[10] = (pcrc >> 16) & 0xFF;
    out[11] = (pcrc >> 24) & 0xFF;
    uint32_t hcrc = crc32_ieee(&out[4], 8);
    out[12] = hcrc & 0xFF;
    out[13] = (hcrc >> 8) & 0xFF;
    out[14] = (hcrc >> 16) & 0xFF;
    out[15] = (hcrc >> 24) & 0xFF;
    memcpy(&out[16], payload, len);
    return 16 + len;
}

/* Scan the RX accumulation buffer for one complete packet. On success copies
 * the payload to payload_out and returns its length; else returns -1. */
static int fig_take_packet(uint8_t *payload_out, int payload_max, uint16_t *vcid_out)
{
    while (s_rx_len >= 4) {
        /* find sync */
        int idx = -1;
        for (int i = 0; i + 4 <= s_rx_len; i++) {
            if (s_rx_buf[i] == 0xBE && s_rx_buf[i + 1] == 0xBA &&
                s_rx_buf[i + 2] == 0xFE && s_rx_buf[i + 3] == 0xCA) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            /* keep last 3 bytes (possible partial sync) */
            if (s_rx_len > 3) {
                memmove(s_rx_buf, s_rx_buf + s_rx_len - 3, 3);
                s_rx_len = 3;
            }
            return -1;
        }
        if (idx > 0) {
            memmove(s_rx_buf, s_rx_buf + idx, s_rx_len - idx);
            s_rx_len -= idx;
        }
        if (s_rx_len < 4 + FIG_HEADER_LEN) {
            ESP_LOGD(TAG, "fig_take_packet: sync found, but waiting for header (%d bytes total)", s_rx_len);
            return -1;
        }

        const uint8_t *hdr = &s_rx_buf[4];
        uint16_t vcid = hdr[0] | (hdr[1] << 8);
        uint16_t plen = hdr[2] | (hdr[3] << 8);
        uint32_t hcrc = hdr[8] | (hdr[9] << 8) | (hdr[10] << 16) | (hdr[11] << 24);
        uint32_t calc_hcrc = crc32_ieee(hdr, 8);
        if (calc_hcrc != hcrc) {
            ESP_LOGW(TAG, "fig_take_packet: bad header CRC: calc=0x%08x expected=0x%08x", (unsigned)calc_hcrc, (unsigned)hcrc);
            /* bad header: skip this sync and retry */
            memmove(s_rx_buf, s_rx_buf + 4, s_rx_len - 4);
            s_rx_len -= 4;
            continue;
        }
        int total = 4 + FIG_HEADER_LEN + plen;
        if (s_rx_len < total) {
            ESP_LOGD(TAG, "fig_take_packet: header OK, waiting for full payload (need %d, have %d)", total, s_rx_len);
            return -1;   /* wait for more */
        }

        int n = plen;
        if (n > payload_max) n = payload_max;
        memcpy(payload_out, &s_rx_buf[16], n);
        if (vcid_out) *vcid_out = vcid;

        memmove(s_rx_buf, s_rx_buf + total, s_rx_len - total);
        s_rx_len -= total;
        return n;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  GATT callbacks                                                    */
/* ------------------------------------------------------------------ */
static int on_write_done(uint16_t conn, const struct ble_gatt_error *err,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    s_op_status = err ? err->status : 0;
    xSemaphoreGive(s_op_sem);
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t mtu, void *arg)
{
    (void)conn; (void)arg;
    if (err && err->status == 0) s_mtu = mtu;
    s_op_status = err ? err->status : 0;
    xSemaphoreGive(s_op_sem);
    return 0;
}

/* No-op callback for full GATT discovery — generates the same ATT traffic
 * as BlueZ's automatic database discovery (Read By Group Type for all
 * primary services).  We don't store the results; we just need the AS11
 * to see the full discovery sequence before we start RPCs. */
static int on_all_svc(uint16_t conn, const struct ble_gatt_error *err,
                      const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn; (void)arg;
    if (err && err->status == 0 && svc) {
        if (ble_uuid_cmp(&svc->uuid.u, &UUID_SERVICE.u) == 0) {
            s_svc_start = svc->start_handle;
            s_svc_end = svc->end_handle;
            ESP_LOGI(TAG, "ResMed service found: 0x%04x-0x%04x", s_svc_start, s_svc_end);
        }
    }
    if (err && err->status == BLE_HS_EDONE) {
        s_op_status = 0;
        xSemaphoreGive(s_op_sem);
    } else if (err && err->status != 0) {
        s_op_status = err->status;
        xSemaphoreGive(s_op_sem);
    }
    return 0;
}

/* Steehl characteristic UUIDs (128-bit, same as in do_connect_and_discover). */
static const ble_uuid128_t UUID_STEEHL_1 =
    BLE_UUID128_INIT(0xbc,0x67,0x8e,0x50,0xc7,0x1d,0x2e,0x8d,
                     0x56,0x4a,0xa2,0xd8,0xac,0x85,0x50,0x3d);
static const ble_uuid128_t UUID_STEEHL_2 =
    BLE_UUID128_INIT(0x82,0x20,0x5c,0xf5,0xe9,0x65,0x1a,0xb1,
                     0xfa,0x4b,0x98,0x27,0x4f,0xc4,0x81,0x16);
static const ble_uuid128_t UUID_STEEHL_3 =
    BLE_UUID128_INIT(0x1c,0x7a,0xb2,0xac,0xf2,0x1b,0x15,0x9b,
                     0x86,0x4a,0x23,0xc8,0xa4,0x3b,0xe3,0xe5);

/* Full characteristic discovery callback — captures handles for Device Name,
 * Appearance, and Steehl vendor chars so we can Read by Handle (ATT 0x0a)
 * instead of Read By UUID (ATT 0x08), matching the BlueZ btmon trace. */
static int on_all_chr(uint16_t conn, const struct ble_gatt_error *err,
                      const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn; (void)arg;
    if (err && err->status == 0 && chr) {
        const ble_uuid16_t uuid_devname  = BLE_UUID16_INIT(0x2a00);
        const ble_uuid16_t uuid_appear   = BLE_UUID16_INIT(0x2a01);
        if (ble_uuid_cmp(&chr->uuid.u, &uuid_devname.u) == 0) {
            s_devname_handle = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &uuid_appear.u) == 0) {
            s_appearance_handle = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &UUID_TX.u) == 0) {
            s_tx_handle = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &UUID_RX.u) == 0) {
            s_rx_handle = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &UUID_STEEHL_1.u) == 0) {
            s_steehl_handles[0] = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &UUID_STEEHL_2.u) == 0) {
            s_steehl_handles[1] = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &UUID_STEEHL_3.u) == 0) {
            s_steehl_handles[2] = chr->val_handle;
        }
    }
    if (err && err->status == BLE_HS_EDONE) {
        s_op_status = 0;
        xSemaphoreGive(s_op_sem);
    } else if (err && err->status != 0) {
        s_op_status = err->status;
        xSemaphoreGive(s_op_sem);
    }
    return 0;
}

static int on_steehl_read(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)arg;
    if (error && error->status == 0 && attr) {
        ESP_LOGI(TAG, "Steehl read success: len=%d", attr->om ? OS_MBUF_PKTLEN(attr->om) : 0);
        s_op_status = 0;
        xSemaphoreGive(s_op_sem);
    } else if (error && error->status == BLE_HS_EDONE) {
        s_op_status = 0;
        xSemaphoreGive(s_op_sem);
    } else if (error && error->status != 0) {
        s_op_status = error->status;
        xSemaphoreGive(s_op_sem);
    }
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                  void *arg)
{
    (void)conn; (void)chr_val_handle; (void)arg;
    if (err && err->status == 0 && dsc) {
        char uuid_str[37];
        ble_uuid_to_str(&dsc->uuid.u, uuid_str);
        ESP_LOGI(TAG, "Discovered descriptor: %s (handle=%d)", uuid_str, dsc->handle);
        const ble_uuid16_t cccd = BLE_UUID16_INIT(0x2902);
        if (ble_uuid_cmp(&dsc->uuid.u, &cccd.u) == 0) {
            if (s_cccd_handle == 0) {
                s_cccd_handle = dsc->handle;
                ESP_LOGI(TAG, "Using CCCD at handle %d", s_cccd_handle);
            }
        }
    }
    if (err && err->status == BLE_HS_EDONE) {
        s_op_status = s_cccd_handle ? 0 : BLE_HS_ENOENT;
        xSemaphoreGive(s_op_sem);
    } else if (err && err->status != 0) {
        s_op_status = err->status;
        xSemaphoreGive(s_op_sem);
    }
    return 0;
}

static void handle_notify(const uint8_t *data, int len);

/* Notification processing task — drains the queue and does the heavy
 * FIG/AES/cJSON work off the NimBLE host task. */
static void notif_proc_task(void *arg)
{
    (void)arg;
    notif_item_t item;
    while (1) {
        if (xQueueReceive(s_notif_queue, &item, portMAX_DELAY) == pdTRUE) {
            handle_notify(item.data, item.len);
            free(item.data);
        }
    }
}

static void handle_notify(const uint8_t *data, int len)
{
    ESP_LOGD(TAG, "handle_notify: len=%d", len);
    if (s_rx_len + len > RX_BUF_MAX) {
        ESP_LOGW(TAG, "rx buffer overflow, resetting");
        s_rx_len = 0;
        return;
    }
    memcpy(s_rx_buf + s_rx_len, data, len);
    s_rx_len += len;

    static uint8_t *payload = NULL;
    static uint8_t *decrypted = NULL;
    if (!payload)   payload   = heap_caps_malloc(RX_BUF_MAX, MALLOC_CAP_SPIRAM);
    if (!decrypted) decrypted = heap_caps_malloc(RX_BUF_MAX, MALLOC_CAP_SPIRAM);
    if (!payload || !decrypted) {
        ESP_LOGE(TAG, "handle_notify: failed to allocate PSRAM decrypt buffers");
        s_rx_len = 0;
        return;
    }
    uint16_t vcid;
    int n;
    while ((n = fig_take_packet(payload, RX_BUF_MAX - 1, &vcid)) >= 0) {
        ESP_LOGD(TAG, "FIG packet received: vcid=0x%04x len=%d", vcid, n);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, payload, n > 64 ? 64 : n, ESP_LOG_DEBUG);

        /* Decrypt encrypted payloads (VCID 0x0396) using the session key. */
        const uint8_t *parse_ptr = payload;
        int            parse_len = n;

        if (vcid == FIG_VCID_RX_ENC && s_session_encrypted) {
            int dlen = aes_cbc_decrypt(s_session_key, payload, n,
                                       decrypted, RX_BUF_MAX - 1);
            if (dlen < 0) {
                ESP_LOGW(TAG, "AES decrypt failed for vcid=0x%04x len=%d", vcid, n);
                continue;
            }
            decrypted[dlen] = '\0';
            parse_ptr = decrypted;
            parse_len = dlen;
            ESP_LOGD(TAG, "decrypted vcid=0x%04x len=%d", vcid, dlen);
        } else {
            payload[n] = '\0';
        }

        /* Fast path: check if this is a StreamData notification.
         * StreamData is high-frequency (~5/sec) and benefits from bypassing
         * cJSON tree building. We scan the raw decrypted text for the method
         * string. If it's StreamData, use the fast parser and skip cJSON. */
        if (parse_len > 20) {
            /* Ensure null-terminated for strstr */
            ((char *)parse_ptr)[parse_len] = '\0';
            if (strstr((const char *)parse_ptr, "\"method\":\"StreamData\"") != NULL) {
                session_writer_on_stream_data_raw((const char *)parse_ptr, parse_len);
                continue;
            }
        }

        /* Slow path: full cJSON parse for events, RPC responses, etc. */
        cJSON *msg = cJSON_ParseWithLength((const char *)parse_ptr, parse_len);
        if (!msg && parse_len > 2) {
            uint16_t inner = (uint16_t)(parse_ptr[0] | (parse_ptr[1] << 8));
            if (inner > 0 && inner <= (uint16_t)(parse_len - 2)) {
                ESP_LOGD(TAG, "stripping 2-byte length prefix (%u), retrying JSON parse", inner);
                parse_ptr += 2;
                parse_len  = inner;
                msg = cJSON_ParseWithLength((const char *)parse_ptr, parse_len);
            }
        }
        if (!msg) {
            ESP_LOGW(TAG, "non-JSON FIG payload vcid=0x%04x len=%d", vcid, n);
            continue;
        }
        cJSON *id = cJSON_GetObjectItem(msg, "id");
        cJSON *method = cJSON_GetObjectItem(msg, "method");
        if (method && !id) {
            const char *m = method->valuestring ? method->valuestring : "?";

            /* SpoolFragment interception: when a spool pull is in progress,
             * route SpoolFragment notifications to the collector instead of
             * the normal notification dispatch.  The collector buffers
             * decoded base64 fragments and signals completion when the
             * device reports SPOOL_COMPLETE or SPOOL_COMPLETE_MORE_DATA_PENDING. */
            if (s_spool_collector && strcmp(m, "SpoolFragment") == 0) {
                cJSON *params = cJSON_GetObjectItem(msg, "params");
                if (params) {
                    cJSON *seq_j = cJSON_GetObjectItem(params, "seq");
                    cJSON *data_j = cJSON_GetObjectItem(params, "data");
                    cJSON *status_j = cJSON_GetObjectItem(params, "status");

                    if (data_j && cJSON_IsString(data_j) &&
                        s_spool_collector->frag_count < SPOOL_MAX_FRAGS) {

                        /* Base64-decode the fragment data.
                         * An empty base64 string (dec_len=0) is valid —
                         * it means the fragment has no payload (e.g. the
                         * device sent a fragment with only a status field).
                         * In that case we skip decoding but MUST still fall
                         * through to the status check below — otherwise the
                         * collector semaphore is never signaled and the pull
                         * times out. */
                        const char *b64 = data_j->valuestring;
                        size_t b64_len = strlen(b64);
                        if (b64_len > 0) {
                            size_t dec_max = (b64_len / 4) * 3 + 4;
                            uint8_t *frag = heap_caps_malloc(dec_max, MALLOC_CAP_SPIRAM);
                            if (!frag) frag = malloc(dec_max);
                            if (frag) {
                                size_t dec_len = 0;
                                int rc = mbedtls_base64_decode(
                                    frag, dec_max, &dec_len,
                                    (const unsigned char *)b64, b64_len);
                                if (rc == 0 && dec_len > 0) {
                                    int idx = s_spool_collector->frag_count++;
                                    s_spool_collector->frags[idx].seq =
                                        seq_j ? seq_j->valueint : idx;
                                    s_spool_collector->frags[idx].data = frag;
                                    s_spool_collector->frags[idx].len = (int)dec_len;
                                    ESP_LOGI(TAG, "spool frag %d: seq=%d len=%d",
                                             idx, s_spool_collector->frags[idx].seq,
                                             (int)dec_len);
                                } else if (rc != 0) {
                                    ESP_LOGW(TAG, "base64 decode failed rc=%d", rc);
                                    free(frag);
                                } else {
                                    /* rc == 0 but dec_len == 0 — empty payload */
                                    free(frag);
                                }
                            }
                        }
                    }

                    /* Check completion status */
                    if (status_j && cJSON_IsString(status_j)) {
                        strlcpy(s_spool_collector->status,
                                status_j->valuestring,
                                sizeof(s_spool_collector->status));
                        /* Capture nextSpoolAddress for multi-round pulls */
                        cJSON *next_j = cJSON_GetObjectItem(params, "nextSpoolAddress");
                        if (next_j) {
                            char *ns = cJSON_PrintUnformatted(next_j);
                            if (ns) {
                                strlcpy(s_spool_collector->next_addr_json, ns,
                                        sizeof(s_spool_collector->next_addr_json));
                                free(ns);
                            }
                        }
                        /* Capture spoolHash — documented (rpc_protocol.md)
                         * as the SHA-256 of the concatenated raw fragment
                         * data, sent on the terminal fragment.  Verified
                         * after concatenation in spool_one_round(). */
                        cJSON *hash_j = cJSON_GetObjectItem(params, "spoolHash");
                        if (hash_j && cJSON_IsString(hash_j)) {
                            strlcpy(s_spool_collector->spool_hash_hex,
                                    hash_j->valuestring,
                                    sizeof(s_spool_collector->spool_hash_hex));
                        }
                        /* ERROR_* statuses are terminal failures, not
                         * completion — record them so the pull can tell
                         * "no more data" apart from "device refused"
                         * (e.g. ERROR_DATA_UNAVAILABLE). */
                        if (strncmp(status_j->valuestring, "ERROR_", 6) == 0) {
                            s_spool_collector->error = true;
                        }
                        if (strcmp(status_j->valuestring, "SPOOL_INCOMPLETE") != 0) {
                            s_spool_collector->done = true;
                            xSemaphoreGive(s_spool_collector->sem);
                        }
                    }
                }
                cJSON_Delete(msg);
                continue;
            }

            /* Normal notification (HeartBeat, therapy events, data).
             * Forward to session writer for therapy detection and data logging. */
            ESP_LOGD(TAG, "notification: %s", m);
            session_writer_on_notification(session_writer_get_active(), msg);
            cJSON_Delete(msg);
            continue;
        }
        /* RPC response */
        if (s_resp_json) cJSON_Delete(s_resp_json);
        s_resp_json = msg;
        xSemaphoreGive(s_resp_sem);
    }
}

/* True when the SomnoBridge owns CPAP BLE slot 1. */
static bool bridge_owns_slot1(void)
{
    return sb_ble_is_paired() || sb_ble_get_status()[0] != 'i';
}

static void auto_reconnect_task(void *arg);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields f;
        char addr_str[18];
        const uint8_t *addr = event->disc.addr.val;
        snprintf(addr_str, sizeof(addr_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

        const uint8_t *raw = event->disc.data;
        int raw_len = event->disc.length_data;

        int rc = ble_hs_adv_parse_fields(&f, raw, raw_len);
        if (rc != 0) {
            /* NimBLE rejects the entire packet if any AD structure is
             * truncated.  The AS11's ADV_IND has a trailing field (type
             * 0x1b) that is 1 byte short, causing rc=10 (BLE_HS_EBADDATA)
             * even though the name field earlier in the payload is valid.
             * Manually walk the AD structures to salvage what we can. */
            ESP_LOGD(TAG, "Scan report from %s: parse failed (rc=%d), "
                     "trying manual AD parse", addr_str, rc);
            memset(&f, 0, sizeof(f));
            as11_adv_fields_t sal;
            as11_adv_salvage(raw, raw_len, &sal);
            f.name        = sal.name;
            f.name_len    = (uint8_t)sal.name_len;
            /* The cast stays HERE rather than inside as11_adv_salvage(): a
             * UUID list begins at an arbitrary offset in the payload and so
             * carries no alignment guarantee, and the salvage parser returns
             * bytes precisely so that the one place assuming otherwise is
             * visible. Unchanged from what shipped. */
            f.uuids16     = (const ble_uuid16_t *)sal.uuids16;
            f.num_uuids16 = (uint8_t)sal.num_uuids16;
        }

        bool match = false;
        char name[32] = {0};
        if (f.name != NULL && f.name_len > 0) {
            int nl = f.name_len < (int)sizeof(name) - 1 ? f.name_len : (int)sizeof(name) - 1;
            memcpy(name, f.name, nl);
            name[nl] = '\0';
            if (strncmp(name, DEVICE_NAME_PREFIX, strlen(DEVICE_NAME_PREFIX)) == 0) {
                match = true;
            }
        }

        // Also check 16-bit UUIDs for ResMed service 0xfd56
        if (!match && f.uuids16 != NULL && f.num_uuids16 > 0) {
            for (int i = 0; i < f.num_uuids16; i++) {
                if (f.uuids16[i].value == UUID_SERVICE.value) {
                    match = true;
                    if (name[0] == '\0') {
                        strlcpy(name, "ResMed AS11", sizeof(name));
                    }
                    break;
                }
            }
        }

        /* SomnoBridge: name prefix "SomnoBri" (31-byte adv truncation) or
         * the private 128-bit service UUID. */
        bool bridge = false;
        if (name[0] != '\0' && strncmp(name, "SomnoBri", 8) == 0) {
            bridge = true;
        } else if (f.uuids128 != NULL && f.num_uuids128 > 0) {
            for (int i = 0; i < f.num_uuids128; i++) {
                if (ble_uuid_cmp(&f.uuids128[i].u, &UUID_SB_SVC.u) == 0) {
                    bridge = true;
                    if (name[0] == '\0') strlcpy(name, "SomnoBridge", sizeof(name));
                    break;
                }
            }
        }
        if (bridge) match = true;

        if (!match) {
            return 0;
        }
        /* dedupe by address */
        for (int i = 0; i < s_scan_count; i++) {
            if (memcmp(&s_scan[i].addr, &event->disc.addr, sizeof(ble_addr_t)) == 0) {
                s_scan[i].rssi = event->disc.rssi;
                if (bridge) s_scan[i].bridge = true;
                /* If the first packet matched by UUID only (fallback name
                 * "ResMed AS11") and the scan response now carries the real
                 * device name (e.g. "ResMed 436648"), update it.  Never
                 * overwrite a real name with the fallback. */
                if (name[0] != '\0' &&
                    strncmp(name, "ResMed AS11", 11) != 0) {
                    strlcpy(s_scan[i].name, name, sizeof(s_scan[i].name));
                }
                ESP_LOGD(TAG, "Scan update: %s (name: '%s', rssi: %d)",
                         addr_str, s_scan[i].name, event->disc.rssi);
                return 0;
            }
        }
        if (s_scan_count < SCAN_MAX) {
            s_scan[s_scan_count].addr = event->disc.addr;
            strlcpy(s_scan[s_scan_count].name, name, sizeof(s_scan[s_scan_count].name));
            s_scan[s_scan_count].rssi = event->disc.rssi;
            s_scan[s_scan_count].bridge = bridge;
            s_scan_count++;
            ESP_LOGI(TAG, "found '%s' rssi=%d", name, event->disc.rssi);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        xSemaphoreGive(s_scan_done);
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        s_connect_status = event->connect.status;
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_manual_disconnect = false;
            s_supv_update_sent = false;   /* per-connection Phase-D attempt */
            s_supv_tmo_context_pending = false;
            /* Fresh reception counters for this connection. */
            s_notif_last_rx_us = 0;
            s_notif_rx_total = 0;
            s_notif_gap_events = 0;
            s_notif_last_gap_us = 0;
        }
        xSemaphoreGive(s_connect_sem);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        /* Fires after every applied parameter change — the AS11's own
         * L2CAP request and any update we initiated.  The desc is the
         * negotiated result, logged in natural units. */
        if (event->conn_update.status == 0) {
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(event->conn_update.conn_handle, &d) == 0) {
                ESP_LOGI(TAG, "conn params updated: itvl=%u (%.1fms) lat=%u "
                         "timeout=%u (%ums)",
                         d.conn_itvl, d.conn_itvl * 1.25, d.conn_latency,
                         d.supervision_timeout, d.supervision_timeout * 10);
            }
        } else {
            ESP_LOGW(TAG, "conn params update failed (status=%d)",
                     event->conn_update.status);
        }
        return 0;

    case BLE_GAP_EVENT_LINK_ESTAB:
        /* A fully-established link — log the parameters we ended up with
         * (the initial values come from ble_gap_connect's defaults, the
         * AS11 usually re-negotiates immediately afterwards). */
        if (event->link_estab.status == 0) {
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(event->link_estab.conn_handle, &d) == 0) {
                ESP_LOGI(TAG, "link established: handle=%u itvl=%u (%.1fms) "
                         "lat=%u timeout=%u (%ums)",
                         d.conn_handle, d.conn_itvl, d.conn_itvl * 1.25,
                         d.conn_latency, d.supervision_timeout,
                         d.supervision_timeout * 10);
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        /* Forensics for the reason-520 investigation
         * (.ai/RECONNECT/PLAN.md Phase A): the event carries a snapshot of
         * the negotiated parameters, so we can finally replace the
         * inferred "~3 s" timeout with the real value. */
        const struct ble_gap_conn_desc *dc = &event->disconnect.conn;
        ESP_LOGW(TAG, "disconnected (reason=%d / %s) handle=%u "
                 "itvl=%u (%.1fms) lat=%u timeout=%u (%ums)",
                 event->disconnect.reason,
                 hci_err_str(event->disconnect.reason),
                 dc->conn_handle, dc->conn_itvl, dc->conn_itvl * 1.25,
                 dc->conn_latency, dc->supervision_timeout,
                 dc->supervision_timeout * 10);
        if ((event->disconnect.reason & 0xFF) == 0x08) {
            /* Connection supervision timeout — the failure mode under
             * investigation.  Latch it for the deferred diagnostic pull
             * and arm the bounded context line that auto_reconnect_task
             * emits (it runs off this callback, so it can afford Wi-Fi
             * and heap queries that are unsafe here). */
            as11_ble_incident_mark("supv_timeout");
            s_supv_tmo_context_pending = true;
        }
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_session_encrypted = false;
        therapy_alert_on_ble_disconnect();

        /* Auto-reconnect if we have valid pairing info, the disconnect
         * was not intentional, and we were in the PAIRED state.
         *
         * Without auto-reconnect, a transient BLE link loss during therapy
         * causes us to miss the TherapyStop event.  The session then stays
         * open and the next TherapyStart merges into it, producing a single
         * glued session instead of two separate ones.
         *
         * s_manual_disconnect is set before every intentional
         * ble_gap_terminate() call (user disconnect, forget pairing,
         * confirm_task before its own reconnect_task launch) to suppress
         * this auto-reconnect.
         *
         * reconnect_task failure paths set state to ERROR or IDLE before
         * calling ble_gap_terminate, so the state check here prevents
         * recursive auto-reconnect when session setup fails. */
        if (s_pair_cache.valid && !s_manual_disconnect &&
            strcmp(s_state, AS11_STATUS_PAIRED) == 0) {
            ESP_LOGI(TAG, "auto-reconnect: unexpected disconnect while paired, "
                         "launching reconnect");
            if (bridge_owns_slot1()) {
                ESP_LOGI(TAG, "auto-reconnect skipped: SomnoBridge owns slot 1");
            } else {
                psram_task_create(auto_reconnect_task, "as11_reconn", 8192,
                                  NULL, 5, tskNO_AFFINITY, NULL, NULL);
            }
        } else {
            ESP_LOGI(TAG, "auto-reconnect: skipped (pairing=%d manual=%d state=%s)",
                     s_pair_cache.valid, s_manual_disconnect, s_state);
        }
        s_manual_disconnect = false;
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "Encryption change: status=%d", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU updated: %d", s_mtu);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint16_t notif_len = OS_MBUF_PKTLEN(event->notify_rx.om);
        ESP_LOGD(TAG, "Notification RX: handle=%d len=%d", event->notify_rx.attr_handle, notif_len);
        /* Reception forensics: only the timestamp and counters live here, so
         * the cost is two reads per notification. */
        {
            int64_t now_us = esp_timer_get_time();
            if (s_notif_last_rx_us != 0 &&
                now_us - s_notif_last_rx_us > NOTIF_GAP_THRESHOLD_US) {
                s_notif_gap_events++;
                s_notif_last_gap_us = now_us;
            }
            s_notif_last_rx_us = now_us;
            s_notif_rx_total++;
        }
        if (notif_len > 0 && s_notif_queue) {
            /* Queue-depth metrics.  Each element owns a separately malloc'd
             * payload, so depth translates into a real worst-case backlog —
             * the right way to size NOTIF_QUEUE_LEN is from the observed
             * high-water mark and dropped bytes under a slow card, not from
             * a guessed item count. */
            UBaseType_t depth = uxQueueMessagesWaiting(s_notif_queue);
            if (depth > s_notif_hwm) {
                s_notif_hwm = depth;
                if (depth >= (NOTIF_QUEUE_LEN * 3) / 4) {
                    ESP_LOGW(TAG, "notif queue high-water %u/%u",
                             (unsigned)depth, (unsigned)NOTIF_QUEUE_LEN);
                }
            }

            uint8_t *notif_data = heap_caps_malloc(notif_len, MALLOC_CAP_SPIRAM);
            if (!notif_data) notif_data = malloc(notif_len);
            if (notif_data) {
                int rc = os_mbuf_copydata(event->notify_rx.om, 0, notif_len, notif_data);
                if (rc == 0) {
                    notif_item_t item = { .data = notif_data, .len = notif_len };
                    if (xQueueSend(s_notif_queue, &item, 0) != pdTRUE) {
                        /* Queue full — drop oldest, then enqueue */
                        notif_item_t dropped;
                        xQueueReceive(s_notif_queue, &dropped, 0);
                        s_notif_dropped++;
                        s_notif_dropped_bytes += (uint32_t)dropped.len;
                        free(dropped.data);
                        xQueueSend(s_notif_queue, &item, 0);
                        notif_data = NULL;  /* owned by queue now */
                        ESP_LOGW(TAG, "notif queue full, dropped 1 item "
                                 "(total %u items / %u bytes)",
                                 (unsigned)s_notif_dropped,
                                 (unsigned)s_notif_dropped_bytes);
                    } else {
                        notif_data = NULL;  /* owned by queue now */
                    }
                } else {
                    ESP_LOGE(TAG, "os_mbuf_copydata failed: %d", rc);
                }
                if (notif_data) free(notif_data);
            } else {
                s_notif_alloc_fail++;
                ESP_LOGE(TAG, "failed to allocate notif_data (%u bytes, "
                         "%u alloc failures)", (unsigned)notif_len,
                         (unsigned)s_notif_alloc_fail);
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        /* The AS11 asked for new connection parameters — either over
         * L2CAP (BLE_GAP_EVENT_L2CAP_UPDATE_REQ) or via the Link-Layer
         * Connection Parameters Request procedure (CONN_UPDATE_REQ).
         * Both share the conn_update_req union member; peer_params
         * carries what it asked for and returning 0 accepts it (NimBLE
         * then issues the HCI update and reports the negotiated result
         * via BLE_GAP_EVENT_CONN_UPDATE, logged above).
         *
         * We deliberately do NOT rewrite self_params here: on the L2CAP
         * path NimBLE does not use it, and on the LL path modifying
         * inside the callback is untested against the AS11.  An
         * experimental timeout raise is applied separately once the
         * peer's own update settles (Phase D in .ai/RECONNECT/PLAN.md). */
        ESP_LOGI(TAG, "peer conn-param request (event %d): itvl=%u-%u "
                 "lat=%u timeout=%u (%ums) — accepting",
                 event->type,
                 event->conn_update_req.peer_params->itvl_min,
                 event->conn_update_req.peer_params->itvl_max,
                 event->conn_update_req.peer_params->latency,
                 event->conn_update_req.peer_params->supervision_timeout,
                 event->conn_update_req.peer_params->supervision_timeout * 10);
        return 0; /* 0 to accept, non-zero to reject */

    case BLE_GAP_EVENT_DATA_LEN_CHG:
        ESP_LOGI(TAG, "DLE changed: tx=%u octets/%u us rx=%u octets/%u us",
                 event->data_len_chg.max_tx_octets,
                 event->data_len_chg.max_tx_time,
                 event->data_len_chg.max_rx_octets,
                 event->data_len_chg.max_rx_time);
        return 0;

    default:
        ESP_LOGW(TAG, "Unhandled GAP event: type=%d", event->type);
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Blocking GATT helpers (run from worker tasks, not host callbacks) */
/* ------------------------------------------------------------------ */
/* Clear any stale operation semaphore tokens from timed-out callbacks. */
static void clear_op_sem(void)
{
    while (xSemaphoreTake(s_op_sem, 0) == pdTRUE) { /* drain */ }
}

static int wait_op(int timeout_ms)
{
    if (xSemaphoreTake(s_op_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return BLE_HS_ETIMEOUT;
    }
    return s_op_status;
}

static uint8_t *fig_tx_pkt(void)
{
    static uint8_t *pkt;
    if (!pkt) {
        pkt = heap_caps_malloc(16 + TX_PAYLOAD_MAX, MALLOC_CAP_SPIRAM);
        if (!pkt) {
            ESP_LOGE(TAG, "fig_tx_pkt: PSRAM alloc failed");
        }
    }
    return pkt;
}

static esp_err_t send_fig(uint16_t vcid, const char *json)
{
    uint8_t *pkt = fig_tx_pkt();
    if (!pkt) return ESP_ERR_NO_MEM;
    uint16_t plen = (uint16_t)strlen(json);
    if (plen > TX_PAYLOAD_MAX) return ESP_ERR_INVALID_SIZE;
    int total = fig_encode(vcid, (const uint8_t *)json, plen, pkt);

    ESP_LOGI(TAG, "send_fig: VCID=0x%04x payload=%d total=%d", vcid, plen, total);
    ESP_LOG_BUFFER_HEX(TAG, pkt, total);

    /* Stream the FIG packet as chunked Write Requests, exactly like the
     * Python client's _send_raw().  The AS11 treats this characteristic
     * as a stream and concatenates incoming writes.  We must NOT use
     * Write Long (Prepare+Execute) — the AS11 rejects it (ATT error 3)
     * and the failed attempt may leave the GATT server in a bad state. */
    int chunk = s_mtu > 3 ? s_mtu - 3 : 20;
    ESP_LOGI(TAG, "send_fig: chunk_size=%d (mtu=%d)", chunk, s_mtu);
    for (int off = 0; off < total; off += chunk) {
        int n = total - off;
        if (n > chunk) n = chunk;
        clear_op_sem();
        int rc = ble_gattc_write_flat(s_conn_handle, s_tx_handle, pkt + off, n, on_write_done, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "write chunk start failed rc=%d off=%d", rc, off);
            return ESP_FAIL;
        }
        int wrc = wait_op(5000);
        if (wrc != 0) {
            ESP_LOGE(TAG, "write chunk failed off=%d (rc=%d)", off, wrc);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/* Send a raw binary FIG packet (for encrypted payloads). */
static esp_err_t send_fig_raw(uint16_t vcid, const uint8_t *data, int len)
{
    uint8_t *pkt = fig_tx_pkt();
    if (!pkt) return ESP_ERR_NO_MEM;
    if (len > TX_PAYLOAD_MAX) return ESP_ERR_INVALID_SIZE;
    int total = fig_encode(vcid, data, (uint16_t)len, pkt);

    ESP_LOGI(TAG, "send_fig_raw: VCID=0x%04x payload=%d total=%d", vcid, len, total);

    int chunk = s_mtu > 3 ? s_mtu - 3 : 20;
    for (int off = 0; off < total; off += chunk) {
        int n = total - off;
        if (n > chunk) n = chunk;
        clear_op_sem();
        int rc = ble_gattc_write_flat(s_conn_handle, s_tx_handle, pkt + off, n, on_write_done, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "write chunk failed rc=%d off=%d", rc, off);
            return ESP_FAIL;
        }
        int wrc = wait_op(5000);
        if (wrc != 0) {
            ESP_LOGE(TAG, "write chunk timeout off=%d (rc=%d)", off, wrc);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/* Send an encrypted JSON-RPC request on VCID 0x0397.
 * Encrypts with AES-256-CBC using the session key, then sends via send_fig_raw.
 * Returns ESP_OK on success. Caller should call wait_response() to get reply. */
static esp_err_t send_rpc_encrypted(const char *json)
{
    if (!s_session_encrypted) {
        ESP_LOGE(TAG, "send_rpc_encrypted: no session key");
        return ESP_ERR_INVALID_STATE;
    }
    int plen = (int)strlen(json);
    uint8_t *enc = heap_caps_malloc(TX_PAYLOAD_MAX, MALLOC_CAP_SPIRAM);
    if (!enc) {
        enc = malloc(TX_PAYLOAD_MAX);
        if (!enc) return ESP_ERR_NO_MEM;
        ESP_LOGW(TAG, "send_rpc_encrypted: fell back to default malloc (PSRAM?)");
    }

    int enc_len = aes_cbc_encrypt(s_session_key, (const uint8_t *)json, plen,
                                  enc, TX_PAYLOAD_MAX);
    if (enc_len < 0) {
        free(enc);
        ESP_LOGE(TAG, "AES encrypt failed");
        return ESP_FAIL;
    }

    esp_err_t ret = send_fig_raw(FIG_VCID_TX_ENC, enc, enc_len);
    free(enc);
    return ret;
}

/* Clear any stale RPC response state before sending a new RPC. */
static void clear_response(void)
{
    while (xSemaphoreTake(s_resp_sem, 0) == pdTRUE) { /* drain */ }
    if (s_resp_json) { cJSON_Delete(s_resp_json); s_resp_json = NULL; }
}

/* Wait for an RPC response; returns parsed cJSON (caller frees) or NULL. */
static cJSON *wait_response(int timeout_ms)
{
    if (xSemaphoreTake(s_resp_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return NULL;
    }
    cJSON *j = s_resp_json;
    s_resp_json = NULL;
    return j;
}

/* ── RPC response-channel serialization ───────────────────────────────
 * The shared response channel (s_resp_sem / s_resp_json) carries exactly
 * one outstanding request.  Every caller performing a
 * clear_response() → send → wait_response() cycle MUST hold s_cmd_mtx
 * for the whole cycle; otherwise a concurrent caller (e.g. an HTTP
 * passthrough RPC racing the post-therapy spool pull) can drain the
 * semaphore or overwrite s_resp_json mid-flight and both sides see
 * garbage or timeouts.
 *
 * Scope rules (see .ai/RECONNECT/PLAN.md Phase B.2):
 *  - Lock at operation granularity around each send/wait triplet.
 *  - NEVER hold the mutex across a call into another public RPC
 *    function (they lock internally; s_cmd_mtx is not recursive).
 *    reconnect_task is the main example: it wraps each of its three
 *    triplets individually and lets as11_ble_get_datetime() take the
 *    lock itself in between.
 *  - spool_one_round() is internal and expects the caller to hold the
 *    mutex for the whole multi-round pull. */
static bool rpc_channel_lock(int timeout_ms)
{
    return xSemaphoreTake(s_cmd_mtx, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
static void rpc_channel_unlock(void)
{
    xSemaphoreGive(s_cmd_mtx);
}

/* ------------------------------------------------------------------ */
/*  SRP-6a                                                            */
/* ------------------------------------------------------------------ */
static void srp_free(void)
{
    if (!s_srp.active) return;
    mbedtls_mpi_free(&s_srp.N);
    mbedtls_mpi_free(&s_srp.g);
    mbedtls_mpi_free(&s_srp.a);
    mbedtls_mpi_free(&s_srp.A);
    s_srp.active = false;
}

/* Generate client keypair: a random, A = g^a mod N. */
static esp_err_t srp_begin(void)
{
    int64_t t0 = esp_timer_get_time();
    srp_free();
    memset(&s_srp, 0, sizeof(s_srp));
    mbedtls_mpi_init(&s_srp.N);
    mbedtls_mpi_init(&s_srp.g);
    mbedtls_mpi_init(&s_srp.a);
    mbedtls_mpi_init(&s_srp.A);
    s_srp.active = true;

    if (mbedtls_mpi_read_string(&s_srp.N, 16, SRP_N_HEX) != 0) return ESP_FAIL;
    if (mbedtls_mpi_lset(&s_srp.g, 2) != 0) return ESP_FAIL;

    uint8_t rnd[32];
    esp_fill_random(rnd, sizeof(rnd));
    if (mbedtls_mpi_read_binary(&s_srp.a, rnd, sizeof(rnd)) != 0) return ESP_FAIL;

    int64_t t1 = esp_timer_get_time();
    mbedtls_mpi RR;
    mbedtls_mpi_init(&RR);
    int rc = mbedtls_mpi_exp_mod(&s_srp.A, &s_srp.g, &s_srp.a, &s_srp.N, &RR);
    mbedtls_mpi_free(&RR);
    int64_t t2 = esp_timer_get_time();
    ESP_LOGI(TAG, "srp_begin: modexp took %lld ms", (t2 - t1) / 1000);
    if (rc != 0) return ESP_FAIL;

    if (mbedtls_mpi_write_binary(&s_srp.A, s_srp.A_pad, SRP_PAD) != 0) return ESP_FAIL;
    ESP_LOGI(TAG, "srp_begin: total %lld ms", (esp_timer_get_time() - t0) / 1000);
    return ESP_OK;
}

/* Complete SRP: compute S, K, M1; verify server M2; outputs hex M1 and K. */
static esp_err_t srp_compute(const char *server_pk_hex, const char *salt_hex,
                             const char *passkey,
                             char m1_hex_out[65], char *server_conf_check /*64+1*/)
{
    esp_err_t result = ESP_FAIL;
    uint8_t N_pad[SRP_PAD], g_pad[SRP_PAD], B_pad[SRP_PAD];
    uint8_t salt[64];
    int salt_len = hex_to_bytes(salt_hex, salt, sizeof(salt));
    if (salt_len < 0) return ESP_FAIL;

    mbedtls_mpi B, k, x, u, gx, kgx, base, exp, ux, S, RR, tmp;
    mbedtls_mpi_init(&B); mbedtls_mpi_init(&k); mbedtls_mpi_init(&x);
    mbedtls_mpi_init(&u); mbedtls_mpi_init(&gx); mbedtls_mpi_init(&kgx);
    mbedtls_mpi_init(&base); mbedtls_mpi_init(&exp); mbedtls_mpi_init(&ux);
    mbedtls_mpi_init(&S); mbedtls_mpi_init(&RR); mbedtls_mpi_init(&tmp);

    if (mbedtls_mpi_read_string(&B, 16, server_pk_hex) != 0) goto done;
    if (mbedtls_mpi_write_binary(&s_srp.N, N_pad, SRP_PAD) != 0) goto done;
    if (mbedtls_mpi_write_binary(&s_srp.g, g_pad, SRP_PAD) != 0) goto done;
    if (mbedtls_mpi_write_binary(&B, B_pad, SRP_PAD) != 0) goto done;

    /* k = H(pad(N) || pad(g)) */
    uint8_t kbuf[32];
    {
        const uint8_t *segs[] = { N_pad, g_pad };
        size_t lens[] = { SRP_PAD, SRP_PAD };
        sha256_segs(segs, lens, 2, kbuf);
    }
    if (mbedtls_mpi_read_binary(&k, kbuf, 32) != 0) goto done;

    /* x = H(salt || H(passkey_ascii)) */
    uint8_t hpk[32], xbuf[32];
    {
        const uint8_t *segs[] = { (const uint8_t *)passkey };
        size_t lens[] = { strlen(passkey) };
        sha256_segs(segs, lens, 1, hpk);
        const uint8_t *segs2[] = { salt, hpk };
        size_t lens2[] = { (size_t)salt_len, 32 };
        sha256_segs(segs2, lens2, 2, xbuf);
    }
    if (mbedtls_mpi_read_binary(&x, xbuf, 32) != 0) goto done;

    /* u = H(pad(A) || pad(B)) */
    uint8_t ubuf[32];
    {
        const uint8_t *segs[] = { s_srp.A_pad, B_pad };
        size_t lens[] = { SRP_PAD, SRP_PAD };
        sha256_segs(segs, lens, 2, ubuf);
    }
    if (mbedtls_mpi_read_binary(&u, ubuf, 32) != 0) goto done;

    /* S = (B - k * g^x)^(a + u*x) mod N */
    int64_t te1 = esp_timer_get_time();
    if (mbedtls_mpi_exp_mod(&gx, &s_srp.g, &x, &s_srp.N, &RR) != 0) goto done; /* g^x */
    int64_t te2 = esp_timer_get_time();
    ESP_LOGI(TAG, "srp_compute: g^x modexp took %lld ms", (te2 - te1) / 1000);
    if (mbedtls_mpi_mul_mpi(&kgx, &k, &gx) != 0) goto done;                     /* k*g^x */
    if (mbedtls_mpi_mod_mpi(&kgx, &kgx, &s_srp.N) != 0) goto done;
    if (mbedtls_mpi_sub_mpi(&base, &B, &kgx) != 0) goto done;                   /* B - k*g^x */
    if (mbedtls_mpi_mod_mpi(&base, &base, &s_srp.N) != 0) goto done;            /* normalise (handles negative) */
    if (mbedtls_mpi_mul_mpi(&ux, &u, &x) != 0) goto done;                       /* u*x */
    if (mbedtls_mpi_add_mpi(&exp, &s_srp.a, &ux) != 0) goto done;               /* a + u*x */
    int64_t te3 = esp_timer_get_time();
    if (mbedtls_mpi_exp_mod(&S, &base, &exp, &s_srp.N, &RR) != 0) goto done;
    int64_t te4 = esp_timer_get_time();
    ESP_LOGI(TAG, "srp_compute: S modexp took %lld ms", (te4 - te3) / 1000);

    uint8_t S_pad[SRP_PAD];
    if (mbedtls_mpi_write_binary(&S, S_pad, SRP_PAD) != 0) goto done;

    /* K = H(pad(S)) */
    uint8_t K[32];
    {
        const uint8_t *segs[] = { S_pad };
        size_t lens[] = { SRP_PAD };
        sha256_segs(segs, lens, 1, K);
    }
    bytes_to_hex(K, 32, s_srp.master_key_hex);

    /* M1 = H((H(N) xor H(g)) || salt || pad(A) || pad(B) || K) */
    uint8_t hN[32], hG[32], hxor[32];
    {
        const uint8_t *sN[] = { N_pad }; size_t lN[] = { SRP_PAD };
        sha256_segs(sN, lN, 1, hN);
        const uint8_t *sG[] = { g_pad }; size_t lG[] = { SRP_PAD };
        sha256_segs(sG, lG, 1, hG);
        for (int i = 0; i < 32; i++) hxor[i] = hN[i] ^ hG[i];
    }
    uint8_t M1[32];
    {
        const uint8_t *segs[] = { hxor, salt, s_srp.A_pad, B_pad, K };
        size_t lens[] = { 32, (size_t)salt_len, SRP_PAD, SRP_PAD, 32 };
        sha256_segs(segs, lens, 5, M1);
    }
    bytes_to_hex(M1, 32, m1_hex_out);

    /* expected M2 = H(pad(A) || M1 || K)  (RFC 5054 SRP-6a server proof,
     * verified against the device's serverConfirmation from a live pairing). */
    uint8_t M2[32];
    {
        const uint8_t *segs[] = { s_srp.A_pad, M1, K };
        size_t lens[] = { SRP_PAD, 32, 32 };
        sha256_segs(segs, lens, 3, M2);
    }
    bytes_to_hex(M2, 32, server_conf_check);

    result = ESP_OK;
done:
    mbedtls_mpi_free(&B); mbedtls_mpi_free(&k); mbedtls_mpi_free(&x);
    mbedtls_mpi_free(&u); mbedtls_mpi_free(&gx); mbedtls_mpi_free(&kgx);
    mbedtls_mpi_free(&base); mbedtls_mpi_free(&exp); mbedtls_mpi_free(&ux);
    mbedtls_mpi_free(&S); mbedtls_mpi_free(&RR); mbedtls_mpi_free(&tmp);
    return result;
}

/* ------------------------------------------------------------------ */
/*  NVS credential store                                              */
/* ------------------------------------------------------------------ */
struct nvs_pair_arg {
    char addr[18];
    char name[32];
    char client_id[64];
    char pair_key[80];
};

static esp_err_t do_save_pairing_nvs(void *arg)
{
    const struct nvs_pair_arg *a = arg;
    struct nvs_pair_arg local = *a;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_str(h, NVS_K_ADDR, local.addr);
    nvs_set_str(h, NVS_K_NAME, local.name);
    nvs_set_str(h, NVS_K_CLIENTID, local.client_id);
    nvs_set_str(h, NVS_K_PAIRKEY, local.pair_key);
    e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) {
        strlcpy(s_pair_cache.addr, local.addr, sizeof(s_pair_cache.addr));
        strlcpy(s_pair_cache.name, local.name, sizeof(s_pair_cache.name));
        strlcpy(s_pair_cache.client_id, local.client_id, sizeof(s_pair_cache.client_id));
        strlcpy(s_pair_cache.pair_key, local.pair_key, sizeof(s_pair_cache.pair_key));
        s_pair_cache.valid = true;
    }
    return e;
}

static bool nvs_get_str_opt(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t len = cap;
    return nvs_get_str(h, key, out, &len) == ESP_OK && out[0] != '\0';
}

/* Load paired credentials from NVS into the in-RAM cache.  Called once at
 * init so that as11_ble_is_paired() and as11_ble_get_paired_info() can
 * serve from RAM without touching flash on every HTTP /api/status poll. */
static void pair_cache_load(void)
{
    memset(&s_pair_cache, 0, sizeof(s_pair_cache));
    nvs_handle_t h;
    nvs_writer_lock();
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) { nvs_writer_unlock(); return; }
    bool ok = nvs_get_str_opt(h, NVS_K_CLIENTID, s_pair_cache.client_id,
                              sizeof(s_pair_cache.client_id));
    nvs_get_str_opt(h, NVS_K_ADDR, s_pair_cache.addr, sizeof(s_pair_cache.addr));
    nvs_get_str_opt(h, NVS_K_NAME, s_pair_cache.name, sizeof(s_pair_cache.name));
    nvs_get_str_opt(h, NVS_K_PAIRKEY, s_pair_cache.pair_key,
                    sizeof(s_pair_cache.pair_key));
    nvs_close(h);
    nvs_writer_unlock();
    s_pair_cache.valid = ok;
}

/* ------------------------------------------------------------------ */
/*  Worker tasks                                                      */
/* ------------------------------------------------------------------ */
/* --- do_connect_and_discover ---
 * Mirrors the exact GATT sequence from the btmon capture of a successful
 * Python/BlueZ pairing session.  Order matters — the AS11 appears to be
 * sensitive to the sequence of ATT operations.
 *
 *   1. Exchange MTU
 *   2. Full service discovery (Read By Group Type: Primary Service)
 *   3. Full characteristic discovery (Read By Type: Characteristic)
 *      -> captures TX, RX, Device Name, Appearance, Steehl handles
 *   4. Descriptor discovery (Find Information) for RX CCCD
 *   5. Write Client Supported Features (0x2B29) -> handle 0x0008, val 0x05
 *   6. Subscribe to Service Changed (Indicate) -> handle 0x0004, val 0x0200
 *   7. Read Device Name, Appearance, Steehl characteristics by handle
 *   8. Re-write Service Changed CCCD (BlueZ does this twice)
 *   9. Enable notifications -> write 0x0100 to RX CCCD
 */
static esp_err_t do_connect_and_discover(void)
{
    s_tx_handle = s_rx_handle = s_cccd_handle = 0;
    s_svc_start = s_svc_end = 0;
    s_rx_len = 0;
    s_mtu = 23;
    s_devname_handle = s_appearance_handle = 0;
    s_steehl_handles[0] = s_steehl_handles[1] = s_steehl_handles[2] = 0;

    int rc = ble_gap_connect(s_own_addr_type, &s_target_addr, 30000, NULL,
                             gap_event, NULL);
    if (rc != 0) { set_error("connect start failed"); return ESP_FAIL; }
    if (xSemaphoreTake(s_connect_sem, pdMS_TO_TICKS(31000)) != pdTRUE) {
        set_error("connect timeout"); return ESP_FAIL;
    }
    if (s_connect_status != 0) { set_error("connection failed"); return ESP_FAIL; }
    ESP_LOGI(TAG, "Connected, conn_handle=%d", s_conn_handle);

    /* 1. Exchange MTU.
     * The AS11 often initiates the MTU exchange immediately after connect,
     * so by the time we call ble_gattc_exchange_mtu the exchange is already
     * done and our on_mtu callback never fires.  The BLE_GAP_EVENT_MTU
     * handler updates s_mtu regardless, so a short wait suffices. */
    clear_op_sem();
    ble_gattc_exchange_mtu(s_conn_handle, on_mtu, NULL);
    wait_op(500);
    if (s_mtu <= 23) {
        /* callback didn't fire and GAP event hasn't arrived yet — wait a bit more */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "MTU negotiated: %d", s_mtu);

    /* 2. Full service discovery (Read By Group Type: Primary Service).
     * Captures ResMed service (0xFD56) handle range. */
    clear_op_sem();
    ble_gattc_disc_all_svcs(s_conn_handle, on_all_svc, NULL);
    wait_op(10000);
    ESP_LOGI(TAG, "Full service discovery done");

    if (s_svc_start == 0 || s_svc_end == 0) {
        set_error("ResMed service not found"); return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ResMed service: 0x%04x-0x%04x", s_svc_start, s_svc_end);

    /* 3. Full characteristic discovery (Read By Type: Characteristic).
     * Captures TX, RX, Device Name, Appearance, and Steehl handles. */
    clear_op_sem();
    ble_gattc_disc_all_chrs(s_conn_handle, 1, 65535, on_all_chr, NULL);
    wait_op(10000);
    ESP_LOGI(TAG, "Full characteristic discovery done");

    ESP_LOGI(TAG, "TX handle=%d, RX handle=%d", s_tx_handle, s_rx_handle);
    if (s_tx_handle == 0 || s_rx_handle == 0) {
        set_error("TX/RX characteristic not found"); return ESP_FAIL;
    }

    /* 4. Descriptor discovery (Find Information) for RX CCCD.
     * NimBLE internally adds 1 to the start handle, so pass s_rx_handle
     * to begin the search at s_rx_handle+1 (where the CCCD lives). */
    clear_op_sem();
    if (ble_gattc_disc_all_dscs(s_conn_handle, s_rx_handle, s_svc_end, on_dsc, NULL) != 0 ||
        wait_op(10000) != 0) {
        set_error("CCCD not found"); return ESP_FAIL;
    }
    ESP_LOGI(TAG, "RX CCCD handle=%d", s_cccd_handle);

    /* 5. Write Client Supported Features (0x2B29) at handle 0x0008 */
    {
        uint8_t csf_val[1] = { 0x05 };
        clear_op_sem();
        rc = ble_gattc_write_flat(s_conn_handle, 0x0008, csf_val, 1, on_write_done, NULL);
        if (rc != 0) { ESP_LOGW(TAG, "CSF write start failed rc=%d", rc); }
        else { wait_op(5000); ESP_LOGI(TAG, "CSF written (handle 0x0008)"); }
    }

    /* 6. Subscribe to Service Changed (Indicate) at handle 0x0004 */
    {
        uint8_t sc_val[2] = { 0x02, 0x00 };
        clear_op_sem();
        rc = ble_gattc_write_flat(s_conn_handle, 0x0004, sc_val, 2, on_write_done, NULL);
        if (rc != 0) { ESP_LOGW(TAG, "Service Changed sub failed rc=%d", rc); }
        else { wait_op(5000); ESP_LOGI(TAG, "Service Changed subscribed (handle 0x0004)"); }
    }

    /* 7. Read Device Name, Appearance, and Steehl characteristics by handle.
     * The btmon trace shows BlueZ uses ATT Read Request (opcode 0x0a) by handle,
     * NOT Read By Type (opcode 0x08) by UUID.  The AS11 may be sensitive to this. */
    if (s_devname_handle) {
        ESP_LOGI(TAG, "Reading Device Name (handle %d)...", s_devname_handle);
        clear_op_sem();
        ble_gattc_read(s_conn_handle, s_devname_handle, on_steehl_read, NULL);
        wait_op(5000);
    }
    if (s_appearance_handle) {
        ESP_LOGI(TAG, "Reading Appearance (handle %d)...", s_appearance_handle);
        clear_op_sem();
        ble_gattc_read(s_conn_handle, s_appearance_handle, on_steehl_read, NULL);
        wait_op(5000);
    }
    for (int i = 0; i < 3; i++) {
        if (s_steehl_handles[i]) {
            ESP_LOGI(TAG, "Reading Steehl characteristic %d (handle %d)...", i + 1, s_steehl_handles[i]);
            clear_op_sem();
            ble_gattc_read(s_conn_handle, s_steehl_handles[i], on_steehl_read, NULL);
            wait_op(5000);
        }
    }

    /* 8. Re-write Service Changed CCCD (BlueZ does this twice in the trace) */
    {
        uint8_t sc_val[2] = { 0x02, 0x00 };
        clear_op_sem();
        rc = ble_gattc_write_flat(s_conn_handle, 0x0004, sc_val, 2, on_write_done, NULL);
        if (rc == 0) { wait_op(5000); ESP_LOGI(TAG, "Service Changed re-subscribed"); }
    }

    /* 9. Enable notifications: write 0x0001 to RX CCCD */
    uint8_t cccd_val[2] = { 0x01, 0x00 };
    clear_op_sem();
    if (ble_gattc_write_flat(s_conn_handle, s_cccd_handle, cccd_val, 2,
                             on_write_done, NULL) != 0 || wait_op(5000) != 0) {
        set_error("enable notify failed"); return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Notifications enabled on handle %d", s_cccd_handle);
    return ESP_OK;
}

static void reconnect_task(void *arg);

static void pair_task(void *arg)
{
    (void)arg;
    /* Use the built-in public MAC address to guarantee compatibility with the CPAP BLE stack */
    s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    
    set_state(AS11_STATUS_CONNECTING);

    if (do_connect_and_discover() != ESP_OK) {
        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        vTaskDelete(NULL);
        return;
    }



    int64_t pair_t0 = esp_timer_get_time();
    if (srp_begin() != ESP_OK) {
        set_error("SRP keygen failed");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "pair_task: srp_begin took %lld ms", (esp_timer_get_time() - pair_t0) / 1000);

    /* StartKeyExchange */
    char A_hex[SRP_PAD * 2 + 1];

    ESP_LOGI(TAG, "Sending StartKeyExchange...");
    bytes_to_hex(s_srp.A_pad, SRP_PAD, A_hex);
    char *json = heap_caps_malloc(800, MALLOC_CAP_SPIRAM);
    if (!json) json = malloc(800);
    if (!json) {
        set_error("StartKeyExchange: out of memory");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelete(NULL);
        return;
    }
    snprintf(json, 800,
             "{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"StartKeyExchange\","
             "\"params\":{\"clientPk\":\"%s\"}}", A_hex);
    /* Hold the RPC channel lock across the whole send+wait cycle — the
     * response arrives on the shared characteristic and must not be
     * consumed by another RPC user (pairing is user-interactive and
     * rare, but a concurrent post-therapy pull would otherwise corrupt
     * both flows). */
    if (!rpc_channel_lock(30000)) {
        set_error("RPC channel busy during pairing");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelete(NULL);
        return;
    }
    clear_response();
    esp_err_t se = send_fig(FIG_VCID_TX, json);
    free(json);
    if (se != ESP_OK) {
        rpc_channel_unlock();
        set_error("StartKeyExchange send failed");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelete(NULL);
        return;
    }

    /* The AS11 shows the passkey as soon as it receives StartKeyExchange,
     * well before it finishes sending the response.  Update the UI state
     * now so the user can start entering the code while we wait for the
     * response (serverPk + salt) in the background. */
    set_state(AS11_STATUS_WAIT_PASSKEY);

    /* Post-send delay: give the AS11 time to process the packet before
     * we start waiting for the response (matches Python client). */
    vTaskDelay(pdMS_TO_TICKS(100));

    cJSON *resp = wait_response(30000);
    rpc_channel_unlock();
    if (!resp) {
        set_error("no StartKeyExchange response");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelete(NULL);
        return;
    }
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    cJSON *spk = result ? cJSON_GetObjectItem(result, "serverPk") : NULL;
    cJSON *salt = result ? cJSON_GetObjectItem(result, "salt") : NULL;
    if (!cJSON_IsString(spk) || !cJSON_IsString(salt)) {
        cJSON_Delete(resp);
        set_error("bad StartKeyExchange response");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelete(NULL);
        return;
    }
    /* stash serverPk + salt for the confirm step */
    strlcpy(s_server_pk, spk->valuestring, SERVER_PK_MAX);
    strlcpy(s_salt, salt->valuestring, SALT_MAX);
    s_kex_ready = true;
    cJSON_Delete(resp);

    ESP_LOGI(TAG, "StartKeyExchange OK - device should show a 4-digit passkey");
    vTaskDelete(NULL);
}

static void confirm_task(void *arg)
{
    (void)arg;
    set_state(AS11_STATUS_CONFIRMING);

    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        set_error("not connected"); vTaskDelete(NULL); return;
    }
    /* The StartKeyExchange response may still be arriving if the user
     * entered the PIN quickly.  Wait up to 5 s for s_kex_ready. */
    for (int i = 0; i < 50 && !s_kex_ready; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_kex_ready || !s_server_pk || !s_salt) {
        set_error("no key-exchange state"); vTaskDelete(NULL); return;
    }

    char m1_hex[65];
    char m2_expected[65];
    int64_t conf_t0 = esp_timer_get_time();
    if (srp_compute(s_server_pk, s_salt, s_passkey,
                    m1_hex, m2_expected) != ESP_OK) {
        set_error("SRP computation failed");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "confirm_task: srp_compute took %lld ms", (esp_timer_get_time() - conf_t0) / 1000);

    char json[256];
    snprintf(json, sizeof(json),
             "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ConfirmKeyExchange\","
             "\"params\":{\"clientConfirmation\":\"%s\"}}", m1_hex);
    if (!rpc_channel_lock(20000)) {
        set_error("RPC channel busy during confirm");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelete(NULL);
        return;
    }
    clear_response();
    if (send_fig(FIG_VCID_TX, json) != ESP_OK) {
        rpc_channel_unlock();
        set_error("ConfirmKeyExchange send failed");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelete(NULL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    int64_t resp_t0 = esp_timer_get_time();
    cJSON *resp = wait_response(15000);
    rpc_channel_unlock();
    ESP_LOGI(TAG, "confirm_task: wait_response took %lld ms", (esp_timer_get_time() - resp_t0) / 1000);
    if (!resp) {
        set_error("no ConfirmKeyExchange response");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelete(NULL);
        return;
    }
    cJSON *err = cJSON_GetObjectItem(resp, "error");
    if (err) {
        cJSON_Delete(resp);
        set_error("device rejected passkey");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelete(NULL);
        return;
    }
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    cJSON *cid = result ? cJSON_GetObjectItem(result, "clientId") : NULL;
    cJSON *sconf = result ? cJSON_GetObjectItem(result, "serverConfirmation") : NULL;
    if (!cJSON_IsString(cid) || !cJSON_IsString(sconf)) {
        cJSON_Delete(resp);
        set_error("bad ConfirmKeyExchange response");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelete(NULL);
        return;
    }
    /* verify server proof (M2) */
    if (strcasecmp(sconf->valuestring, m2_expected) != 0) {
        ESP_LOGE(TAG, "server proof mismatch: got %s expected %s",
                 sconf->valuestring, m2_expected);
        cJSON_Delete(resp);
        set_error("server proof mismatch");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelete(NULL);
        return;
    }

    char addr_str[18];
    addr_to_str(&s_target_addr, addr_str);
    char cid_buf[64];
    strlcpy(cid_buf, cid->valuestring, sizeof(cid_buf));
    struct nvs_pair_arg nvarg;
    strlcpy(nvarg.addr, addr_str, sizeof(nvarg.addr));
    strlcpy(nvarg.name, s_target_name, sizeof(nvarg.name));
    strlcpy(nvarg.client_id, cid->valuestring, sizeof(nvarg.client_id));
    strlcpy(nvarg.pair_key, s_srp.master_key_hex, sizeof(nvarg.pair_key));
    esp_err_t ns = nvs_writer_run(do_save_pairing_nvs, &nvarg);
    cJSON_Delete(resp);
    if (ns != ESP_OK) {
        set_error("NVS save failed");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "paired with %s (clientId=%s)", addr_str, cid_buf);
    session_writer_set_device_info(addr_str, cid_buf);
    set_state(AS11_STATUS_PAIRED);
    srp_free();

    /* Disconnect the pairing link; reconnect_task will re-establish
     * the connection with encrypted session + stream subscriptions.
     * Without this, therapy data never flows until a manual reboot.
     * Set s_manual_disconnect so the GAP disconnect event doesn't
     * launch a duplicate auto_reconnect_task. */
    s_manual_disconnect = true;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    psram_task_create(reconnect_task, "as11_reconn", 8192, NULL, 5, tskNO_AFFINITY, NULL, NULL);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Encrypted session handshake helper                                */
/* ------------------------------------------------------------------ */
static esp_err_t do_setup_encrypted_session(const char *cid_str,
                                            const char *pair_key_hex,
                                            bool *out_hard_fail)
{
    if (out_hard_fail) *out_hard_fail = false;

    /* ---- Encrypted session establishment ----
     * 1. RequestSession(clientId) -> {challenge, nonce}  [plaintext RPC]
     * 2. response = HMAC-SHA256(K, challenge)
     * 3. CheckSessionIntegrity(response)                  [plaintext RPC]
     * 4. session_key = SHA256(K || nonce)
     */
    uint8_t K_bytes[32];
    if (hex_to_bytes(pair_key_hex, K_bytes, sizeof(K_bytes)) != 32) {
        ESP_LOGE(TAG, "reconnect: bad pair key");
        set_error("bad pair key in cache");
        if (out_hard_fail) *out_hard_fail = true;
        return ESP_ERR_INVALID_ARG;
    }

    /* 1. RequestSession */
    char *rpc = heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    if (!rpc) rpc = malloc(512);
    if (!rpc) return ESP_ERR_NO_MEM;
    snprintf(rpc, 512,
             "{\"id\":10,\"jsonrpc\":\"2.0\",\"method\":\"RequestSession\","
             "\"params\":{\"clientId\":\"%s\"}}", cid_str);
    if (!rpc_channel_lock(15000)) {
        free(rpc);
        ESP_LOGW(TAG, "reconnect: RPC channel busy (RequestSession)");
        return ESP_ERR_TIMEOUT;
    }
    clear_response();
    esp_err_t se = send_fig(FIG_VCID_TX, rpc);
    free(rpc);
    if (se != ESP_OK) {
        rpc_channel_unlock();
        ESP_LOGW(TAG, "reconnect: RequestSession send failed");
        return ESP_FAIL;
    }

    cJSON *resp = wait_response(10000);
    rpc_channel_unlock();
    if (!resp) {
        ESP_LOGW(TAG, "reconnect: RequestSession timeout");
        return ESP_ERR_TIMEOUT;
    }
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    cJSON *challenge_j = result ? cJSON_GetObjectItem(result, "challenge") : NULL;
    cJSON *nonce_j = result ? cJSON_GetObjectItem(result, "nonce") : NULL;
    if (!cJSON_IsString(challenge_j) || !cJSON_IsString(nonce_j)) {
        ESP_LOGE(TAG, "reconnect: RequestSession missing challenge/nonce");
        cJSON_Delete(resp);
        set_error("bad RequestSession response");
        if (out_hard_fail) *out_hard_fail = true;
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* 2. HMAC-SHA256(K, challenge) */
    uint8_t challenge_bytes[64];
    int ch_len = hex_to_bytes(challenge_j->valuestring, challenge_bytes, sizeof(challenge_bytes));
    if (ch_len <= 0) {
        ESP_LOGE(TAG, "reconnect: bad challenge hex");
        cJSON_Delete(resp);
        set_error("bad challenge hex in session setup");
        if (out_hard_fail) *out_hard_fail = true;
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t hmac_out[32];
    hmac_sha256(K_bytes, 32, challenge_bytes, ch_len, hmac_out);
    char response_hex[65];
    bytes_to_hex(hmac_out, 32, response_hex);

    /* Save nonce before deleting resp (needed for key derivation later) */
    char nonce_hex_saved[130];
    strlcpy(nonce_hex_saved, nonce_j->valuestring, sizeof(nonce_hex_saved));

    ESP_LOGI(TAG, "reconnect: challenge=%s... response=%s...",
             challenge_j->valuestring, response_hex);

    /* 3. CheckSessionIntegrity */
    rpc = heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    if (!rpc) rpc = malloc(512);
    if (!rpc) {
        cJSON_Delete(resp);
        return ESP_ERR_NO_MEM;
    }
    snprintf(rpc, 512,
             "{\"id\":11,\"jsonrpc\":\"2.0\",\"method\":\"CheckSessionIntegrity\","
             "\"params\":{\"response\":\"%s\"}}", response_hex);
    if (!rpc_channel_lock(15000)) {
        free(rpc);
        cJSON_Delete(resp);
        ESP_LOGW(TAG, "reconnect: RPC channel busy (CheckSessionIntegrity)");
        return ESP_ERR_TIMEOUT;
    }
    clear_response();
    se = send_fig(FIG_VCID_TX, rpc);
    free(rpc);
    cJSON_Delete(resp);
    if (se != ESP_OK) {
        rpc_channel_unlock();
        ESP_LOGW(TAG, "reconnect: CheckSessionIntegrity send failed");
        return ESP_FAIL;
    }

    resp = wait_response(10000);
    rpc_channel_unlock();
    if (!resp) {
        ESP_LOGW(TAG, "reconnect: CheckSessionIntegrity timeout");
        return ESP_ERR_TIMEOUT;
    }
    cJSON *err = cJSON_GetObjectItem(resp, "error");
    if (err) {
        ESP_LOGE(TAG, "reconnect: CheckSessionIntegrity error: %s",
                 cJSON_PrintUnformatted(err));
        cJSON_Delete(resp);
        set_error("device rejected session integrity");
        if (out_hard_fail) *out_hard_fail = true;
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "reconnect: session integrity verified");
    cJSON_Delete(resp);

    /* 4. Derive session key = SHA256(K || nonce) */
    uint8_t nonce_bytes[64];
    int nonce_len = hex_to_bytes(nonce_hex_saved, nonce_bytes, sizeof(nonce_bytes));
    if (nonce_len <= 0) {
        ESP_LOGE(TAG, "reconnect: bad nonce hex");
        set_error("bad nonce hex in session setup");
        if (out_hard_fail) *out_hard_fail = true;
        return ESP_ERR_INVALID_RESPONSE;
    }

    {
        const uint8_t *segs[] = { K_bytes, nonce_bytes };
        size_t lens[] = { 32, (size_t)nonce_len };
        sha256_segs(segs, lens, 2, s_session_key);
    }
    s_session_encrypted = true;
    ESP_LOGI(TAG, "reconnect: session key derived");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Auto-reconnect (uses saved NVS pairing, no SRP needed)             */
/* ------------------------------------------------------------------ */

/* Phase D (experimental, opt-in): raise the supervision timeout via a
 * central-initiated LL_CONNECTION_UPDATE_IND.  As central we send the
 * update unilaterally — the AS11 cannot negotiate it back; its only outs
 * are dropping the link or issuing another L2CAP request later, so we
 * attempt this at most once per connection and never loop.
 *
 * Runs in reconnect_task (own task context, NOT the GAP callback): the
 * call issues an HCI command and waits for the controller's response.
 * Preserves the AS11's negotiated interval and latency and changes only
 * the supervision timeout. */
static void apply_exp_supv_timeout(void)
{
    uint16_t target_ms = as11_ble_exp_supv_timeout_ms();
    if (target_ms == 0 || s_supv_update_sent ||
        s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    s_supv_update_sent = true;   /* once per connection, success or not */

    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(s_conn_handle, &d) != 0) {
        return;
    }
    uint16_t target_units = target_ms / 100;   /* BLE: 10 ms units */
    if (d.supervision_timeout >= target_units) {
        ESP_LOGI(TAG, "exp: supervision timeout already %ums >= target %ums",
                 d.supervision_timeout * 10, target_ms);
        return;
    }

    struct ble_gap_upd_params params = {
        .itvl_min = d.conn_itvl,        /* keep AS11's negotiated timing */
        .itvl_max = d.conn_itvl,
        .latency  = d.conn_latency,
        .supervision_timeout = target_units,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    ESP_LOGW(TAG, "exp: requesting supervision timeout %ums (was %ums), "
             "itvl/latency preserved", target_ms, d.supervision_timeout * 10);
    int rc = ble_gap_update_params(s_conn_handle, &params);
    if (rc != 0) {
        /* BLE_HS_EALREADY = another update still pending — do not retry;
         * the peer's update wins this connection. */
        ESP_LOGW(TAG, "exp: ble_gap_update_params rc=%d (EALREADY=%d)",
                 rc, BLE_HS_EALREADY);
    }
}

static void reconnect_task(void *arg)
{
    (void)arg;

    /* Wait for NimBLE host to be ready */
    int wait_ms = 0;
    while (!s_host_ready && wait_ms < 10000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_ms += 100;
    }
    if (!s_host_ready) {
        ESP_LOGW(TAG, "reconnect: host not ready, aborting");
        vTaskDelete(NULL);
        return;
    }

    /* Use cached pairing info (loaded at init or after pairing). */
    if (!s_pair_cache.valid) {
        ESP_LOGD(TAG, "reconnect: no cached pairing");
        vTaskDelete(NULL);
        return;
    }

    char addr_str[24];
    char name_str[32];
    char cid_str[64];
    char pair_key_hex[65];
    strlcpy(addr_str, s_pair_cache.addr, sizeof(addr_str));
    strlcpy(name_str, s_pair_cache.name, sizeof(name_str));
    strlcpy(cid_str, s_pair_cache.client_id, sizeof(cid_str));
    strlcpy(pair_key_hex, s_pair_cache.pair_key, sizeof(pair_key_hex));

    if (addr_str[0] == '\0' || cid_str[0] == '\0' || pair_key_hex[0] == '\0') {
        ESP_LOGD(TAG, "reconnect: incomplete pairing cache");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "reconnect: connecting to %s (%s)", addr_str, name_str);

    /* Parse address and set up target */
    s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    if (!str_to_addr(addr_str, &s_target_addr)) {
        ESP_LOGE(TAG, "reconnect: bad addr '%s'", addr_str);
        vTaskDelete(NULL);
        return;
    }
    strlcpy(s_target_name, name_str, sizeof(s_target_name));

    int setup_failures = 0;
    while (1) {
        if (!s_pair_cache.valid || s_manual_disconnect) {
            ESP_LOGI(TAG, "reconnect: aborted (pairing=%d manual=%d)",
                     s_pair_cache.valid, s_manual_disconnect);
            set_state(AS11_STATUS_IDLE);
            vTaskDelete(NULL);
            return;
        }

        set_state(AS11_STATUS_CONNECTING);

        /* Two-phase retry. The SCHEDULE lives in as11_reconnect.c, which has no
         * ESP-IDF dependency and is covered by scripts/as11_reconnect_test.c;
         * this loop owns only the radio work and the abort checks.
         * Phase 1 — fast: AS11_RECONNECT_FAST_ATTEMPTS tries, 4s then 6s, for a
         *   transient disconnect (the AS11 needs a few seconds to restart
         *   advertising).
         * Phase 2 — slow: AS11_RECONNECT_SLOW_DELAY_S between attempts for when
         *   the AS11 is off at boot and turned on later (with the 30s BLE connect
         *   timeout, a ~90s cycle).  Without this, a single failed reconnect_task
         *   at boot means the ST never connects to the AS11 until manually
         *   rebooted. */
        bool connected = false;
        for (int attempt = 1; attempt <= AS11_RECONNECT_FAST_ATTEMPTS && !connected; attempt++) {
            int delay_s = as11_reconnect_delay_s(attempt);
            if (delay_s > 0) {
                ESP_LOGI(TAG, "reconnect: retry %d/%d in %ds",
                         attempt, AS11_RECONNECT_FAST_ATTEMPTS, delay_s);
                vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));
            }
            if (!s_pair_cache.valid || s_manual_disconnect) {
                ESP_LOGI(TAG, "reconnect: aborted (pairing=%d manual=%d)",
                         s_pair_cache.valid, s_manual_disconnect);
                set_state(AS11_STATUS_IDLE);
                vTaskDelete(NULL);
                return;
            }
            if (do_connect_and_discover() == ESP_OK) {
                connected = true;
                break;
            }
            ESP_LOGW(TAG, "reconnect: connect/discover attempt %d/%d failed: %s",
                     attempt, AS11_RECONNECT_FAST_ATTEMPTS, as11_ble_get_error());
            if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            }
        }

        /* Phase 2: slow retry loop — AS11 may not be powered yet. */
        int slow_attempt = 0;
        while (!connected) {
            if (!s_pair_cache.valid || s_manual_disconnect) {
                ESP_LOGI(TAG, "reconnect: aborted during slow retry "
                             "(pairing=%d manual=%d)",
                         s_pair_cache.valid, s_manual_disconnect);
                set_state(AS11_STATUS_IDLE);
                vTaskDelete(NULL);
                return;
            }
            slow_attempt++;
            /* The same schedule function, continuing the attempt count past the
             * fast phase — so both phases read their timing from one place. */
            int wait_s = as11_reconnect_delay_s(AS11_RECONNECT_FAST_ATTEMPTS + slow_attempt);
            ESP_LOGI(TAG, "reconnect: waiting %ds before slow retry %d...",
                     wait_s, slow_attempt);
            for (int i = 0; i < wait_s; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (!s_pair_cache.valid || s_manual_disconnect) break;
            }
            if (!s_pair_cache.valid || s_manual_disconnect) {
                ESP_LOGI(TAG, "reconnect: aborted during slow retry wait");
                set_state(AS11_STATUS_IDLE);
                vTaskDelete(NULL);
                return;
            }
            set_state(AS11_STATUS_CONNECTING);
            ESP_LOGI(TAG, "reconnect: slow retry %d connecting...", slow_attempt);
            if (do_connect_and_discover() == ESP_OK) {
                connected = true;
                break;
            }
            ESP_LOGW(TAG, "reconnect: slow retry %d failed: %s",
                     slow_attempt, as11_ble_get_error());
            if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            }
            set_state(AS11_STATUS_IDLE);
        }
        if (!connected) {
            ESP_LOGE(TAG, "reconnect: all connect attempts failed");
            set_state(AS11_STATUS_IDLE);
            vTaskDelete(NULL);
            return;
        }

        /* Set device info for session metadata */
        session_writer_set_device_info(addr_str, cid_str);

        /* ---- Encrypted session establishment ---- */
        bool hard_fail = false;
        esp_err_t err = do_setup_encrypted_session(cid_str, pair_key_hex, &hard_fail);
        if (err == ESP_OK) {
            break; /* Session established successfully, proceed to stream setup */
        }

        setup_failures++;
        ESP_LOGE(TAG, "reconnect: session setup failed (%s, attempt %d, hard=%d)",
                 esp_err_to_name(err), setup_failures, hard_fail);

        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        }

        if (hard_fail && setup_failures >= 3) {
            ESP_LOGE(TAG, "reconnect: persistent session setup rejection, latching error state");
            set_state(AS11_STATUS_ERROR);
            vTaskDelete(NULL);
            return;
        }

        /* Transient failure: idle state so disconnect handler doesn't spawn auto_reconnect,
         * wait a brief backoff (5s) for AS11 to resume advertising, then retry. */
        set_state(AS11_STATUS_IDLE);
        for (int i = 0; i < 5; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (!s_pair_cache.valid || s_manual_disconnect) break;
        }
    }

    /* Small delay to let the AS11 process the session establishment */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* ---- Test encrypted channel: Get device identity ---- */
    char *rpc = heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    cJSON *resp = NULL;
    if (!rpc) rpc = malloc(512);
    if (!rpc) {
        ESP_LOGW(TAG, "reconnect: Get (test) skipped, out of memory");
        goto after_get_test;
    }
    snprintf(rpc, 512,
             "{\"id\":12,\"jsonrpc\":\"1.0\",\"method\":\"Get\","
             "\"params\":[\"ProductGeographicIdentifier\",\"HardwareIdentifier\"]}");
    if (rpc_channel_lock(15000)) {
        clear_response();
        if (send_rpc_encrypted(rpc) != ESP_OK) {
            ESP_LOGW(TAG, "reconnect: Get (test) send failed");
        } else {
            resp = wait_response(10000);
            if (resp) {
                ESP_LOGI(TAG, "reconnect: Get (test) response received");
                cJSON_Delete(resp);
            } else {
                ESP_LOGW(TAG, "reconnect: Get (test) timeout (non-fatal)");
            }
        }
        rpc_channel_unlock();
    } else {
        ESP_LOGW(TAG, "reconnect: RPC channel busy, Get (test) skipped");
    }
    free(rpc);
after_get_test:

    /* ---- Log AS11 firmware identity (once per connection) ----
     * GetVersion reports FlowGenerator/BT/Cellular module software
     * versions.  Correlating this with incidents rules out
     * firmware-specific behaviour (e.g. the 8.6.0 unit under
     * investigation).  Non-fatal, sent before StartStream while ACL
     * buffers are still free. */
    {
        char verbuf[512];
        if (as11_ble_get_version(verbuf, sizeof(verbuf)) == ESP_OK) {
            ESP_LOGI(TAG, "reconnect: AS11 identity: %s", verbuf);
        } else {
            ESP_LOGW(TAG, "reconnect: GetVersion failed (non-fatal)");
        }
    }


    /* ---- Wait for usable time before subscribing to events or
     * starting the data stream.  Therapy recording depends on accurate
     * time for session timestamps, clock drift calculation, and spool
     * staleness detection.
     *
     * Primary source: NTP (time_sync_is_synced()).
     * Fallback: AS11 clock + stored drift (time_sync_recover_from_as11()).
     * If neither is available, block until NTP eventually syncs. */
    if (!time_sync_is_synced()) {
        ESP_LOGI(TAG, "reconnect: NTP not synced, attempting AS11 drift recovery");
        esp_err_t rec = time_sync_recover_from_as11();
        if (rec == ESP_OK) {
            ESP_LOGI(TAG, "reconnect: time recovered from AS11 + drift (degraded mode)");
        } else {
            ESP_LOGW(TAG, "reconnect: AS11 drift recovery failed (%s), "
                     "blocking until NTP syncs", esp_err_to_name(rec));
            while (!time_is_usable()) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
    }
    ESP_LOGI(TAG, "reconnect: time usable (source=%d), proceeding with stream setup",
             (int)time_source_get());

    /* Capture machine identification and therapy settings while the link is
     * idle (before StartStream begins 25 Hz notifications), so a same-machine
     * fallback snapshot is on the SD card even on the very first night or if
     * BLE drops at TherapyStop.  Skipped during active therapy (where stream
     * gap time matters) and while post-therapy collection is in flight (which
     * performs its own retry as soon as reconnect completes). */
    if (!session_writer_get_active() && !session_writer_post_active() &&
        sd_storage_is_ready()) {
        post_therapy_cache_device_metadata();
    }

    /* ---- Subscribe to therapy events (encrypted) ----
     * Four event selectors:
     *   TherapyEvents-RespiratoryEvents  — apnea/hypopnea events (EVE.edf)
     *   UsageEvents-TherapyStatusEvents  — TherapyStart/Stop, MaskOn/Off
     *   SystemActivityEvents-FrequentActivityEvents — PressureStart/Stop,
     *     CooldownStarted, StandbyStarted, etc.  PressureStart signals that
     *     pressure has begun ramping — used to gate BRP/PLD recording.
     *   _SNC — Summary spool update counter.  Pushes ValueChange when the
     *     AS11 writes new Summary data (nor:1:/Summary.bin).  Used to detect
     *     when Summary spool is fresh after TherapyStop without polling.
     *   _ZLE — Zero Leak Estimate.  Boolean state variable that gates
     *     BRP/PLD data writing on the AS11.  ValueChange notifications
     *     signal when the AS11 starts/stops accepting valid flow data,
     *     providing a more accurate EDF start alignment than MaskOn.
     * Rationale: https://github.com/ilyakruchinin/SomnoTrace/issues/20#issuecomment-4975037843 */
    rpc = heap_caps_malloc(700, MALLOC_CAP_SPIRAM);
    if (!rpc) rpc = malloc(700);
    if (!rpc) {
        ESP_LOGW(TAG, "reconnect: SubscribeEvent skipped, out of memory");
        goto after_subscribe;
    }
    snprintf(rpc, 700,
             "{\"id\":13,\"jsonrpc\":\"1.0\",\"method\":\"SubscribeEvent\","
             "\"params\":{\"dataIds\":["
             "\"TherapyEvents-RespiratoryEvents\","
             "\"UsageEvents-TherapyStatusEvents\","
             "\"SystemActivityEvents-FrequentActivityEvents\","
             "\"_SNC\","
             "\"_ZLE\""
             "]}}");
    if (rpc_channel_lock(15000)) {
        clear_response();
        if (send_rpc_encrypted(rpc) != ESP_OK) {
            ESP_LOGW(TAG, "reconnect: SubscribeEvent send failed");
        } else {
            resp = wait_response(10000);
            if (resp) {
                ESP_LOGI(TAG, "reconnect: SubscribeEvent response received");
                cJSON_Delete(resp);
            } else {
                ESP_LOGW(TAG, "reconnect: SubscribeEvent timeout (non-fatal)");
            }
        }
        rpc_channel_unlock();
    } else {
        ESP_LOGW(TAG, "reconnect: RPC channel busy, SubscribeEvent skipped");
    }
    free(rpc);
after_subscribe:

    /* ---- Query AS11 clock before starting stream ----
     * GetDateTime must be sent before StartStream because active streaming
     * congests BLE ACL buffers, making subsequent RPCs fail. */
    {
        int64_t as11_ms = 0;
        if (as11_ble_get_datetime(&as11_ms) == ESP_OK) {
            s_as11_clock_ms = as11_ms;
            s_as11_clock_capture_ntp_ms = (int64_t)time(NULL) * 1000;
            ESP_LOGI(TAG, "reconnect: AS11 clock captured: %lld ms (NTP=%lld)",
                     (long long)as11_ms, (long long)s_as11_clock_capture_ntp_ms);
        } else {
            ESP_LOGW(TAG, "reconnect: GetDateTime failed — clock_drift_ms will be unavailable");
        }
    }

    /* Phase D (experimental, opt-in): raise the supervision timeout
     * before the stream starts, while ACL buffers are free. */
    apply_exp_supv_timeout();

    /* ---- Start data stream (encrypted) ----
     * Uses short tags matching STREAM_EDF_ALIASES in as11_rpc_vars.py:
     *   BRP: _RFL, _MKP  (40ms natural interval = 25 Hz)
     *   PLD: 12 channels (2000ms natural interval = 0.5 Hz)
     *   SA2: _HRT, _SAO  (1000ms natural interval = 1 Hz)
     * AS11 normalizes short tags to long names in StreamData notifications.
     * Unsupported signals return valid:false in the StartStream response.
     * sampleIntervalMs:40 applies to all; reportIntervalMs:200 gives 5 reports/s. */
    rpc = heap_caps_malloc(600, MALLOC_CAP_SPIRAM);
    if (!rpc) rpc = malloc(600);
    if (!rpc) {
        ESP_LOGW(TAG, "reconnect: StartStream skipped, out of memory");
        goto after_start_stream;
    }
    snprintf(rpc, 600,
             "{\"id\":14,\"jsonrpc\":\"1.0\",\"method\":\"StartStream\","
             "\"params\":{\"dataIds\":["
             "\"_RFL\",\"_MKP\","
             "\"_MKF\",\"_MKI\",\"_MKE\",\"_LKF\","
             "\"_RR2\",\"_TD2\",\"_MV2\",\"_TGT\",\"_IE2\","
             "\"_SNI\",\"_FFL\",\"_INT\","
             "\"_HRT\",\"_SAO\""
             "],\"sampleIntervalMs\":40,\"reportIntervalMs\":200}}");
    if (rpc_channel_lock(15000)) {
        clear_response();
        if (send_rpc_encrypted(rpc) != ESP_OK) {
            ESP_LOGW(TAG, "reconnect: StartStream send failed");
        } else {
            resp = wait_response(10000);
            if (resp) {
                char *resp_str = cJSON_PrintUnformatted(resp);
                ESP_LOGI(TAG, "reconnect: StartStream response: %s",
                         resp_str ? resp_str : "(null)");
                if (resp_str) free(resp_str);
                cJSON_Delete(resp);
            } else {
                ESP_LOGW(TAG, "reconnect: StartStream timeout (non-fatal)");
            }
        }
        rpc_channel_unlock();
    } else {
        ESP_LOGW(TAG, "reconnect: RPC channel busy, StartStream skipped");
    }
    free(rpc);
after_start_stream:

    __atomic_add_fetch(&s_session_epoch, 1, __ATOMIC_SEQ_CST);
    set_state(AS11_STATUS_PAIRED);
    #if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    if (as11_ble_exp_coex_fix()) {
        /* Corrected call: esp_coex_status_bit_set(type, status).  The
         * historical call below passed the status enum as the type and a
         * bool as the status, which is an argument inversion (it set
         * status=1 on type=0x10).  Behind the experimental flag because
         * it changes radio arbitration behaviour, not just logging
         * (.ai/RECONNECT/PLAN.md Phase D). */
        esp_coex_status_bit_set(ESP_COEX_ST_TYPE_BLE,
                                ESP_COEX_BLE_ST_MESH_TRAFFIC);
    } else {
        /* Original (argument-inverted) call kept verbatim for
         * bug-compatibility while the fix soaks. */
        esp_coex_status_bit_set(ESP_COEX_BLE_ST_MESH_TRAFFIC, true);
    }
    #endif
    ESP_LOGI(TAG, "reconnect: connected to %s, session established, streams started", addr_str);

    vTaskDelete(NULL);
}

/* ── Bounded disconnect-context line (Phase A) ─────────────────────────
 * Emitted by auto_reconnect_task (own task context) when the previous
 * disconnect was a supervision timeout.  Everything here is a local
 * query — no BLE traffic, no blocking HCI calls — because the goal is a
 * snapshot of the ESP's own state at the moment the link died: was Wi-Fi
 * up, was the O2 link alive, was an upload or post-therapy pull in
 * flight, how was the heap.  This is what discriminates "AS11 stopped
 * transmitting" from "our radio starved" the next time reason 520
 * appears. */
static void log_disconnect_context(void)
{
    netprov_link_t link;
    netprov_get_link(&link);

    uint8_t channel = 0;
    int8_t ap_rssi = 0;
    wifi_ap_record_t ap = {0};
    if (link.up && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        channel = ap.primary;
        ap_rssi = ap.rssi;
    }

    int64_t sess_start = 0;
    bool have_sess = session_writer_active_start_epoch_ms(&sess_start);
    int64_t sess_age_s = have_sess ?
        (((int64_t)time(NULL) * 1000 - sess_start) / 1000) : -1;

    /* Reception forensics — see the counters' declaration.  silence_ms is
     * how long the link had already been quiet when the supervision timeout
     * fired; a value near the 3 s timeout means the AS11 stopped sending
     * first, while a small value with steady gaps points at radio contention. */
    int64_t now_us = esp_timer_get_time();
    int64_t silence_ms = s_notif_last_rx_us ? (now_us - s_notif_last_rx_us) / 1000 : -1;
    int64_t last_gap_ms = s_notif_last_gap_us ? (now_us - s_notif_last_gap_us) / 1000 : -1;

    ESP_LOGW(TAG, "520-context: wifi=%s rssi=%d ch=%u o2=%s upload=%d "
             "post_active=%d session=%s (age=%llds) heap_int=%u heap_min=%u "
             "rx=%u gaps=%u silence_ms=%lld last_gap_ms=%lld "
             "q_hwm=%u q_drop=%u q_drop_bytes=%u alloc_fail=%u",
             link.up ? "up" : "down",
             link.up ? (int)ap_rssi : link.rssi,
             channel,
             oximeter_get_status(),
             upload_sched_uploading() ? 1 : 0,
             session_writer_post_active() ? 1 : 0,
             have_sess ? "active" : "none",
             (long long)sess_age_s,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)s_notif_rx_total,
             (unsigned)s_notif_gap_events,
             (long long)silence_ms,
             (long long)last_gap_ms,
             (unsigned)s_notif_hwm,
             (unsigned)s_notif_dropped,
             (unsigned)s_notif_dropped_bytes,
             (unsigned)s_notif_alloc_fail);
}

/* ------------------------------------------------------------------
 *  Auto-reconnect wrapper — launched from BLE_GAP_EVENT_DISCONNECT
 *  when an unexpected disconnect occurs while paired.  Adds an
 *  initial delay (AS11 needs time to restart advertising) then
 *  delegates to reconnect_task which has its own retry loop.
 * ------------------------------------------------------------------ */
static void auto_reconnect_task(void *arg)
{
    (void)arg;

    /* Bounded context line for a supervision-timeout disconnect — log
     * before the countdown so the timestamp stays close to the event. */
    if (s_supv_tmo_context_pending) {
        s_supv_tmo_context_pending = false;
        log_disconnect_context();
    }

    /* Wait a few seconds before attempting to reconnect.  The AS11
     * typically needs 2-5 seconds after a disconnect before it
     * advertises again.  We check every second whether the pairing
     * is still valid or the user has disconnected, and abort if so. */
    for (int i = 3; i > 0; i--) {
        ESP_LOGI(TAG, "auto-reconnect in %ds...", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_pair_cache.valid || s_manual_disconnect) {
            ESP_LOGI(TAG, "auto-reconnect: aborted during wait "
                         "(pairing=%d manual=%d)",
                     s_pair_cache.valid, s_manual_disconnect);
            vTaskDelete(NULL);
            return;
        }
    }

    /* reconnect_task handles the full connect + encrypted session +
     * stream setup.  It calls vTaskDelete(NULL) internally, so this
     * task is cleaned up when reconnect_task finishes. */
    reconnect_task(arg);
}


static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr type failed rc=%d", rc);
        return;
    }
    ble_att_set_preferred_mtu(247);
    s_host_ready = true;
    ESP_LOGI(TAG, "NimBLE host ready");
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset; reason=%d", reason);
    s_host_ready = false;
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

esp_err_t as11_ble_init(void)
{
    s_state_mtx  = xSemaphoreCreateMutex();
    s_cmd_mtx    = xSemaphoreCreateMutex();
    s_op_sem     = xSemaphoreCreateBinary();
    s_connect_sem = xSemaphoreCreateBinary();
    s_resp_sem   = xSemaphoreCreateBinary();
    s_scan_done  = xSemaphoreCreateBinary();
    if (!s_state_mtx || !s_cmd_mtx || !s_op_sem || !s_connect_sem || !s_resp_sem || !s_scan_done) {
        return ESP_ERR_NO_MEM;
    }

    /* Allocate BLE RX accumulation buffer in PSRAM to avoid exhausting
     * internal RAM.  The SettingProfiles Get RPC response can exceed 4 KB
     * after AES-CBC encryption and FIG framing; 16 KB gives ample headroom.
     * The payload and decrypted buffers in handle_notify() are allocated
     * lazily on first use (also PSRAM). */
    s_rx_buf = heap_caps_malloc(RX_BUF_MAX, MALLOC_CAP_SPIRAM);
    if (!s_rx_buf) {
        ESP_LOGE(TAG, "init: failed to allocate s_rx_buf in PSRAM");
        return ESP_ERR_NO_MEM;
    }
    s_server_pk = heap_caps_calloc(1, SERVER_PK_MAX, MALLOC_CAP_SPIRAM);
    s_salt = heap_caps_calloc(1, SALT_MAX, MALLOC_CAP_SPIRAM);
    if (!s_server_pk || !s_salt) {
        ESP_LOGE(TAG, "init: failed to allocate SRP buffers in PSRAM");
        return ESP_ERR_NO_MEM;
    }

    /* Create notification queue and processing task */
    s_notif_queue = xQueueCreate(NOTIF_QUEUE_LEN, sizeof(notif_item_t));
    if (!s_notif_queue) return ESP_ERR_NO_MEM;
    s_notif_task = psram_task_create(notif_proc_task, "notif_proc", 8192, NULL, 10, tskNO_AFFINITY, NULL, NULL);
    ESP_LOGI(TAG, "notification processing task started");

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", ret);
        return ret;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    /* SomnoBridge needs LE Secure Connections + MITM + bonding (§5.2).
     * The AS11 (application-layer SRP) and the O2 ring never request SMP
     * security, so their links stay unencrypted exactly as before. */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    /* Without a registered bond store ble_gap_security_initiate() fails with
     * BLE_HS_EINVAL, so SomnoBridge pairing could never start.  Declared here
     * because the store-config header is not on the public include path. */
    extern void ble_store_config_init(void);
    ble_store_config_init();

    /* Quiet NimBLE's per-byte HCI hex dumps — warnings/errors only. */
    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    esp_log_level_set("NimBLE_HCI", ESP_LOG_WARN);
    
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE initialised");

    /* Load saved pairing credentials into RAM cache so that HTTP /api/status
     * can check paired state without touching NVS on every request. */
    pair_cache_load();

    /* If we have saved pairing credentials, auto-reconnect to the AS11
     * in the background. The reconnect task waits for host sync, loads
     * the address from NVS, connects, discovers GATT services, and
     * enables notifications — no SRP key exchange needed. */
    if (as11_ble_is_paired()) {
        ESP_LOGI(TAG, "saved pairing found, auto-reconnecting...");
        psram_task_create(reconnect_task, "as11_reconn", 8192, NULL, 5, tskNO_AFFINITY, NULL, NULL);
    }

    return ESP_OK;
}

bool as11_ble_is_host_ready(void)
{
    return s_host_ready;
}

uint8_t as11_ble_get_own_addr_type(void)
{
    return s_own_addr_type;
}

esp_err_t as11_ble_scan(int timeout_sec)
{
    if (!s_host_ready) return ESP_ERR_INVALID_STATE;
    s_scan_count = 0;
    set_state(AS11_STATUS_SCANNING);

    struct ble_gap_disc_params dp = {
        .itvl = 96,            /* 60 ms */
        .window = 96,          /* 60 ms (100% duty cycle for best coexistence) */
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,          /* active scan to collect names */
        .filter_duplicates = 0,  /* need both ADV_IND (name) and SCAN_RSP (UUID) */
    };
    /* drain stale completion signal */
    xSemaphoreTake(s_scan_done, 0);

    int rc = ble_gap_disc(s_own_addr_type, timeout_sec * 1000, &dp, gap_event, NULL);
    if (rc != 0) {
        set_error("scan start failed");
        return ESP_FAIL;
    }
    xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(timeout_sec * 1000 + 2000));
    set_state(AS11_STATUS_IDLE);
    return ESP_OK;
}

bool as11_ble_scan_lookup(const char *addr_str, uint8_t *addr_type)
{
    bool bridge = false;
    return as11_ble_scan_lookup_ex(addr_str, addr_type, &bridge);
}

bool as11_ble_scan_lookup_ex(const char *addr_str, uint8_t *addr_type,
                             bool *is_bridge)
{
    if (!addr_str) return false;
    char a[18];
    for (int i = 0; i < s_scan_count; i++) {
        addr_to_str(&s_scan[i].addr, a);
        if (strcasecmp(a, addr_str) == 0) {
            if (addr_type) *addr_type = s_scan[i].addr.type;
            if (is_bridge) *is_bridge = s_scan[i].bridge;
            return true;
        }
    }
    return false;
}

cJSON *as11_ble_get_scan_results(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_scan_count; i++) {
        char addr_str[18];
        addr_to_str(&s_scan[i].addr, addr_str);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "addr", addr_str);
        cJSON_AddStringToObject(o, "name", s_scan[i].name);
        cJSON_AddNumberToObject(o, "rssi", s_scan[i].rssi);
        cJSON_AddStringToObject(o, "type", s_scan[i].bridge ? "somnobridge" : "as11");
        cJSON_AddNumberToObject(o, "addr_type", s_scan[i].addr.type);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

esp_err_t as11_ble_start_pair(const char *addr_str)
{
    if (!s_host_ready) return ESP_ERR_INVALID_STATE;
    if (!addr_str) return ESP_ERR_INVALID_ARG;

    /* resolve address type/bytes from the scan cache if possible */
    bool found = false;
    char tmp[18];
    for (int i = 0; i < s_scan_count; i++) {
        addr_to_str(&s_scan[i].addr, tmp);
        if (strcasecmp(tmp, addr_str) == 0) {
            s_target_addr = s_scan[i].addr;
            strlcpy(s_target_name, s_scan[i].name, sizeof(s_target_name));
            found = true;
            break;
        }
    }
    if (!found) {
        if (!str_to_addr(addr_str, &s_target_addr)) return ESP_ERR_INVALID_ARG;
        strlcpy(s_target_name, "AirSense 11", sizeof(s_target_name));
    }

    s_error[0] = '\0';
    TaskHandle_t h = psram_task_create(pair_task, "as11_pair", 8192, NULL, 5, tskNO_AFFINITY, NULL, NULL);
    if (!h) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t as11_ble_confirm_pair(const char *passkey)
{
    if (!passkey || !*passkey) return ESP_ERR_INVALID_ARG;
    strlcpy(s_passkey, passkey, sizeof(s_passkey));
    TaskHandle_t h = psram_task_create(confirm_task, "as11_confirm", 8192, NULL, 5, tskNO_AFFINITY, NULL, NULL);
    if (!h) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

const char *as11_ble_get_status(void)
{
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    const char *s = s_state;
    xSemaphoreGive(s_state_mtx);
    return s;
}

const char *as11_ble_get_error(void)
{
    return s_error;
}

bool as11_ble_is_paired(void)
{
    return s_pair_cache.valid;
}

uint32_t as11_ble_get_session_epoch(void)
{
    return __atomic_load_n(&s_session_epoch, __ATOMIC_SEQ_CST);
}

bool as11_ble_wait_for_reconnect(uint32_t prev_epoch, uint32_t timeout_ms)
{
    const uint32_t step_ms = 250;
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (!s_pair_cache.valid || s_manual_disconnect) {
            return false;
        }
        uint32_t cur = __atomic_load_n(&s_session_epoch, __ATOMIC_SEQ_CST);
        if (s_session_encrypted && cur != prev_epoch &&
            strcmp(s_state, AS11_STATUS_PAIRED) == 0) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        waited += step_ms;
    }
    return false;
}

cJSON *as11_ble_get_paired_info(void)
{
    if (!s_pair_cache.valid) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "addr", s_pair_cache.addr);
    cJSON_AddStringToObject(o, "name", s_pair_cache.name);
    cJSON_AddStringToObject(o, "clientId", s_pair_cache.client_id);
    return o;
}

static esp_err_t do_forget_nvs(void *arg)
{
    (void)arg;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_erase_all(h);
    e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t as11_ble_forget(void)
{
    /* Delegate the NVS erase so callers on a PSRAM stack (httpd forget handler)
     * are safe; the BLE teardown below stays on the caller (no flash). */
    esp_err_t e = nvs_writer_run(do_forget_nvs, NULL);
    if (e == ESP_OK) {
        memset(&s_pair_cache, 0, sizeof(s_pair_cache));
    }
    s_manual_disconnect = true;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    set_state(AS11_STATUS_IDLE);
    return e;
}

esp_err_t as11_ble_disconnect(void)
{
    s_manual_disconnect = true;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(TAG, "disconnecting BLE (conn_handle=%d)", s_conn_handle);
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    set_state(AS11_STATUS_IDLE);
    return ESP_OK;
}

/* Stop the AS11 data stream.
 * With 64 ACL buffers + offloaded notification processing, outgoing RPCs
 * can be sent while stream notifications are active. */
esp_err_t as11_ble_stop_stream(void)
{
    /* No-op for now — the stream continues between sessions and is
     * restarted on next reconnect. Stopping via CCCD would block RPC
     * responses (which arrive as notifications on the same characteristic). */
    return ESP_OK;
}

/* Compute clock drift from AS11 clock captured before stream start.
 * Returns ESP_OK and stores drift in *out_drift_ms.
 * drift = NTP_time - AS11_time (positive = AS11 clock is behind). */
esp_err_t as11_ble_get_clock_drift(int64_t *out_drift_ms)
{
    if (!out_drift_ms) return ESP_ERR_INVALID_ARG;
    if (s_as11_clock_ms == 0 || s_as11_clock_capture_ntp_ms == 0) {
        ESP_LOGW(TAG, "get_clock_drift: no AS11 clock capture available");
        return ESP_ERR_INVALID_STATE;
    }
    *out_drift_ms = s_as11_clock_capture_ntp_ms - s_as11_clock_ms;
    ESP_LOGI(TAG, "get_clock_drift: drift=%lld ms (NTP=%lld AS11=%lld)",
             (long long)*out_drift_ms,
             (long long)s_as11_clock_capture_ntp_ms,
             (long long)s_as11_clock_ms);
    return ESP_OK;
}

/* Shared implementation for as11_ble_get_datetime / _ex.  Takes the RPC
 * channel lock for its send+wait cycle (safe to call from any task —
 * including reconnect_task, which does NOT hold the lock at that point).
 * out_meas_ms receives the NTP wall-clock midpoint of the round trip:
 * the instant the returned AS11 reading best corresponds to. */
static esp_err_t get_datetime_impl(int64_t *out_epoch_ms, int64_t *out_meas_ms)
{
    if (!out_epoch_ms) return ESP_ERR_INVALID_ARG;
    if (!s_session_encrypted) {
        ESP_LOGW(TAG, "get_datetime: no encrypted session");
        return ESP_ERR_INVALID_STATE;
    }

    /* Midpoint of the round trip: the AS11 timestamps its reply at some
     * point between our send and the receive; half the elapsed time is
     * the best single estimate of when the reading was taken.
     * gettimeofday (µs wall clock) rather than time(NULL) so the
     * midpoint carries ms resolution — a 1 s quantisation would be the
     * dominant drift error on a ~100-300 ms RPC round trip. */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t t_send_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;

    const char *rpc = "{\"id\":20,\"jsonrpc\":\"1.0\",\"method\":\"GetDateTime\"}";
    if (!rpc_channel_lock(8000)) {
        ESP_LOGW(TAG, "get_datetime: RPC channel busy");
        return ESP_ERR_TIMEOUT;
    }
    clear_response();
    esp_err_t send_err = send_rpc_encrypted(rpc);
    if (send_err != ESP_OK) {
        rpc_channel_unlock();
        ESP_LOGW(TAG, "get_datetime: send failed");
        return ESP_FAIL;
    }

    cJSON *resp = wait_response(5000);
    rpc_channel_unlock();
    gettimeofday(&tv, NULL);
    int64_t t_done_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    if (out_meas_ms) {
        *out_meas_ms = t_send_ms + (t_done_ms - t_send_ms) / 2;
    }
    if (!resp) {
        ESP_LOGW(TAG, "get_datetime: timeout");
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    if (result) {
        cJSON *dt = cJSON_GetObjectItem(result, "dateTime");
        if (dt && cJSON_IsString(dt)) {
            /* Parse ISO 8601: "2026-06-25T15:08:00.000Z" */
            struct tm tm = {0};
            int ms = 0;
            int parsed = sscanf(dt->valuestring, "%d-%d-%dT%d:%d:%d.%dZ",
                                &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                                &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &ms);
            if (parsed >= 6) {
                tm.tm_year -= 1900;
                tm.tm_mon -= 1;
                /* Manual UTC epoch calculation (timegm not available in newlib) */
                /* Days from 1970-01-01 to start of given year */
                int year = tm.tm_year + 1900;
                int days = (year - 1970) * 365L;
                /* Add leap years */
                for (int y = 1970; y < year; y++) {
                    if ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0))
                        days++;
                }
                /* Days in months before the given month */
                static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
                for (int m = 0; m < tm.tm_mon; m++) {
                    days += mdays[m];
                    if (m == 1 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)))
                        days++;
                }
                days += tm.tm_mday - 1;
                int64_t epoch = (int64_t)days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
                *out_epoch_ms = epoch * 1000 + ms;
                ret = ESP_OK;
                ESP_LOGI(TAG, "get_datetime: AS11 clock = %s (%lld ms)",
                         dt->valuestring, (long long)*out_epoch_ms);
            }
        }
    }

    if (ret != ESP_OK) {
        char *s = cJSON_Print(resp);
        ESP_LOGW(TAG, "get_datetime: parse failed: %s", s ? s : "?");
        if (s) free(s);
    }

    cJSON_Delete(resp);
    return ret;
}

esp_err_t as11_ble_get_datetime(int64_t *out_epoch_ms)
{
    return get_datetime_impl(out_epoch_ms, NULL);
}

esp_err_t as11_ble_get_datetime_ex(int64_t *out_epoch_ms, int64_t *out_meas_ms)
{
    return get_datetime_impl(out_epoch_ms, out_meas_ms);
}

/* Query AS11 firmware identity via GetVersion and format a compact
 * one-line summary into buf.  Non-fatal telemetry for incident
 * correlation (.ai/RECONNECT/PLAN.md Phase A). */
esp_err_t as11_ble_get_version(char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';
    if (!s_session_encrypted) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *rpc = "{\"id\":21,\"jsonrpc\":\"2.0\",\"method\":\"GetVersion\"}";
    if (!rpc_channel_lock(8000)) {
        return ESP_ERR_TIMEOUT;
    }
    clear_response();
    esp_err_t send_err = send_rpc_encrypted(rpc);
    if (send_err != ESP_OK) {
        rpc_channel_unlock();
        return ESP_FAIL;
    }
    cJSON *resp = wait_response(5000);
    rpc_channel_unlock();
    if (!resp) return ESP_ERR_TIMEOUT;

    esp_err_t ret = ESP_FAIL;
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    if (cJSON_IsObject(result)) {
        /* Compact summary: one ApplicationIdentifier per module. */
        size_t used = 0;
        buf[0] = '\0';
        const char *modules[] = { "FlowGenerator", "BluetoothModule",
                                  "CellularModule", "AlarmModule" };
        for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); i++) {
            cJSON *mod = cJSON_GetObjectItem(result, modules[i]);
            if (!cJSON_IsObject(mod)) continue;
            cJSON *sw = cJSON_GetObjectItem(mod, "IdentificationProfiles");
            sw = sw ? cJSON_GetObjectItem(sw, "Software") : NULL;
            cJSON *app = sw ? cJSON_GetObjectItem(sw, "ApplicationIdentifier") : NULL;
            if (app && cJSON_IsString(app)) {
                int n = snprintf(buf + used, buflen - used, "%s%s=",
                                 used ? " " : "", modules[i]);
                if (n < 0 || (size_t)n >= buflen - used) break;
                used += n;
                /* Append the identifier value (bounded). */
                n = snprintf(buf + used, buflen - used, "%s",
                             app->valuestring);
                if (n < 0 || (size_t)n >= buflen - used) break;
                used += n;
            }
        }
        ret = (used > 0) ? ESP_OK : ESP_FAIL;
    }

    if (ret != ESP_OK) {
        char *s = cJSON_Print(resp);
        ESP_LOGW(TAG, "get_version: unexpected response: %s", s ? s : "?");
        if (s) free(s);
    }
    cJSON_Delete(resp);
    return ret;
}

/* ── Spool RPC ────────────────────────────────────────────────────────
 *
 * Post-therapy data collection.  The AS11 stores session summaries, event
 * logs, and other data in internal "spools" accessed via the StartSpool /
 * PullSpoolFragments RPC cycle.  SpoolFragment notifications arrive
 * asynchronously on the same BLE characteristic as StreamData and RPC
 * responses; handle_notify() intercepts them when s_spool_collector is set.
 *
 * The pull function runs in the post worker task (session_writer's
 * sw_post_task → post_therapy_collect), NOT in notif_proc_task — the
 * comment claiming same-task execution was stale.  SpoolFragment
 * notifications are processed by notif_proc_task, which routes them into
 * the collector and signals its semaphore; the puller waits on that
 * semaphore from its own task.  All arming callers hold s_cmd_mtx for
 * the whole pull, which is what makes the shared collector pointer safe.
 *
 * Multi-round pulls: if the device returns SPOOL_COMPLETE_MORE_DATA_PENDING,
 * the last fragment includes a nextSpoolAddress JSON object.  We loop with
 * that address for the next StartSpool call, appending data from each round.
 */

/* Fragment comparison for qsort (sort by seq ascending). */
static int frag_cmp(const void *a, const void *b)
{
    const spool_frag_t *fa = (const spool_frag_t *)a;
    const spool_frag_t *fb = (const spool_frag_t *)b;
    return fa->seq - fb->seq;
}

/* One round of StartSpool → PullSpoolFragments → collect → concatenate.
 * Returns ESP_OK and sets *round_data / *round_len.  If the status was
 * SPOOL_COMPLETE_MORE_DATA_PENDING, *next_addr_out is set to the
 * nextSpoolAddress JSON string (caller uses it for the next round).
 * Otherwise *next_addr_out is set to empty string. */
static esp_err_t spool_one_round(const char *spool_addr_json,
                                 uint8_t **round_data, size_t *round_len,
                                 char *next_addr_out, size_t next_addr_max)
{
    *round_data = NULL;
    *round_len = 0;
    next_addr_out[0] = '\0';

    /* Set up collector before sending RPCs so we don't miss fragments. */
    spool_collector_t coll = {0};
    coll.sem = xSemaphoreCreateBinary();
    if (!coll.sem) return ESP_ERR_NO_MEM;
    s_spool_collector = &coll;

    /* 1. StartSpool RPC */
    char rpc[512];
    snprintf(rpc, sizeof(rpc),
             "{\"id\":30,\"jsonrpc\":\"1.0\",\"method\":\"StartSpool\","
             "\"params\":{\"spoolAddress\":%s,\"maxSpoolSize\":100000}}",
             spool_addr_json);
    clear_response();
    if (send_rpc_encrypted(rpc) != ESP_OK) {
        ESP_LOGE(TAG, "spool: StartSpool send failed");
        vSemaphoreDelete(coll.sem);
        s_spool_collector = NULL;
        return ESP_FAIL;
    }

    cJSON *resp = wait_response(10000);
    if (!resp) {
        ESP_LOGE(TAG, "spool: StartSpool timeout");
        vSemaphoreDelete(coll.sem);
        s_spool_collector = NULL;
        return ESP_FAIL;
    }
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    cJSON *spool_id_j = result ? cJSON_GetObjectItem(result, "spoolId") : NULL;
    if (!spool_id_j || !cJSON_IsNumber(spool_id_j)) {
        char *s = cJSON_Print(resp);
        ESP_LOGE(TAG, "spool: StartSpool bad response: %s", s ? s : "?");
        if (s) free(s);
        cJSON_Delete(resp);
        vSemaphoreDelete(coll.sem);
        s_spool_collector = NULL;
        return ESP_FAIL;
    }
    int spool_id = spool_id_j->valueint;
    cJSON_Delete(resp);
    ESP_LOGI(TAG, "spool: StartSpool ok, spoolId=%d", spool_id);

    /* 2. PullSpoolFragments RPC — triggers SpoolFragment notifications */
    snprintf(rpc, sizeof(rpc),
             "{\"id\":31,\"jsonrpc\":\"1.0\",\"method\":\"PullSpoolFragments\","
             "\"params\":{\"spoolId\":%d,\"maxFragmentSize\":2808,\"maxNotifications\":0}}",
             spool_id);
    clear_response();
    if (send_rpc_encrypted(rpc) != ESP_OK) {
        ESP_LOGE(TAG, "spool: PullSpoolFragments send failed");
        vSemaphoreDelete(coll.sem);
        s_spool_collector = NULL;
        return ESP_FAIL;
    }

    /* The PullSpoolFragments response is an ack; fragments arrive as
     * separate SpoolFragment notifications.  Wait for the collector. */
    resp = wait_response(5000);
    if (resp) cJSON_Delete(resp);  /* ack response, not needed */

    /* 3. Wait for all fragments to arrive */
    if (xSemaphoreTake(coll.sem, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "spool: fragment collection timeout (%d frags received)",
                 coll.frag_count);
        /* Detach first: a SpoolFragment notification arriving late must not
         * append to a collector that is being torn down. */
        s_spool_collector = NULL;
        vSemaphoreDelete(coll.sem);
        /* Fragments already received were malloc'd by the notification
         * handler.  The success and out-of-memory paths free them; this one
         * did not, leaking up to SPOOL_MAX_FRAGS x maxFragmentSize per
         * timed-out round. */
        for (int i = 0; i < coll.frag_count; i++) {
            free(coll.frags[i].data);
        }
        /* Clear any stale RPC response state so subsequent RPCs
         * don't pick up a leftover response from the timed-out pull. */
        clear_response();
        return ESP_FAIL;
    }

    /* Clear collector before processing (new fragments would be lost) */
    s_spool_collector = NULL;
    vSemaphoreDelete(coll.sem);

    ESP_LOGI(TAG, "spool: collected %d fragments, status=%s",
             coll.frag_count, coll.status);

    /* Device refused the read (e.g. ERROR_DATA_UNAVAILABLE) — terminal
     * failure, do not treat whatever fragments arrived as valid data. */
    if (coll.error) {
        ESP_LOGE(TAG, "spool: device reported %s", coll.status);
        for (int i = 0; i < coll.frag_count; i++) free(coll.frags[i].data);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* 4. Sort fragments by seq and concatenate */
    if (coll.frag_count == 0) {
        return ESP_OK;  /* empty spool — valid (e.g. no events) */
    }

    qsort(coll.frags, coll.frag_count, sizeof(spool_frag_t), frag_cmp);

    /* Sequence-continuity check: the AS11 protocol documents fragment
     * sequence numbers starting at 0 and increasing without gaps
     * (rpc_protocol.md).  A gap means notifications were lost — the
     * reassembled payload would be silently corrupt, so fail the round
     * instead of saving it. */
    for (int i = 0; i < coll.frag_count; i++) {
        if (coll.frags[i].seq != i) {
            ESP_LOGE(TAG, "spool: fragment sequence discontinuity at %d "
                     "(expected seq %d, got %d) — discarding round",
                     i, i, coll.frags[i].seq);
            for (int j = 0; j < coll.frag_count; j++) free(coll.frags[j].data);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    size_t total = 0;
    for (int i = 0; i < coll.frag_count; i++) {
        total += coll.frags[i].len;
    }

    uint8_t *data = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!data) data = malloc(total);
    if (!data) {
        ESP_LOGE(TAG, "spool: malloc %u failed", (unsigned)total);
        for (int i = 0; i < coll.frag_count; i++) free(coll.frags[i].data);
        return ESP_ERR_NO_MEM;
    }

    size_t offset = 0;
    for (int i = 0; i < coll.frag_count; i++) {
        memcpy(data + offset, coll.frags[i].data, coll.frags[i].len);
        offset += coll.frags[i].len;
        free(coll.frags[i].data);
    }

    /* Integrity check: spoolHash is the SHA-256 of the concatenated raw
     * fragment data (rpc_protocol.md), sent on the terminal fragment.
     * Empty hash on a non-empty payload means the device did not send
     * one — warn but accept (older firmware may omit it); a present but
     * mismatching hash is corruption — fail the round. */
    if (total > 0 && coll.spool_hash_hex[0]) {
        uint8_t hash[32];
        memset(hash, 0, sizeof(hash));
        mbedtls_sha256(data, total, hash, 0);
        char hex[65];
        for (int i = 0; i < 32; i++) {
            snprintf(&hex[i * 2], 3, "%02x", hash[i]);
        }
        if (strcasecmp(hex, coll.spool_hash_hex) != 0) {
            ESP_LOGE(TAG, "spool: spoolHash mismatch (calc %s... vs dev %.8s...) "
                     "— discarding round", hex, coll.spool_hash_hex);
            free(data);
            return ESP_ERR_INVALID_RESPONSE;
        }
        ESP_LOGD(TAG, "spool: spoolHash verified (%s...)", hex);
    } else if (total > 0) {
        ESP_LOGW(TAG, "spool: no spoolHash in terminal fragment — "
                 "integrity unverified");
    }

    *round_data = data;
    *round_len = total;

    /* Check for multi-round continuation */
    if (strcmp(coll.status, "SPOOL_COMPLETE_MORE_DATA_PENDING") == 0 &&
        coll.next_addr_json[0]) {
        strlcpy(next_addr_out, coll.next_addr_json, next_addr_max);
    }

    return ESP_OK;
}

/* Shared pull implementation.  Holds the RPC channel lock for the whole
 * multi-round pull so no other RPC user (passthrough, get_values, a
 * concurrent post-therapy pull) can interleave requests on the shared
 * response channel or overwrite s_spool_collector mid-pull.
 *
 * max_bytes bounds the accumulated payload: if the next round would
 * push the total past it, the pull fails with ESP_ERR_INVALID_SIZE
 * (caller decides whether to skip or retry).  SIZE_MAX disables the cap.
 *
 * NOTE: spool_one_round() is called with the lock already held — it is
 * internal and does no locking of its own. */
static esp_err_t spool_pull_locked(const char *spool_type, const char *from_dt,
                                   uint8_t **out_data, size_t *out_len,
                                   size_t max_bytes)
{
    if (!spool_type || !from_dt || !out_data || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_session_encrypted) {
        ESP_LOGW(TAG, "spool_pull: no encrypted session");
        return ESP_ERR_INVALID_STATE;
    }

    *out_data = NULL;
    *out_len = 0;

    if (!rpc_channel_lock(15000)) {
        ESP_LOGW(TAG, "spool_pull: RPC channel busy (%s)", spool_type);
        return ESP_ERR_TIMEOUT;
    }

    /* Build initial spool address JSON:
     * {"<spool_type>":{"fromDateTime":"<from_dt>"}} */
    char addr_json[256];
    snprintf(addr_json, sizeof(addr_json),
             "{\"%s\":{\"fromDateTime\":\"%s\"}}",
             spool_type, from_dt);

    /* Accumulate data across multiple rounds */
    uint8_t *all_data = NULL;
    size_t all_len = 0;
    int round = 0;
    esp_err_t status = ESP_OK;

    while (addr_json[0]) {
        round++;
        ESP_LOGI(TAG, "spool_pull: round %d for %s", round, spool_type);

        uint8_t *round_data = NULL;
        size_t round_len = 0;
        char next_addr[256] = {0};

        esp_err_t ret = spool_one_round(addr_json, &round_data, &round_len,
                                        next_addr, sizeof(next_addr));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "spool_pull: round %d failed", round);
            status = ret;
            free(round_data);
            break;
        }

        if (round_data && round_len > 0) {
            if (max_bytes != SIZE_MAX && all_len + round_len > max_bytes) {
                ESP_LOGW(TAG, "spool_pull: %s exceeds %u-byte cap "
                         "(%u+%u) — aborting pull",
                         spool_type, (unsigned)max_bytes,
                         (unsigned)all_len, (unsigned)round_len);
                free(round_data);
                status = ESP_ERR_INVALID_SIZE;
                break;
            }
            /* Append to accumulated data */
            uint8_t *new_data = realloc(all_data, all_len + round_len);
            if (!new_data) {
                ESP_LOGE(TAG, "spool_pull: realloc %u failed",
                         (unsigned)(all_len + round_len));
                free(all_data);
                free(round_data);
                all_data = NULL;
                status = ESP_ERR_NO_MEM;
                break;
            }
            memcpy(new_data + all_len, round_data, round_len);
            all_data = new_data;
            all_len += round_len;
        }
        free(round_data);

        /* Continue with nextSpoolAddress or stop */
        if (next_addr[0]) {
            strlcpy(addr_json, next_addr, sizeof(addr_json));
        } else {
            break;  /* SPOOL_COMPLETE — done */
        }

        if (round >= 10) {
            ESP_LOGW(TAG, "spool_pull: too many rounds (%d), stopping", round);
            break;
        }
    }

    rpc_channel_unlock();

    if (status != ESP_OK) {
        free(all_data);
        return status;
    }

    *out_data = all_data;
    *out_len = all_len;
    ESP_LOGI(TAG, "spool_pull: %s done, %u bytes in %d round(s)",
             spool_type, (unsigned)all_len, round);
    return ESP_OK;
}

/* Default cap for unbounded pulls: large enough for any real Summary /
 * events spool (tens of KB), small enough that a runaway multi-round
 * pull cannot exhaust the heap. */
#define SPOOL_PULL_MAX_BYTES  (1u << 20)  /* 1 MiB */

esp_err_t as11_ble_spool_pull(const char *spool_type, const char *from_dt,
                              uint8_t **out_data, size_t *out_len)
{
    return spool_pull_locked(spool_type, from_dt, out_data, out_len,
                             SPOOL_PULL_MAX_BYTES);
}

esp_err_t as11_ble_spool_pull_bounded(const char *spool_type, const char *from_dt,
                                      uint8_t **out_data, size_t *out_len,
                                      size_t max_bytes)
{
    return spool_pull_locked(spool_type, from_dt, out_data, out_len, max_bytes);
}

cJSON *as11_ble_get_values(const char *const *keys, int n_keys)
{
    if (!keys || n_keys <= 0) return NULL;
    if (!s_session_encrypted) {
        ESP_LOGW(TAG, "get_values: no encrypted session");
        return NULL;
    }

    /* Build JSON params array: ["key1","key2",...] */
    /* Worst case: each key is ~40 chars, plus quotes and comma */
    size_t buf_size = n_keys * 48 + 64;
    char *params = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!params) params = malloc(buf_size);
    if (!params) return NULL;

    size_t pos = 0;
    pos += snprintf(params + pos, buf_size - pos, "[");
    for (int i = 0; i < n_keys; i++) {
        if (i > 0) pos += snprintf(params + pos, buf_size - pos, ",");
        pos += snprintf(params + pos, buf_size - pos, "\"%s\"", keys[i]);
    }
    pos += snprintf(params + pos, buf_size - pos, "]");

    /* Build full RPC */
    char rpc[512];
    snprintf(rpc, sizeof(rpc),
             "{\"id\":40,\"jsonrpc\":\"1.0\",\"method\":\"Get\",\"params\":%s}",
             params);
    free(params);

    if (!rpc_channel_lock(15000)) {
        ESP_LOGW(TAG, "get_values: RPC channel busy");
        return NULL;
    }
    clear_response();
    if (send_rpc_encrypted(rpc) != ESP_OK) {
        ESP_LOGW(TAG, "get_values: send failed");
        rpc_channel_unlock();
        return NULL;
    }

    cJSON *resp = wait_response(10000);
    rpc_channel_unlock();
    if (!resp) {
        ESP_LOGW(TAG, "get_values: timeout");
        return NULL;
    }

    cJSON *err = cJSON_GetObjectItem(resp, "error");
    if (err) {
        char *s = cJSON_Print(err);
        ESP_LOGW(TAG, "get_values: RPC error: %s", s ? s : "?");
        if (s) free(s);
        cJSON_Delete(resp);
        return NULL;
    }

    /* Return the result object (caller frees) */
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    if (result) {
        result = cJSON_DetachItemFromObject(resp, "result");
    }
    cJSON_Delete(resp);
    return result;
}

esp_err_t as11_ble_stop_therapy(void)
{
    if (!s_session_encrypted) {
        ESP_LOGW(TAG, "stop_therapy: no encrypted session");
        return ESP_ERR_INVALID_STATE;
    }

    /* Serialize on the shared RPC channel: clear_response()/wait_response()
     * own s_resp_json and must not interleave with other callers. */
    if (xSemaphoreTake(s_cmd_mtx, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGW(TAG, "stop_therapy: BLE command bus busy");
        return ESP_ERR_TIMEOUT;
    }

    const char *rpc = "{\"id\":50,\"jsonrpc\":\"1.0\",\"method\":\"EnterStandby\"}";
    clear_response();
    if (send_rpc_encrypted(rpc) != ESP_OK) {
        ESP_LOGW(TAG, "stop_therapy: send failed");
        xSemaphoreGive(s_cmd_mtx);
        return ESP_FAIL;
    }

    cJSON *resp = wait_response(10000);
    if (!resp) {
        ESP_LOGW(TAG, "stop_therapy: timeout");
        xSemaphoreGive(s_cmd_mtx);
        return ESP_ERR_TIMEOUT;
    }

    cJSON *err = cJSON_GetObjectItem(resp, "error");
    if (err) {
        char *s = cJSON_Print(err);
        ESP_LOGW(TAG, "stop_therapy: RPC error: %s", s ? s : "?");
        if (s) free(s);
        cJSON_Delete(resp);
        xSemaphoreGive(s_cmd_mtx);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "stop_therapy: EnterStandby accepted");
    cJSON_Delete(resp);
    xSemaphoreGive(s_cmd_mtx);
    return ESP_OK;
}

esp_err_t as11_ble_start_therapy(void)
{
    if (!s_session_encrypted) {
        ESP_LOGW(TAG, "start_therapy: no encrypted session");
        return ESP_ERR_INVALID_STATE;
    }

    /* Serialize on the shared RPC channel: clear_response()/wait_response()
     * own s_resp_json and must not interleave with other callers. */
    if (xSemaphoreTake(s_cmd_mtx, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGW(TAG, "start_therapy: BLE command bus busy");
        return ESP_ERR_TIMEOUT;
    }

    const char *rpc = "{\"id\":51,\"jsonrpc\":\"1.0\",\"method\":\"EnterTherapy\"}";
    clear_response();
    if (send_rpc_encrypted(rpc) != ESP_OK) {
        ESP_LOGW(TAG, "start_therapy: send failed");
        xSemaphoreGive(s_cmd_mtx);
        return ESP_FAIL;
    }

    cJSON *resp = wait_response(10000);
    if (!resp) {
        ESP_LOGW(TAG, "start_therapy: timeout");
        xSemaphoreGive(s_cmd_mtx);
        return ESP_ERR_TIMEOUT;
    }

    cJSON *err = cJSON_GetObjectItem(resp, "error");
    if (err) {
        char *s = cJSON_Print(err);
        ESP_LOGW(TAG, "start_therapy: RPC error: %s", s ? s : "?");
        if (s) free(s);
        cJSON_Delete(resp);
        xSemaphoreGive(s_cmd_mtx);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "start_therapy: EnterTherapy accepted");
    cJSON_Delete(resp);
    xSemaphoreGive(s_cmd_mtx);
    return ESP_OK;
}

esp_err_t as11_ble_passthrough_rpc(const char *json_in, char **json_out, uint32_t timeout_ms)
{
    if (!json_in || !*json_in || !json_out) return ESP_ERR_INVALID_ARG;
    *json_out = NULL;

    if (!s_session_encrypted) {
        ESP_LOGW(TAG, "passthrough_rpc: no active encrypted BLE session");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_cmd_mtx, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "passthrough_rpc: BLE command bus busy");
        return ESP_ERR_TIMEOUT;
    }

    /* PullSpoolFragments special case: the direct response is only an
     * ack — the actual payload arrives as asynchronous SpoolFragment
     * notifications.  Arm a local collector so those notifications are
     * captured and returned to the bridge caller (this is what makes
     * diagnostic spools retrievable over HTTP, see docs/rpc-bridge.md
     * and .ai/RECONNECT/PLAN.md Phase C4).  The command mutex is held
     * for the whole cycle, so no other RPC user can steal the
     * notifications or overwrite the collector pointer. */
    bool is_spool_pull = strstr(json_in, "\"PullSpoolFragments\"") != NULL;
    spool_collector_t coll = {0};
    if (is_spool_pull) {
        coll.sem = xSemaphoreCreateBinary();
        if (!coll.sem) {
            xSemaphoreGive(s_cmd_mtx);
            return ESP_ERR_NO_MEM;
        }
        s_spool_collector = &coll;
    }

    clear_response();
    if (send_rpc_encrypted(json_in) != ESP_OK) {
        ESP_LOGE(TAG, "passthrough_rpc: send_rpc_encrypted failed");
        if (is_spool_pull) {
            s_spool_collector = NULL;
            vSemaphoreDelete(coll.sem);
        }
        xSemaphoreGive(s_cmd_mtx);
        return ESP_FAIL;
    }

    cJSON *resp = wait_response((int)timeout_ms);
    if (is_spool_pull) {
        /* Wait for the fragment notifications.  This is an additional
         * window of up to timeout_ms on top of the ack wait (worst case
         * 2x timeout_ms total); a pull that never completes returns the
         * ack plus whatever fragments arrived, with "spoolTimedOut":true. */
        int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
        while (!coll.done &&
               xSemaphoreTake(coll.sem, pdMS_TO_TICKS(500)) != pdTRUE) {
            if (esp_timer_get_time() >= deadline) {
                coll.timed_out = true;
                break;
            }
        }
        s_spool_collector = NULL;   /* detach before teardown */
    }
    xSemaphoreGive(s_cmd_mtx);

    if (!resp && !is_spool_pull) {
        ESP_LOGW(TAG, "passthrough_rpc: timeout waiting for AS11 response");
        return ESP_ERR_TIMEOUT;
    }

    if (is_spool_pull) {
        /* Build the combined response: the ack object plus the collected
         * fragments (base64 re-encoded, matching the wire format). */
        cJSON *out = resp ? resp : cJSON_CreateObject();
        if (!out) {
            vSemaphoreDelete(coll.sem);
            for (int i = 0; i < coll.frag_count; i++) free(coll.frags[i].data);
            return ESP_ERR_NO_MEM;
        }
        cJSON *frag_arr = cJSON_AddArrayToObject(out, "spoolFragments");
        size_t total_bytes = 0;
        int emitted = 0;
        for (int i = 0; i < coll.frag_count; i++) {
            /* Response-size bound: base64 inflates by ~4/3; keep the
             * JSON under ~96 KB so the HTTP layer and PSRAM stay
             * comfortable.  Excess fragments are dropped and flagged. */
            if (total_bytes + (size_t)coll.frags[i].len > 96 * 1024) {
                cJSON_AddBoolToObject(out, "spoolTruncated", true);
                break;
            }
            cJSON *f = cJSON_CreateObject();
            if (!f) break;
            cJSON_AddNumberToObject(f, "seq", coll.frags[i].seq);
            cJSON_AddNumberToObject(f, "len", coll.frags[i].len);
            unsigned char b64[4 * ((SPOOL_FRAG_MAX + 2) / 3) + 1];
            size_t olen = 0;
            if (mbedtls_base64_encode(b64, sizeof(b64), &olen,
                                      coll.frags[i].data,
                                      (size_t)coll.frags[i].len) == 0) {
                cJSON_AddStringToObject(f, "data", (const char *)b64);
            }
            cJSON_AddItemToArray(frag_arr, f);
            total_bytes += (size_t)coll.frags[i].len;
            emitted++;
        }
        if (coll.status[0]) {
            cJSON_AddStringToObject(out, "spoolStatus", coll.status);
        }
        if (coll.spool_hash_hex[0]) {
            cJSON_AddStringToObject(out, "spoolHash", coll.spool_hash_hex);
        }
        if (coll.timed_out) {
            cJSON_AddBoolToObject(out, "spoolTimedOut", true);
        }
        ESP_LOGI(TAG, "passthrough_rpc: spool pull collected %d frags "
                 "(%u bytes, status=%s)", emitted, (unsigned)total_bytes,
                 coll.status[0] ? coll.status : "?");
        for (int i = 0; i < coll.frag_count; i++) free(coll.frags[i].data);
        vSemaphoreDelete(coll.sem);

        char *resp_str = cJSON_PrintUnformatted(out);
        cJSON_Delete(out);
        if (!resp_str) return ESP_ERR_NO_MEM;
        *json_out = resp_str;
        return ESP_OK;
    }

    char *resp_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    if (!resp_str) {
        return ESP_ERR_NO_MEM;
    }

    *json_out = resp_str;
    return ESP_OK;
}
