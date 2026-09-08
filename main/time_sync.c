/*
 * SomnoTrace - NTP time synchronisation with DHCP option 42 support
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
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#include "time_sync.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <dirent.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs_writer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_netif_sntp.h"

#include "as11_ble.h"
#include "sd_storage.h"
#include "cJSON.h"

static const char *TAG = "time_sync";

#define NVS_NAMESPACE    "cfg"
#define NVS_KEY_TZ_STR   "tz_str"
#define NVS_KEY_TZ_NAME  "tz_name"
#define NVS_KEY_NTP_SRV  "ntp_srv"
#define NVS_KEY_DRIFT    "drift_ms"
#define NVS_KEY_DRIFT_AT "drift_at"
#define TZ_STR_MAX       64
#define TZ_NAME_MAX      40
#define NTP_SRV_MAX      64
#define SNTP_SYNC_MS    (3600 * 1000)   /* 1 hour */
#define NTP_INITIAL_TIMEOUT_MS  15000   /* per-attempt wait for initial sync */
#define NTP_INITIAL_ATTEMPTS    3

static bool s_synced = false;
static bool s_initial_sync_done = false;

/* ── Time-source provenance ─────────────────────────────────────────── */
static time_source_t s_source = TIME_SRC_NONE;
static int64_t s_drift_ms = 0;       /* last known drift (NTP - AS11) */
static int64_t s_drift_at_ms = 0;    /* NTP epoch ms when drift was measured */
static bool s_drift_loaded = false;  /* true once s_drift_ms/at loaded from NVS */
static const char *s_drift_src = "none";  /* provenance of s_drift_ms */

