/*
 * SomnoTrace - Post-therapy data collection from AS11 spools and RPC
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

#include "post_therapy.h"
#include "as11_time.h"
#include "as11_ble.h"
#include "session_writer.h"
#include "sd_storage.h"
#include "settings_provenance.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "post_therapy";

/* ── Protobuf helpers (minimal, for Summary spool decoding) ─────────── */

static uint64_t pb_decode_varint(const uint8_t *buf, size_t buf_len, size_t *pos)
{
    uint64_t val = 0;
    int shift = 0;
    while (*pos < buf_len) {
        uint8_t b = buf[(*pos)++];
        val |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return val;
}

/* Extract a scalar varint field from a Summary record.
 * Returns the value, or 0 if the field is not found. */
static int64_t extract_scalar_field(const uint8_t *rec, size_t rec_len, int target_field)
{
    size_t pos = 0;
    while (pos < rec_len) {
        uint64_t tag = pb_decode_varint(rec, rec_len, &pos);
        int field = (int)(tag >> 3);
        int wire = (int)(tag & 0x07);
        if (field == 0) break;

        if (field == target_field && wire == 0) {
            return (int64_t)pb_decode_varint(rec, rec_len, &pos);
        } else if (wire == 0) {
            pb_decode_varint(rec, rec_len, &pos);
        } else if (wire == 2) {
            size_t lpos = pos;
            uint64_t flen = pb_decode_varint(rec, rec_len, &lpos);
            pos = lpos + flen;
        } else if (wire == 1) {
            pos += 8;
        } else if (wire == 5) {
            pos += 4;
        } else {
            break;
        }
    }
    return 0;
}

/* Extract PeriodStart (field 2, varint) from a Summary record. */
static int64_t extract_period_start(const uint8_t *rec, size_t rec_len)
{
    return extract_scalar_field(rec, rec_len, 2);
}

/* Extract ClockB (field 40, varint) from a Summary record.
 * ClockB is the AS11's internal clock timestamp at the time the spool
 * record was last written.  Used for staleness detection. */
static int64_t extract_clock_b(const uint8_t *rec, size_t rec_len)
{
    return extract_scalar_field(rec, rec_len, 40);
}

/* Compute the noon-based day label (YYYYMMDD) for an AS11-clock timestamp.
 *
 * The AS11 defines its reporting day noon-to-noon in ITS OWN timezone, so the
 * label must be derived with the device's offset rather than the ESP's.  When
 * the two differ, using ESP local time shifts every Summary record a day (see
 * as11_time.h and issue #75).  as11_time_noon_day() falls back to ESP local
 * time when the offset is not yet known. */
static void noon_day_from_epoch(int64_t epoch_ms, char *out, size_t out_len)
{
    as11_time_noon_day(epoch_ms, out, out_len);
}

/* ── File helpers ───────────────────────────────────────────────────── */

/* Read a binary file into a malloc'd buffer.
 * Returns NULL on failure.  Caller must free(). */
static uint8_t *read_bin_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc(sz);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return NULL; }
    if (out_len) *out_len = rd;
    return buf;
}

static esp_err_t write_bin_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "failed to open %s for writing: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    if (written != len) {
        ESP_LOGE(TAG, "short write to %s: %u/%u", path, (unsigned)written, (unsigned)len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Atomic-ish write: write to temp file, then rename over target.
 * FAT32 rename() fails with EEXIST, so remove the target first. */
static esp_err_t write_bin_atomic(const char *path, const uint8_t *data, size_t len)
{
    char tmp_path[330];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    if (write_bin_file(tmp_path, data, len) != ESP_OK) return ESP_FAIL;

    remove(path);
    if (rename(tmp_path, path) != 0) {
        ESP_LOGE(TAG, "rename %s → %s failed: %s", tmp_path, path, strerror(errno));
        remove(tmp_path);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "wrote %s (%u bytes)", path, (unsigned)len);
    return ESP_OK;
}

static esp_err_t write_json_file(const char *path, const cJSON *json)
{
    char *str = cJSON_Print(json);
    if (!str) return ESP_FAIL;

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "failed to open %s: %s", path, strerror(errno));
        free(str);
        return ESP_FAIL;
    }
    fputs(str, f);
    fputc('\n', f);
    fclose(f);
    free(str);
    ESP_LOGI(TAG, "wrote %s", path);
    return ESP_OK;
}

