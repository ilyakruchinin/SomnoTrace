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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
    write_json_file(path, ident);
    cJSON_Delete(ident);
    return ESP_OK;
}

static esp_err_t collect_settings(const char *dir, const char *prefix)
{
    cJSON *settings = as11_ble_get_values(
        SETTINGS_KEYS, sizeof(SETTINGS_KEYS) / sizeof(SETTINGS_KEYS[0]));
    if (!settings) {
        ESP_LOGW(TAG, "failed to get device settings");
        return ESP_FAIL;
    }

    /* The device reports its own UTC offset here; capture it so Summary
     * PeriodStart values can be mapped onto the AS11's noon-day boundaries
     * even before any spool record has been parsed. */
    as11_offset_t as11_off;
    if (as11_time_offset_from_settings(settings, &as11_off)) {
        ESP_LOGI(TAG, "AS11 timezone offset %d min (from device settings)",
                 (int)(as11_off / 60));
    }

    char path[330];
    snprintf(path, sizeof(path), "%s/%s_settings.json", dir, prefix);
    write_json_file(path, settings);

    /* Also save to summaries directory for fast O(1) STR.edf lookup */
    const char *slash = strrchr(dir, '/');
    const char *day_label = slash ? slash + 1 : dir;
    if (day_label && strlen(day_label) == 8) {
        char sum_settings_path[300];
        snprintf(sum_settings_path, sizeof(sum_settings_path), "%s/%s.settings.json",
                 SD_SUMMARIES_DIR, day_label);
        write_json_file(sum_settings_path, settings);
    }

    cJSON_Delete(settings);
    return ESP_OK;
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

    /* 3. Get device identification → <prefix>_ident.json */
    if (collect_identification(session_dir, file_prefix) != ESP_OK) {
        errors++;
    }

    /* 4. Get current settings → <prefix>_settings.json */
    if (collect_settings(session_dir, file_prefix) != ESP_OK) {
        errors++;
    }

    /* 5. Check if the current day's spool is fresh (updated after session end). */
    bool fresh = summary_spool_is_current(end_epoch_ms, clock_drift_ms);
    if (spool_current) *spool_current = fresh;

    /* Write manifest with clock_drift_ms for EDF generation */
    cJSON *manifest = cJSON_CreateObject();
    cJSON_AddStringToObject(manifest, "collection_time", from_dt);
    cJSON_AddNumberToObject(manifest, "clock_drift_ms", (double)clock_drift_ms);
    cJSON_AddNumberToObject(manifest, "errors", errors);
    cJSON_AddBoolToObject(manifest, "spool_current", fresh);
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