static void sntp_sync_cb(struct timeval *tv)
{
    s_synced = true;
    s_source = TIME_SRC_NTP;
    time_t now = tv->tv_sec;
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    ESP_LOGI(TAG, "NTP sync OK: %04d-%02d-%02d %02d:%02d:%02d",
             tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
}

static void apply_timezone(const char *tz_str)
{
    if (!tz_str || tz_str[0] == '\0') {
        tz_str = "UTC0";
    }
    setenv("TZ", tz_str, 1);
    tzset();
    ESP_LOGI(TAG, "timezone set to %s", tz_str);
}

/* Args for the NVS write, passed by pointer to the nvs_writer task. */
typedef struct {
    const char *tz_str;
    const char *tz_name;
} tz_save_args_t;

static esp_err_t do_set_timezone(void *arg)
{
    const tz_save_args_t *a = (const tz_save_args_t *)arg;
    char tz_str[TZ_STR_MAX];
    char tz_name[TZ_NAME_MAX];
    strlcpy(tz_str, a->tz_str ? a->tz_str : "", sizeof(tz_str));
    strlcpy(tz_name, a->tz_name ? a->tz_name : "", sizeof(tz_name));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, NVS_KEY_TZ_STR, tz_str);
    if (err == ESP_OK && tz_name[0]) {
        nvs_set_str(h, NVS_KEY_TZ_NAME, tz_name);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t time_sync_set_timezone(const char *tz_str, const char *tz_name)
{
    if (!tz_str || tz_str[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    /* Delegate the flash write so callers on a PSRAM stack (httpd) are safe. */
    tz_save_args_t args = { .tz_str = tz_str, .tz_name = tz_name };
    esp_err_t err = nvs_writer_run(do_set_timezone, &args);
    if (err == ESP_OK) {
        apply_timezone(tz_str);
    }
    return err;
}

typedef struct {
    const char *key;
    char *out;
    size_t out_len;
    bool ok;
} nvs_string_read_args_t;

/* The output pointer may refer to a PSRAM-backed caller stack.  Keep the NVS
 * read destination on this internal-stack proxy task, close the handle first,
 * and only then copy the result to the caller. */
static esp_err_t do_read_nvs_string(void *arg)
{
    nvs_string_read_args_t *a = arg;
    char key[16];
    strlcpy(key, a->key ? a->key : "", sizeof(key));
    char *out = a->out;
    size_t out_len = a->out_len;
    char value[64] = {0};
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) { a->ok = false; return err; }
    size_t value_len = sizeof(value);
    err = nvs_get_str(h, key, value, &value_len);
    nvs_close(h);
    a->ok = err == ESP_OK;
    if (a->ok && out && out_len) strlcpy(out, value, out_len);
    return err;
}

static void read_nvs_string(const char *key, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    nvs_string_read_args_t args = {
        .key = key, .out = out, .out_len = out_len, .ok = false,
    };
    nvs_writer_run(do_read_nvs_string, &args);
}

void time_sync_get_timezone(char *tz_str, size_t tz_str_len)
{
    read_nvs_string(NVS_KEY_TZ_STR, tz_str, tz_str_len);
}

void time_sync_get_tz_name(char *tz_name, size_t tz_name_len)
{
    read_nvs_string(NVS_KEY_TZ_NAME, tz_name, tz_name_len);
}

static esp_err_t do_set_ntp_server(void *arg)
{
    const char *server_arg = (const char *)arg;
    char server[ NTP_SRV_MAX ];
    strlcpy(server, server_arg ? server_arg : "", sizeof(server));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    if (server[0] != '\0') {
        err = nvs_set_str(h, NVS_KEY_NTP_SRV, server);
    } else {
        err = nvs_erase_key(h, NVS_KEY_NTP_SRV);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t time_sync_set_ntp_server(const char *server)
{
    /* Delegate the flash write so callers on a PSRAM stack (httpd) are safe. */
    esp_err_t err = nvs_writer_run(do_set_ntp_server, (void *)server);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NTP server set to: %s",
                 (server && server[0]) ? server : "(auto)");
    }
    return err;
}

void time_sync_get_ntp_server(char *server, size_t server_len)
{
    read_nvs_string(NVS_KEY_NTP_SRV, server, server_len);
}

bool time_sync_is_synced(void)
{
    return s_synced;
}

/* ── Time-source provenance API ─────────────────────────────────────── */

static bool load_drift_from_nvs(void);

time_source_t time_source_get(void)
{
    return s_source;
}

bool time_is_usable(void)
{
    return s_source != TIME_SRC_NONE;
}

int64_t time_source_drift_age_ms(void)
{
    if (s_drift_at_ms == 0) return -1;
    int64_t now_ms = (int64_t)time(NULL) * 1000;
    return now_ms - s_drift_at_ms;
}

bool time_sync_has_drift(void)
{
    if (s_drift_loaded) return true;
    return load_drift_from_nvs();
}

static bool load_drift_from_sd(void);

bool time_sync_get_drift_snapshot(time_drift_snapshot_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->source = "none";

    if (!s_drift_loaded) {
        /* NVS first, then the SD upgrade fallback (devices that predate the
         * NVS keys still have drift recorded in session manifests). */
        if (!load_drift_from_nvs()) load_drift_from_sd();
    }
    if (!s_drift_loaded) return false;

    out->available = true;
    out->drift_ms = s_drift_ms;
    out->measured_at_ms = s_drift_at_ms;
    out->source = s_drift_src;
    return true;
}

/* ── Drift persistence ─────────────────────────────────────────────── */

typedef struct {
    int64_t drift_ms;
    int64_t measured_at_ms;
} drift_save_args_t;

static esp_err_t do_save_drift(void *arg)
{
    const drift_save_args_t *a = (const drift_save_args_t *)arg;
    int64_t drift_ms = a->drift_ms;
    int64_t measured_at_ms = a->measured_at_ms;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_i64(h, NVS_KEY_DRIFT, drift_ms);
    if (err == ESP_OK) err = nvs_set_i64(h, NVS_KEY_DRIFT_AT, measured_at_ms);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void time_sync_save_drift(int64_t drift_ms, int64_t measured_at_ms)
{
    s_drift_ms = drift_ms;
    s_drift_at_ms = measured_at_ms;
    s_drift_loaded = true;
    s_drift_src = "nvs";

    drift_save_args_t args = { .drift_ms = drift_ms, .measured_at_ms = measured_at_ms };
    esp_err_t err = nvs_writer_run(do_save_drift, &args);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "drift saved: %lld ms (measured at %lld)",
                 (long long)drift_ms, (long long)measured_at_ms);
    } else {
        ESP_LOGW(TAG, "drift NVS save failed: %s", esp_err_to_name(err));
    }
}

typedef struct {
    int64_t drift_ms;
    int64_t measured_at_ms;
    bool ok;
} drift_read_args_t;

/* Executed by nvs_writer on its internal-RAM stack.  Copy the request values
 * into locals before entering NVS/flash, and copy results back only after the
 * NVS handle is closed and flash access is restored.  This is important even
 * though the caller may be using a PSRAM-backed stack. */
static esp_err_t do_load_drift_from_nvs(void *arg)
{
    drift_read_args_t *a = arg;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        a->ok = false;
        return err;
    }

    int64_t drift = 0, at = 0;
    bool ok = (nvs_get_i64(h, NVS_KEY_DRIFT, &drift) == ESP_OK &&
               nvs_get_i64(h, NVS_KEY_DRIFT_AT, &at) == ESP_OK);
    nvs_close(h);
    a->drift_ms = drift;
    a->measured_at_ms = at;
    a->ok = ok;
    return ok ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

/* Load drift from NVS into s_drift_ms / s_drift_at_ms.
 * Returns true if a valid drift sample was found.  All NVS access is routed
 * through the internal-stack proxy because this function is also reached from
 * the PSRAM-backed AS11 reconnect task. */
static bool load_drift_from_nvs(void)
{
    drift_read_args_t args = {0};
    nvs_writer_run(do_load_drift_from_nvs, &args);

    if (args.ok) {
        s_drift_ms = args.drift_ms;
        s_drift_at_ms = args.measured_at_ms;
        s_drift_loaded = true;
        s_drift_src = "nvs";
        ESP_LOGI(TAG, "drift loaded from NVS: %lld ms (age %lld s)",
                 (long long)args.drift_ms,
                 (long long)((time(NULL) * 1000 - args.measured_at_ms) / 1000));
    }
    return args.ok;
}

/* Fallback: scan SD session JSONs for the newest clock_drift_valid entry.
 * Used when NVS has no drift keys (upgrade path for existing devices). */
static bool load_drift_from_sd(void)
{
    char streams_path[64];
    snprintf(streams_path, sizeof(streams_path), "%s", SD_STREAMS_DIR);

    DIR *streams_dir = opendir(streams_path);
    if (!streams_dir) return false;

    int64_t best_drift = 0;
    int64_t best_start = 0;
    bool found = false;

    struct dirent *day_entry;
    while ((day_entry = readdir(streams_dir)) != NULL) {
        if (day_entry->d_name[0] == '.') continue;

        char day_path[320];
        snprintf(day_path, sizeof(day_path), "%s/%s", streams_path, day_entry->d_name);
        DIR *day_dir = opendir(day_path);
        if (!day_dir) continue;

        struct dirent *f_entry;
        while ((f_entry = readdir(day_dir)) != NULL) {
            const char *name = f_entry->d_name;
            size_t nlen = strlen(name);
            if (nlen < 13 || strcmp(name + nlen - 13, "_session.json") != 0)
                continue;

            char json_path[768];
            snprintf(json_path, sizeof(json_path), "%s/%s", day_path, name);
            FILE *f = fopen(json_path, "r");
            if (!f) continue;

            fseek(f, 0, SEEK_END);
            long fsize = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (fsize <= 0 || fsize > 8192) { fclose(f); continue; }

            char *buf = malloc(fsize + 1);
            if (!buf) { fclose(f); continue; }
            size_t rd = fread(buf, 1, fsize, f);
            fclose(f);
            buf[rd] = '\0';

            cJSON *root = cJSON_Parse(buf);
            free(buf);
            if (!root) continue;

            cJSON *valid = cJSON_GetObjectItem(root, "clock_drift_valid");
            if (valid && cJSON_IsTrue(valid)) {
                cJSON *d = cJSON_GetObjectItem(root, "clock_drift_ms");
                cJSON *s = cJSON_GetObjectItem(root, "start_epoch_ms");
                if (d && cJSON_IsNumber(d) && s && cJSON_IsNumber(s)) {
                    /* Select by session START time, not by the drift value.
                     * This previously read d->valuedouble (the drift), so the
                     * "newest sample wins" comparison actually ranked by drift
                     * magnitude and s_drift_at_ms was set to a drift, making
                     * the reported age meaningless. */
                    int64_t start = (int64_t)s->valuedouble;
                    if (!found || start > best_start) {
                        best_start = start;
                        best_drift = (int64_t)d->valuedouble;
                        found = true;
                    }
                }
            }
            cJSON_Delete(root);
        }
        closedir(day_dir);
    }
    closedir(streams_dir);

    if (found) {
        s_drift_ms = best_drift;
        s_drift_at_ms = best_start;
        s_drift_loaded = true;
        s_drift_src = "sd";
        ESP_LOGI(TAG, "drift loaded from SD: %lld ms (session start %lld)",
                 (long long)best_drift, (long long)best_start);
    }
    return found;
}

esp_err_t time_sync_recover_from_as11(void)
{
    if (s_source == TIME_SRC_NTP) {
        ESP_LOGI(TAG, "recover_from_as11: already NTP-synced, nothing to do");
        return ESP_OK;
    }

    /* Load drift from NVS, or fall back to SD scan for upgrades. */
    if (!s_drift_loaded) {
        if (!load_drift_from_nvs() && !load_drift_from_sd()) {
            ESP_LOGW(TAG, "recover_from_as11: no drift sample available");
            return ESP_ERR_NOT_FOUND;
        }
    }

    /* Query AS11 wall clock. */
    int64_t as11_ms = 0;
    esp_err_t err = as11_ble_get_datetime(&as11_ms);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "recover_from_as11: GetDateTime failed: %s",
                 esp_err_to_name(err));
        return ESP_FAIL;
    }

    /* Apply: wall = AS11 + drift. */
    int64_t wall_ms = as11_ms + s_drift_ms;
    struct timeval tv = { .tv_sec = wall_ms / 1000, .tv_usec = (wall_ms % 1000) * 1000 };
    settimeofday(&tv, NULL);

    s_source = TIME_SRC_AS11_DRIFT;

    time_t now = (time_t)(wall_ms / 1000);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    ESP_LOGW(TAG, "time recovered from AS11 + drift: "
             "%04d-%02d-%02d %02d:%02d:%02d "
             "(AS11=%lld drift=%lld ms, drift age=%lld s)",
             tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
             (long long)as11_ms, (long long)s_drift_ms,
             (long long)(time_source_drift_age_ms() / 1000));

    return ESP_OK;
}