static void epoch_ms_to_iso_utc(int64_t epoch_ms, char *out, size_t out_len)
{
    time_t t = (time_t)(epoch_ms / 1000);
    int ms = (int)(epoch_ms % 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(out, out_len, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             (tm.tm_year + 1900) % 10000, (tm.tm_mon + 1) % 100, tm.tm_mday % 100,
             tm.tm_hour % 100, tm.tm_min % 100, tm.tm_sec % 100, ms % 1000);
}

/* ── Summary spool decode & per-day storage ─────────────────────────── */

/* Pull Summary spool with 30-day lookback, decode into per-day files.
 * Each day record is written to .somnotrace/sessions/summaries/YYYYMMDD.spool
 * (atomic write — latest pull wins). */
static esp_err_t collect_summary_spool(int64_t clock_drift_ms)
{
    /* Fixed 30-day lookback window — no state file needed. */
    int64_t now_ms = (int64_t)time(NULL) * 1000;
    int64_t from_ms = now_ms - 30LL * 86400 * 1000;
    char from_dt[32];
    epoch_ms_to_iso_utc(from_ms, from_dt, sizeof(from_dt));
    ESP_LOGI(TAG, "Summary spool: fromDateTime=%s (30-day lookback)", from_dt);

    uint8_t *data = NULL;
    size_t len = 0;
    esp_err_t ret = as11_ble_spool_pull("Summary", from_dt, &data, &len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Summary spool pull failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (!data || len == 0) {
        ESP_LOGI(TAG, "Summary spool is empty");
        free(data);
        return ESP_OK;
    }

    /* Iterate top-level field-2 wrappers (each contains a day record) */
    int days_written = 0;
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag = pb_decode_varint(data, len, &pos);
        int field = (int)(tag >> 3);
        int wire = (int)(tag & 0x07);
        if (field == 0) break;

        if (field == 2 && wire == 2) {
            size_t lpos = pos;
            uint64_t flen = pb_decode_varint(data, len, &lpos);
            pos = lpos;
            if (pos + flen > len) break;

            const uint8_t *rec = data + pos;
            size_t rec_len = (size_t)flen;

            /* Extract PeriodStart from this day record.
             * Use the raw AS11 timestamp (no drift correction) for the
             * noon-day label — the AS11 defines its noon-day boundaries
             * by its own clock, and edf_gen.c also uses raw PeriodStart
             * for day classification.  Applying drift here would shift
             * the label by one day when the corrected time falls before
             * noon (e.g. 12:00 AS11 → 11:52 NTP → wrong day). */
            int64_t period_start = extract_period_start(rec, rec_len);
            if (period_start > 0) {
                char day_label[16];
                /* Deriving variant: PeriodStart is a noon stamp, so this also
                 * teaches as11_time the device's offset on the very first
                 * record — before collect_settings() has run. */
                as11_time_noon_day_for_period_start(period_start, day_label,
                                                    sizeof(day_label));

                char spool_path[300];
                snprintf(spool_path, sizeof(spool_path), "%s/%s.spool",
                         SD_SUMMARIES_DIR, day_label);
                write_bin_atomic(spool_path, rec, rec_len);
                days_written++;
            }
            pos += flen;
        } else if (wire == 2) {
            size_t lpos = pos;
            uint64_t flen = pb_decode_varint(data, len, &lpos);
            pos = lpos + flen;
        } else if (wire == 0) {
            pb_decode_varint(data, len, &pos);
        } else if (wire == 1) {
            pos += 8;
        } else if (wire == 5) {
            pos += 4;
        } else {
            break;
        }
    }

    free(data);
    ESP_LOGI(TAG, "Summary spool: wrote %d day record(s) to %s", days_written, SD_SUMMARIES_DIR);
    return ESP_OK;
}

/* ── Spool collection (TherapyEvents) ───────────────────────────────── */

static esp_err_t collect_resp_events(const char *dir, const char *prefix,
                                     const char *from_dt)
{
    uint8_t *data = NULL;
    size_t len = 0;

    esp_err_t ret = as11_ble_spool_pull("TherapyEvents-RespiratoryEvents",
                                        from_dt, &data, &len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "resp events spool pull failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (data && len > 0) {
        char path[330];
        snprintf(path, sizeof(path), "%s/%s_resp_events.bin", dir, prefix);
        write_bin_file(path, data, len);
    } else {
        ESP_LOGI(TAG, "resp events spool is empty");
        char path[330];
        snprintf(path, sizeof(path), "%s/%s_resp_events.bin", dir, prefix);
        FILE *f = fopen(path, "wb");
        if (f) fclose(f);
    }

    free(data);
    return ESP_OK;
}

/* ── Device identification via Get RPC ──────────────────────────────── */

static const char *const IDENTITY_KEYS[] = {
    "UniversalIdentifier",
    "SerialNumber",
    "ProductCode",
    "ProductName",
    "ProductGeographicIdentifier",
    "HardwareIdentifier",
    "BootloaderIdentifier",
    "ApplicationIdentifier",
    "ConfigurationIdentifier",
    "PlatformIdentifier",
    "VariantIdentifier",
    "RegionIdentifier",
    "ProfileVariantIdentifier",
    "DataVersionIdentifier",
    "DataModelVersionIdentifier",
};

static const char *const SETTINGS_KEYS[] = {
    "SettingProfiles",
};

static const char *json_get_str(const cJSON *obj, const char *key)
{
    const cJSON *v = obj ? cJSON_GetObjectItem(obj, key) : NULL;
    return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
}

static void save_device_metadata_snapshot(const cJSON *ident, const cJSON *settings)
{
    size_t existing_len = 0;
    uint8_t *existing_raw = read_bin_file(SD_DEVICE_METADATA_PATH, &existing_len);
    cJSON *existing = NULL;
    if (existing_raw && existing_len > 0) {
        char * str = malloc(existing_len + 1);
        if (str) {
            memcpy(str, existing_raw, existing_len);
            str[existing_len] = '\0';
            existing = cJSON_Parse(str);
            free(str);
        }
    }

    const cJSON *old_ident = existing ? cJSON_GetObjectItem(existing, "ident") : NULL;
    const cJSON *old_settings = existing ? cJSON_GetObjectItem(existing, "settings") : NULL;
    if (!cJSON_IsObject(old_ident)) old_ident = NULL;
    if (!cJSON_IsObject(old_settings)) old_settings = NULL;

    const cJSON *use_ident = ident ? ident : old_ident;
    if (!use_ident) {
        if (existing) cJSON_Delete(existing);
        free(existing_raw);
        return;
    }

    const char *serial = json_get_str(use_ident, "SerialNumber");
    const char *old_serial = json_get_str(old_ident, "SerialNumber");

    const cJSON *use_settings = settings;
    if (!use_settings && old_settings) {
        /* Only carry over cached settings if they belong to the same machine. */
        if (!ident || strcmp(serial, old_serial) == 0) {
            use_settings = old_settings;
        }
    }

    cJSON *snap = cJSON_CreateObject();
    if (snap) {
        cJSON_AddStringToObject(snap, "serial", serial);
        cJSON_AddItemReferenceToObject(snap, "ident", (cJSON *)use_ident);
        if (use_settings) {
            cJSON_AddItemReferenceToObject(snap, "settings", (cJSON *)use_settings);
        }
        char *new_str = cJSON_PrintUnformatted(snap);
        cJSON_Delete(snap);
        if (new_str) {
            size_t new_len = strlen(new_str);
            bool unchanged = (existing_raw && existing_len == new_len &&
                              memcmp(existing_raw, new_str, new_len) == 0);
            if (!unchanged) {
                if (write_bin_atomic(SD_DEVICE_METADATA_PATH,
                                     (const uint8_t *)new_str, new_len) == ESP_OK) {
                    ESP_LOGI(TAG, "saved device metadata snapshot (serial=%s)",
                             serial[0] ? serial : "?");
                }
            }
            free(new_str);
        }
    }

    if (existing) cJSON_Delete(existing);
    free(existing_raw);
}

void post_therapy_cache_device_metadata(void)
{
    if (!sd_storage_is_ready()) return;

    cJSON *ident = as11_ble_get_values(
        IDENTITY_KEYS, sizeof(IDENTITY_KEYS) / sizeof(IDENTITY_KEYS[0]));
    if (!ident) {
        ESP_LOGW(TAG, "pre-stream metadata: failed to get device identification");
        return;
    }

    cJSON *settings = as11_ble_get_values(
        SETTINGS_KEYS, sizeof(SETTINGS_KEYS) / sizeof(SETTINGS_KEYS[0]));
    if (settings) {
        as11_offset_t as11_off;
        if (as11_time_offset_from_settings(settings, &as11_off)) {
            ESP_LOGI(TAG, "AS11 timezone offset %d min (from pre-stream settings)",
                     (int)(as11_off / 60));
        }
    } else {
        ESP_LOGW(TAG, "pre-stream metadata: failed to get device settings");
    }

    save_device_metadata_snapshot(ident, settings);
    cJSON_Delete(ident);
    if (settings) cJSON_Delete(settings);
}

static esp_err_t write_session_settings_files(const char *dir, const char *prefix,
                                              const cJSON *settings, bool live)
{
    as11_offset_t as11_off;
    if (as11_time_offset_from_settings(settings, &as11_off)) {
        ESP_LOGI(TAG, "AS11 timezone offset %d min (from device settings)",
                 (int)(as11_off / 60));
    }

    const char *slash = strrchr(dir, '/');
    const char *day_label = slash ? slash + 1 : dir;
    cJSON *marked = cJSON_Duplicate(settings, true);
    if (!marked || !settings_provenance_set(marked,
            live ? SETTINGS_SOURCE_LIVE : SETTINGS_SOURCE_CACHED, day_label, prefix)) {
        cJSON_Delete(marked);
        return ESP_FAIL;
    }

    char path[330];
    int n = snprintf(path, sizeof(path), "%s/%s.%s.settings.json", dir, prefix,
                     live ? "live" : "cached");
    char *json = cJSON_PrintUnformatted(marked);
    if (!json || n < 0 || (size_t)n + 4 >= sizeof(path) ||
        write_bin_atomic(path, (const uint8_t *)json, strlen(json)) != ESP_OK) {
        free(json);
        cJSON_Delete(marked);
        return ESP_FAIL;
    }

    /* Keep legacy snapshots untouched; cached device settings are not history. */
    if (live) {
        char sum_settings_path[300];
        snprintf(sum_settings_path, sizeof(sum_settings_path), "%s/%s.live.settings.json",
                 SD_SUMMARIES_DIR, day_label);
        if (write_bin_atomic(sum_settings_path, (const uint8_t *)json, strlen(json)) != ESP_OK) {
            ESP_LOGW(TAG, "could not write daily live settings snapshot");
        }
    }
    free(json);
    cJSON_Delete(marked);
    return ESP_OK;
}

static void fallback_from_device_metadata(const char *dir, const char *prefix,
                                          bool *ident_ok, bool *settings_ok)
{
    size_t raw_len = 0;
    uint8_t *raw = read_bin_file(SD_DEVICE_METADATA_PATH, &raw_len);
    if (!raw || raw_len == 0) {
        free(raw);
        return;
    }
    char *str = malloc(raw_len + 1);
    if (!str) {
        free(raw);
        return;
    }
    memcpy(str, raw, raw_len);
    str[raw_len] = '\0';
    free(raw);

    cJSON *snap = cJSON_Parse(str);
    free(str);
    if (!snap) return;

    if (ident_ok && !*ident_ok) {
        cJSON *id_obj = cJSON_GetObjectItem(snap, "ident");
        if (cJSON_IsObject(id_obj)) {
            char path[330];
            snprintf(path, sizeof(path), "%s/%s_ident.json", dir, prefix);
            if (write_json_file(path, id_obj) == ESP_OK) {
                ESP_LOGW(TAG, "used cached device identification for %s", prefix);
                *ident_ok = true;
            }
        }
    }
    if (settings_ok && !*settings_ok) {
        cJSON *st_obj = cJSON_GetObjectItem(snap, "settings");
        if (cJSON_IsObject(st_obj)) {
            if (write_session_settings_files(dir, prefix, st_obj, false) == ESP_OK) {
                ESP_LOGW(TAG, "used cached device settings for %s", prefix);
                *settings_ok = true;
            }
        }
    }
    cJSON_Delete(snap);
}

static esp_err_t collect_identification(const char *dir, const char *prefix)
{
    cJSON *ident = as11_ble_get_values(
        IDENTITY_KEYS, sizeof(IDENTITY_KEYS) / sizeof(IDENTITY_KEYS[0]));
    if (!ident) {
        ESP_LOGW(TAG, "failed to get device identification");
        return ESP_FAIL;
    }

    char path[330];
    snprintf(path, sizeof(path), "%s/%s_ident.json", dir, prefix);
    esp_err_t err = write_json_file(path, ident);
    save_device_metadata_snapshot(ident, NULL);
    cJSON_Delete(ident);
    return err;
}

static esp_err_t collect_settings(const char *dir, const char *prefix)
{
    cJSON *settings = as11_ble_get_values(
        SETTINGS_KEYS, sizeof(SETTINGS_KEYS) / sizeof(SETTINGS_KEYS[0]));
    if (!settings) {
        ESP_LOGW(TAG, "failed to get device settings");
        return ESP_FAIL;
    }

    esp_err_t err = write_session_settings_files(dir, prefix, settings, true);
    save_device_metadata_snapshot(NULL, settings);
    cJSON_Delete(settings);
    return err;
}

/* ── Spool staleness detection ──────────────────────────────────────── */

/* Tolerance for ClockB comparison (ms).  ClockB is the AS11 clock at the
 * time the spool record was last written.  After therapy ends, the AS11
 * needs some time to compute stats and update the spool.  We check whether
 * ClockB >= AS11 session end time - tolerance.
 *
 * 5 seconds accounts for small timing differences between when the ESP
 * detects therapy end and when the AS11 considers the session ended,
 * plus ClockB quantisation.  Clock drift is already corrected for, so
 * this tolerance can be tight. */
#define SPOOL_FRESHNESS_TOL_MS  5000

/* Field 40 (ClockB) in the Summary protobuf. */
#define SUM_F_CLOCK_B           40

/* Check if the current day's Summary spool record is fresh (updated after
 * the current session ended).
 *
 * Reads the current noon-day's .spool file from SD, extracts ClockB
 * (field 40 — AS11 clock at last spool write), and compares it against
 * the AS11-equivalent session end time.
 *
 * Returns true if the spool is current, false if stale or missing. */
static bool summary_spool_is_current(int64_t end_epoch_ms, int64_t clock_drift_ms)
{
    /* Compute the AS11-equivalent session end time.
     * clock_drift_ms = NTP - AS11, so AS11 = NTP - drift. */
    int64_t as11_end_ms = end_epoch_ms - clock_drift_ms;
    int64_t threshold = as11_end_ms - SPOOL_FRESHNESS_TOL_MS;

    /* Determine the current noon-day label from the AS11 end time
     * (the AS11 defines noon-day boundaries by its own clock). */
    char day_label[16];
    noon_day_from_epoch(as11_end_ms, day_label, sizeof(day_label));

    char spool_path[300];
    snprintf(spool_path, sizeof(spool_path), "%s/%s.spool",
             SD_SUMMARIES_DIR, day_label);

    size_t spool_len = 0;
    uint8_t *spool_data = read_bin_file(spool_path, &spool_len);
    if (!spool_data || spool_len == 0) {
        ESP_LOGW(TAG, "spool_is_current: no spool file for %s", day_label);
        free(spool_data);
        return false;
    }

    int64_t clock_b = extract_clock_b(spool_data, spool_len);
    free(spool_data);

    if (clock_b == 0) {
        ESP_LOGW(TAG, "spool_is_current: ClockB not present in spool for %s", day_label);
        return false;
    }

    bool fresh = (clock_b >= threshold);
    ESP_LOGI(TAG, "spool_is_current: day=%s ClockB=%lld as11_end=%lld "
             "threshold=%lld → %s",
             day_label, (long long)clock_b, (long long)as11_end_ms,
             (long long)threshold, fresh ? "FRESH" : "STALE");
    return fresh;
}

/* Pull only the current noon-day's Summary spool record and write it to
 * .somnotrace/sessions/summaries/YYYYMMDD.spool (atomic — latest pull wins).
 *
 * Uses a fromDateTime starting at noon today (AS11 time) so only the
 * current day's record is returned, making each retry fast.
 * Returns ESP_OK on success (even if no record found for today). */
static esp_err_t refresh_today_summary_spool(int64_t end_epoch_ms,
                                              int64_t clock_drift_ms)
{
    int64_t as11_end_ms = end_epoch_ms - clock_drift_ms;

    /* Compute noon today (AS11 time) as the fromDateTime. */
    char day_label[16];
    as11_time_noon_day(as11_end_ms, day_label, sizeof(day_label));
    int64_t noon_s = as11_time_local_noon_epoch(day_label);
    int64_t noon_ms = noon_s * 1000;

    char from_dt[32];
    epoch_ms_to_iso_utc(noon_ms, from_dt, sizeof(from_dt));

    uint8_t *data = NULL;
    size_t len = 0;
    esp_err_t ret = as11_ble_spool_pull("Summary", from_dt, &data, &len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "refresh_today: spool pull failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (!data || len == 0) {
        ESP_LOGI(TAG, "refresh_today: spool is empty");
        free(data);
        return ESP_OK;
    }

    /* Iterate top-level field-2 wrappers (each contains a day record) */
    int days_written = 0;
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag = pb_decode_varint(data, len, &pos);
        int field = (int)(tag >> 3);
        int wire = (int)(tag & 0x07);
        if (field == 0) break;

        if (field == 2 && wire == 2) {
            size_t lpos = pos;
            uint64_t flen = pb_decode_varint(data, len, &lpos);
            pos = lpos;
            if (pos + flen > len) break;

            const uint8_t *rec = data + pos;
            size_t rec_len = (size_t)flen;

            int64_t period_start = extract_period_start(rec, rec_len);
            if (period_start > 0) {
                char day_label[16];
                /* Deriving variant: PeriodStart is a noon stamp, so this also
                 * teaches as11_time the device's offset on the very first
                 * record — before collect_settings() has run. */
                as11_time_noon_day_for_period_start(period_start, day_label,
                                                    sizeof(day_label));

                char spool_path[300];
                snprintf(spool_path, sizeof(spool_path), "%s/%s.spool",
                         SD_SUMMARIES_DIR, day_label);
                write_bin_atomic(spool_path, rec, rec_len);
                days_written++;
            }
            pos += flen;
        } else if (wire == 2) {
            size_t lpos = pos;
            uint64_t flen = pb_decode_varint(data, len, &lpos);
            pos = lpos + flen;
        } else if (wire == 0) {
            pb_decode_varint(data, len, &pos);
        } else if (wire == 1) {
            pos += 8;
        } else if (wire == 5) {
            pos += 4;
        } else {
            break;
        }
    }

    free(data);
    ESP_LOGI(TAG, "refresh_today: wrote %d day record(s)", days_written);
    return ESP_OK;
}

/* ── Incident diagnostics (deferred spool pull, Phase C) ───────────────
 *
 * When the night had a supervision-timeout disconnect or an unfillable
 * long gap (incident latch in as11_ble.c), pull the AS11's own
 * diagnostic event spools — CellularActivityEvents first, then the
 * system/diagnostic exception families — and store them as raw .bin
 * files next to the session.  This is what should tell us, next time,
 * whether the AS11's modem was active, whether it logged an internal
 * fault, or whether nothing unusual happened on its side at all.
 *
 * Trigger rules (see .ai/RECONNECT/PLAN.md):
 *  - NEVER on the split path: a split finalize is mid-therapy (a new
 *    session is already active), so the active-session check below
 *    naturally defers the pull to the night's final post-therapy.
 *  - Bounded: 8 KiB per spool via as11_ble_spool_pull_bounded; a spool
 *    larger than that is skipped (ESP_ERR_INVALID_SIZE), not truncated.
 *  - Raw only: DiagnosticTenMinutePeriodic is Rice-compressed and has no
 *    validated decoder — it is NOT pulled here.
 *  - The latch is cleared when the round produced definitive answers
 *    (data written, empty, oversized, or device-refused).  Transport
 *    failures keep it pending for the next stop, up to 3 attempts. */
#define DIAG_SPOOL_MAX_BYTES  8192