void time_sync_apply_saved_timezone(void)
{
    char tz_str[TZ_STR_MAX];
    time_sync_get_timezone(tz_str, sizeof(tz_str));
    apply_timezone(tz_str);
}

esp_err_t time_sync_init(void)
{
    /* Idempotent — also called early in app_main() so that any session
     * started during BLE reconnect gets a local-time session id. */
    time_sync_apply_saved_timezone();

    /* Check for a user-configured custom NTP server in NVS. */
    char ntp_srv[NTP_SRV_MAX];
    time_sync_get_ntp_server(ntp_srv, sizeof(ntp_srv));
    bool has_custom_ntp = (ntp_srv[0] != '\0');

    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(
        has_custom_ntp ? ntp_srv : "pool.ntp.org");
    sntp_cfg.smooth_sync = false;
    sntp_cfg.sync_cb = sntp_sync_cb;

    if (has_custom_ntp) {
        /* Custom NTP: use it exclusively, no DHCP, no fallbacks. */
        sntp_cfg.server_from_dhcp = false;
        sntp_cfg.wait_for_sync = false;
        sntp_cfg.start = false;
        sntp_cfg.renew_servers_after_new_IP = false;
        sntp_cfg.ip_event_to_renew = 0;
        ESP_LOGI(TAG, "SNTP started — custom server: %s", ntp_srv);
    } else {
        /* Auto mode: DHCP option 42 + public NTP fallbacks. */
        sntp_cfg.server_from_dhcp = true;
        sntp_cfg.wait_for_sync = false;
        sntp_cfg.start = false;
        sntp_cfg.renew_servers_after_new_IP = true;
        sntp_cfg.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
        sntp_cfg.index_of_first_server = 1;  /* slot 0 = static fallback */
    }

    esp_err_t err = esp_netif_sntp_init(&sntp_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!has_custom_ntp) {
        /* Add a second fallback server */
        esp_sntp_setservername(2, "time.google.com");
    }

    /* Set periodic re-sync interval (1 hour) before starting */
    esp_sntp_set_sync_interval(SNTP_SYNC_MS);

    err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_start failed: %s", esp_err_to_name(err));
        return err;
    }

    if (has_custom_ntp) {
        ESP_LOGI(TAG, "SNTP started — custom server: %s, sync every %d ms", ntp_srv, SNTP_SYNC_MS);
    } else {
        ESP_LOGI(TAG, "SNTP started — DHCP option 42 + pool.ntp.org + time.google.com, sync every %d ms", SNTP_SYNC_MS);
    }
    return ESP_OK;
}

bool time_sync_wait_initial(void)
{
    if (s_synced) {
        s_initial_sync_done = true;
        return true;
    }

    /* SNTP is already running from time_sync_init().  It retries on its own
     * (lwIP default retry timeout ~15 s).  We poll s_synced in three windows
     * of NTP_INITIAL_TIMEOUT_MS each, giving ~45 s total for the first sync. */
    for (int attempt = 1; attempt <= NTP_INITIAL_ATTEMPTS; attempt++) {
        ESP_LOGI(TAG, "waiting for initial NTP sync (attempt %d/%d)...",
                 attempt, NTP_INITIAL_ATTEMPTS);

        int waited = 0;
        while (!s_synced && waited < NTP_INITIAL_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(500));
            waited += 500;
        }

        if (s_synced) {
            s_initial_sync_done = true;
            ESP_LOGI(TAG, "initial NTP sync succeeded on attempt %d", attempt);
            return true;
        }

        ESP_LOGW(TAG, "NTP sync attempt %d timed out after %d ms",
                 attempt, NTP_INITIAL_TIMEOUT_MS);
    }

    s_initial_sync_done = true;
    ESP_LOGE(TAG, "initial NTP sync failed after %d attempts", NTP_INITIAL_ATTEMPTS);
    return false;
}