static const char *const DIAG_SPOOLS[] = {
    "CellularActivityEvents",                     /* modem/RF activity     */
    "SystemExceptionEvents-SystemErrors",
    "SystemExceptionEvents-RecoverableErrors",
    "DiagnosticExceptionEvents-ResettableErrors",
    "DiagnosticExceptionEvents-FatalErrors",
    "SystemActivityEvents-SporadicActivityEvents",
    "SystemActivityEvents-FrequentActivityEvents",
};

/* Sanitize a spool name for use in a filename (keep [A-Za-z0-9-]). */
static void diag_sanitize_name(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        out[o++] = (isalnum(c) || c == '-') ? (char)c : '-';
    }
    out[o] = '\0';
}

static void collect_incident_diagnostics(const char *dir, const char *prefix,
                                         int64_t clock_drift_ms)
{
    if (!as11_ble_incident_pending()) {
        return;
    }
    /* Defer while therapy is active: a split finalize runs this function
     * with the replacement session already streaming.  The latch stays
     * pending and the night's final post-therapy picks it up. */
    if (session_writer_get_active()) {
        ESP_LOGI(TAG, "incident diagnostics: therapy active — deferring");
        return;
    }

    int64_t incident_ms = as11_ble_incident_time_ms();
    /* Spool fromDateTime is in AS11 clock time: NTP - drift.  Start the
     * window 15 min before the incident so pre-incident activity is
     * captured too. */
    int64_t from_ms = incident_ms - clock_drift_ms - 15LL * 60 * 1000;
    if (from_ms < 0) from_ms = 0;
    char from_dt[32];
    epoch_ms_to_iso_utc(from_ms, from_dt, sizeof(from_dt));

    ESP_LOGI(TAG, "=== INCIDENT DIAGNOSTICS (%s at %lld ms, from %s) ===",
             as11_ble_incident_pending() ? "latched" : "?",
             (long long)incident_ms, from_dt);
    as11_ble_incident_note_attempt();

    int definitive = 0;   /* spools that returned a conclusive answer */
    int failures = 0;     /* transport-level failures (retry-worthy)   */
    int total_bytes = 0;

    for (size_t i = 0; i < sizeof(DIAG_SPOOLS) / sizeof(DIAG_SPOOLS[0]); i++) {
        const char *spool = DIAG_SPOOLS[i];
        uint8_t *data = NULL;
        size_t len = 0;
        esp_err_t ret = as11_ble_spool_pull_bounded(spool, from_dt,
                                                    &data, &len,
                                                    DIAG_SPOOL_MAX_BYTES);
        char safe_name[48];
        diag_sanitize_name(spool, safe_name, sizeof(safe_name));
        char path[330];
        snprintf(path, sizeof(path), "%s/%s_diag_%s.bin",
                 dir, prefix, safe_name);

        switch (ret) {
        case ESP_OK:
            if (data && len > 0) {
                if (write_bin_file(path, data, len) == ESP_OK) {
                    ESP_LOGI(TAG, "incident diag: %s → %s (%u bytes)",
                             spool, path, (unsigned)len);
                    total_bytes += (int)len;
                } else {
                    ESP_LOGE(TAG, "incident diag: %s write failed", spool);
                    failures++;
                }
            } else {
                ESP_LOGI(TAG, "incident diag: %s empty", spool);
            }
            definitive++;
            break;
        case ESP_ERR_INVALID_SIZE:
            ESP_LOGW(TAG, "incident diag: %s exceeds %d-byte cap — skipped",
                     spool, DIAG_SPOOL_MAX_BYTES);
            definitive++;
            break;
        case ESP_ERR_INVALID_RESPONSE:
            /* Device refused (e.g. ERROR_DATA_UNAVAILABLE) or integrity
             * check failed — conclusive for this spool. */
            ESP_LOGW(TAG, "incident diag: %s refused/corrupt", spool);
            definitive++;
            break;
        default:
            ESP_LOGW(TAG, "incident diag: %s pull failed: %s",
                     spool, esp_err_to_name(ret));
            failures++;
            break;
        }
        free(data);
    }

    ESP_LOGI(TAG, "=== INCIDENT DIAGNOSTICS DONE: %d definitive, %d failed, "
             "%d bytes ===", definitive, failures, total_bytes);

    /* Clear the latch when the round answered conclusively, or after
     * three attempts so a persistently broken link cannot burn BLE time
     * at every stop, night after night. */
    if (failures == 0 || as11_ble_incident_attempts() >= 3) {
        as11_ble_incident_clear();
        ESP_LOGI(TAG, "incident diagnostics: latch cleared");
    }
}

/* ── Main collection entry point ────────────────────────────────────── */

esp_err_t post_therapy_collect(const char *session_dir, const char *file_prefix,
                               int64_t start_epoch_ms, int64_t clock_drift_ms,
                               int64_t end_epoch_ms, bool *spool_current)
{
    if (!session_dir || !file_prefix) return ESP_ERR_INVALID_ARG;
    if (spool_current) *spool_current = false;
    if (!sd_storage_is_ready()) {
        ESP_LOGW(TAG, "SD not ready, skipping post-therapy collection");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "=== POST-THERAPY COLLECTION START ===");
    ESP_LOGI(TAG, "session_dir=%s prefix=%s", session_dir, file_prefix);

    /* TherapyEvents spool uses session start time (with small margin). */
    char from_dt[32];
    epoch_ms_to_iso_utc(start_epoch_ms - 60000, from_dt, sizeof(from_dt));

    int errors = 0;

    /* 1. Pull Summary spool with 30-day lookback → per-day .spool files */
    if (collect_summary_spool(clock_drift_ms) != ESP_OK) {
        errors++;
    }

    /* 2. Pull TherapyEvents-RespiratoryEvents spool → <prefix>_resp_events.bin
     * TODO: This spool pull is no longer needed for EVE.edf/CSL.edf generation
     * (those now read from events.snt). It remains as a diagnostic backup.
     * Removing it would save ~1-2 s of BLE time at session end. */
    if (collect_resp_events(session_dir, file_prefix, from_dt) != ESP_OK) {
        errors++;
    }

    /* 3 & 4. Get device identification and a provenance-tagged settings
     * snapshot for this session.
     *
     * If an ATT write stalls here, NimBLE's 30 s ATT transaction timer will
     * terminate the connection and auto-reconnect in ~5 s.  Wait passively
     * for the new BLE session epoch (up to 45 s) and retry only the failed
     * step(s); if BLE remains unreachable, fall back to the same-machine
     * snapshot captured on session setup (SD_DEVICE_METADATA_PATH). */
    uint32_t ble_epoch = as11_ble_get_session_epoch();
    bool reconnected = false;

    bool ident_ok = (collect_identification(session_dir, file_prefix) == ESP_OK);
    if (!ident_ok) {
        ESP_LOGW(TAG, "identification RPC failed (epoch=%lu) — waiting up to 45s for BLE reconnect",
                 (unsigned long)ble_epoch);
        if (as11_ble_wait_for_reconnect(ble_epoch, 45000)) {
            reconnected = true;
            ble_epoch = as11_ble_get_session_epoch();
            ESP_LOGI(TAG, "BLE reconnected (epoch=%lu), retrying identification",
                     (unsigned long)ble_epoch);
            ident_ok = (collect_identification(session_dir, file_prefix) == ESP_OK);
        }
    }

    bool settings_ok = (collect_settings(session_dir, file_prefix) == ESP_OK);
    if (!settings_ok && !reconnected) {
        ESP_LOGW(TAG, "settings RPC failed (epoch=%lu) — waiting up to 45s for BLE reconnect",
                 (unsigned long)ble_epoch);
        if (as11_ble_wait_for_reconnect(ble_epoch, 45000)) {
            ble_epoch = as11_ble_get_session_epoch();
            ESP_LOGI(TAG, "BLE reconnected (epoch=%lu), retrying settings",
                     (unsigned long)ble_epoch);
            if (!ident_ok) {
                ident_ok = (collect_identification(session_dir, file_prefix) == ESP_OK);
            }
            settings_ok = (collect_settings(session_dir, file_prefix) == ESP_OK);
        }
    }

    if (!ident_ok || !settings_ok) {
        fallback_from_device_metadata(session_dir, file_prefix, &ident_ok, &settings_ok);
    }
    if (!ident_ok) errors++;
    if (!settings_ok) errors++;

    /* 5. Check if the current day's spool is fresh (updated after session end). */
    bool fresh = summary_spool_is_current(end_epoch_ms, clock_drift_ms);
    if (spool_current) *spool_current = fresh;

    /* 6. Deferred incident diagnostics (Phase C): if the night had a
     * supervision-timeout disconnect or an unfillable long gap and
     * therapy is now fully stopped, pull the AS11's diagnostic event
     * spools once.  The function itself re-checks the latch and the
     * active-session state, so calling it unconditionally here is safe.
     * Snapshot the latch state first — a successful round clears it. */
    bool had_incident = as11_ble_incident_pending();
    collect_incident_diagnostics(session_dir, file_prefix, clock_drift_ms);

    /* Write manifest with clock_drift_ms for EDF generation */
    cJSON *manifest = cJSON_CreateObject();
    cJSON_AddStringToObject(manifest, "collection_time", from_dt);
    cJSON_AddNumberToObject(manifest, "clock_drift_ms", (double)clock_drift_ms);
    cJSON_AddNumberToObject(manifest, "errors", errors);
    cJSON_AddBoolToObject(manifest, "spool_current", fresh);
    cJSON_AddBoolToObject(manifest, "incident_diagnostics", had_incident);
    char mpath[330];
    snprintf(mpath, sizeof(mpath), "%s/%s_manifest.json", session_dir, file_prefix);
    write_json_file(mpath, manifest);
    cJSON_Delete(manifest);

    ESP_LOGI(TAG, "=== POST-THERAPY COLLECTION DONE (%d errors, spool %s) ===",
             errors, fresh ? "CURRENT" : "STALE");
    return errors > 0 ? ESP_FAIL : ESP_OK;
}

/* Return the AS11-clock timestamp at which the noon-day period containing
 * as11_ms closes (i.e. the following noon). */
static int64_t noon_period_end(int64_t as11_ms)
{
    return as11_time_noon_period_end_ms(as11_ms);
}

/* Wait for the AS11 to update its Summary spool after TherapyStop, then
 * pull the fresh data.  Called from a low-priority task (core 0).
 *
 * The AS11 pushes an _SNC ValueChange EventNotification when it writes
 * new Summary data (nor:1:/Summary.bin).  We subscribe to "_SNC" in the
 * SubscribeEvent call and detect the push in session_writer's notification
 * handler.  This function polls session_writer_snc_changed() every 3 seconds
 * (just checking a flag — no BLE RPC needed) and pulls the spool when the
 * flag is set.
 *
 * IMPORTANT — when waiting is pointless:
 * The AS11 only writes the Summary record for a noon-day period *after that
 * period closes*, at the following noon.  It never publishes a partial record
 * for the in-progress day.  Verified against device logs: a session ending
 * 06:45 on Aug 5 (period 20260804, closing at noon Aug 5) saw the AS11 return
 * only the already-closed 20260803 record, even after an _SNC push; the
 * 20260804 record first appeared in a pull made after noon that same day.
 *
 * Since the period containing the session end is by definition still open at
 * session end, retrying cannot make the record appear — it would just burn
 * 120 s and delay EDF generation and upload.  So we check first and return
 * immediately when the period is still open.  The STR generator handles this
 * by synthesizing the current-day record from session data, and the next
 * session after the noon rollover picks up the AS11-authoritative record via
 * the 30-day lookback pull and regenerates STR.edf.
 *
 * Fallback: if no _SNC notification arrives within 2 minutes (e.g.
 * subscription wasn't accepted, or BLE dropped the notification), we
 * pull the spool blindly on the last attempt and proceed with available
 * data.
 *
 * Returns true if the spool became fresh, false if still stale after timeout. */
bool post_therapy_wait_spool_current(int64_t end_epoch_ms, int64_t clock_drift_ms)
{
    const int max_attempts = 40;
    const int retry_delay_ms = 3000;

    /* clock_drift_ms = NTP - AS11, so AS11 = NTP - drift. */
    int64_t as11_end_ms = end_epoch_ms - clock_drift_ms;
    int64_t as11_now_ms = (int64_t)time(NULL) * 1000 - clock_drift_ms;
    int64_t period_end_ms = noon_period_end(as11_end_ms);
    if (as11_now_ms < period_end_ms) {
        ESP_LOGI(TAG, "spool_refresh: noon-day period still open "
                 "(closes in %lld s) — AS11 has not written this day's record "
                 "yet and will not until then; skipping wait",
                 (long long)((period_end_ms - as11_now_ms) / 1000));
        return false;
    }

    /* Clear any stale _SNC flag from before this session's TherapyStop. */
    session_writer_snc_changed(NULL);

    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));

        /* Check if _SNC ValueChange notification was received (push from AS11). */
        int64_t snc_val = 0;
        if (session_writer_snc_changed(&snc_val)) {
            ESP_LOGI(TAG, "spool_refresh: _SNC ValueChange received (%lld) "
                     "on attempt %d, pulling spool",
                     (long long)snc_val, attempt);
        } else if (attempt < max_attempts) {
            ESP_LOGD(TAG, "spool_refresh: no _SNC change yet (attempt %d/%d)",
                     attempt, max_attempts);
            continue;  /* keep waiting for the push notification */
        } else {
            ESP_LOGW(TAG, "spool_refresh: no _SNC notification after %d attempts, "
                     "pulling spool as fallback", attempt);
        }

        ESP_LOGI(TAG, "spool_refresh: pulling spool (attempt %d/%d)",
                 attempt, max_attempts);

        esp_err_t ret = refresh_today_summary_spool(end_epoch_ms, clock_drift_ms);
        if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "spool_refresh: BLE session lost, stopping");
            return false;
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "spool_refresh: pull failed, will retry");
            continue;
        }

        if (summary_spool_is_current(end_epoch_ms, clock_drift_ms)) {
            ESP_LOGI(TAG, "spool_refresh: spool is CURRENT after %d attempts",
                     attempt);
            return true;
        }
    }

    ESP_LOGW(TAG, "spool_refresh: spool still STALE after %d attempts (%d s), "
             "proceeding with available data", max_attempts,
             max_attempts * retry_delay_ms / 1000);
    return false;
}
