/*
 * SomnoTrace - SNT to EDF waveform conversion (BRP, PLD, SA2)
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

#include "edf_waveform.h"

static const char *TAG = "edf_wav";

/* ── SNT Reading Helpers ────────────────────────────────────────────── */

static uint32_t snt_available_samples(FILE *f, int channels_in_file)
{
    if (!f || channels_in_file <= 0) return UINT32_MAX;

    long cur = ftell(f);
    if (cur < 0) return UINT32_MAX;
    if (fseek(f, 0, SEEK_END) != 0) return UINT32_MAX;
    long end = ftell(f);
    if (fseek(f, cur, SEEK_SET) != 0 || end < 0) return UINT32_MAX;

    if (end <= (long)sizeof(snt_header_t)) return 0;
    long data_bytes = end - (long)sizeof(snt_header_t);
    long frame = (long)channels_in_file * (long)sizeof(int16_t);
    return (uint32_t)(data_bytes / frame);
}

/* ── Event Analysis & Gating ────────────────────────────────────────── */

int64_t edf_find_mask_on_time(const char *events_snt_path)
{
    FILE *f = fopen(events_snt_path, "r");
    if (!f) return -1;

    int64_t mask_on_ms = -1;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        cJSON *msg = cJSON_Parse(line);
        if (!msg) continue;
        cJSON *params = cJSON_GetObjectItem(msg, "params");
        if (params) {
            cJSON *events = cJSON_GetObjectItem(params, "events");
            if (events && cJSON_IsArray(events)) {
                int n = cJSON_GetArraySize(events);
                for (int i = 0; i < n; i++) {
                    cJSON *ev = cJSON_GetArrayItem(events, i);
                    if (!ev) continue;
                    cJSON *label = cJSON_GetObjectItem(ev, "event");
                    if (!label || !cJSON_IsString(label)) continue;
                    if (strcmp(label->valuestring, "MaskOn") == 0) {
                        cJSON *rt = cJSON_GetObjectItem(ev, "reportTime");
                        if (rt && cJSON_IsString(rt)) {
                            mask_on_ms = as11_time_parse_iso8601_ms(rt->valuestring);
                            cJSON_Delete(msg);
                            fclose(f);
                            return mask_on_ms;
                        }
                    }
                }
            }
        }
        cJSON_Delete(msg);
    }
    fclose(f);
    return mask_on_ms;
}

int64_t edf_find_mask_off_time(const char *events_snt_path)
{
    FILE *f = fopen(events_snt_path, "r");
    if (!f) return -1;

    int64_t mask_off_ms = -1;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        cJSON *msg = cJSON_Parse(line);
        if (!msg) continue;
        cJSON *params = cJSON_GetObjectItem(msg, "params");
        if (params) {
            cJSON *events = cJSON_GetObjectItem(params, "events");
            if (events && cJSON_IsArray(events)) {
                int n = cJSON_GetArraySize(events);
                for (int i = 0; i < n; i++) {
                    cJSON *ev = cJSON_GetArrayItem(events, i);
                    if (!ev) continue;
                    cJSON *label = cJSON_GetObjectItem(ev, "event");
                    if (!label || !cJSON_IsString(label)) continue;
                    if (strcmp(label->valuestring, "MaskOff") == 0) {
                        cJSON *rt = cJSON_GetObjectItem(ev, "reportTime");
                        if (rt && cJSON_IsString(rt)) {
                            mask_off_ms = as11_time_parse_iso8601_ms(rt->valuestring);
                        }
                    }
                }
            }
        }
        cJSON_Delete(msg);
    }
    fclose(f);
    return mask_off_ms;
}

int64_t edf_find_zle_edge_time(const char *events_snt_path, int want_value,
                               int64_t clock_drift_ms)
{
    FILE *f = fopen(events_snt_path, "r");
    if (!f) return -1;

    int64_t zle_ms = -1;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        cJSON *msg = cJSON_Parse(line);
        if (!msg) continue;
        cJSON *params = cJSON_GetObjectItem(msg, "params");
        if (params) {
            cJSON *data_id = cJSON_GetObjectItem(params, "dataId");
            if (data_id && cJSON_IsString(data_id) &&
                strcmp(data_id->valuestring, "_ZLE") == 0) {
                cJSON *events = cJSON_GetObjectItem(params, "events");
                if (events && cJSON_IsArray(events)) {
                    int n = cJSON_GetArraySize(events);
                    for (int i = 0; i < n; i++) {
                        cJSON *ev = cJSON_GetArrayItem(events, i);
                        if (!ev) continue;
                        cJSON *val = cJSON_GetObjectItem(ev, "value");
                        if (val && cJSON_IsNumber(val) &&
                            (int)val->valuedouble == want_value) {
                            int64_t cand = -1;
                            cJSON *rt = cJSON_GetObjectItem(ev, "reportTime");
                            if (rt && cJSON_IsString(rt)) {
                                int64_t as11_ms =
                                    as11_time_parse_iso8601_ms(rt->valuestring);
                                if (as11_ms > 0)
                                    cand = as11_ms + clock_drift_ms;
                            }
                            if (cand < 0) {
                                cJSON *ntp = cJSON_GetObjectItem(ev, "ntpTimeMs");
                                if (ntp && cJSON_IsNumber(ntp))
                                    cand = (int64_t)ntp->valuedouble;
                            }
                            if (cand > 0) {
                                zle_ms = cand;
                                if (want_value == 1) {
                                    cJSON_Delete(msg);
                                    fclose(f);
                                    return zle_ms;
                                }
                            }
                        }
                    }
                }
            }
        }
        cJSON_Delete(msg);
    }
    fclose(f);
    return zle_ms;
}

/* ════════════════════════════════════════════════════════════════════
 *  SNT to EDF Conversion
 * ════════════════════════════════════════════════════════════════════ */

esp_err_t edf_convert_snt_to_edf(const char *snt_path, const char *edf_path,
                                 const char *patient_id, const char *recording_id,
                                 const char *start_date, const char *start_time,
                                 const edf_signal_def_t *signals, int n_signals,
                                 const char *record_dur,
                                 const int *channel_map,
                                 int skip_records, int max_records,
                                 const char *second_snt_path)
{
    uint32_t skip_samples = (uint32_t)skip_records;
    uint32_t max_samples = (uint32_t)max_records;
    const char *snt_path2 = second_snt_path;

    FILE *snt = fopen(snt_path, "rb");
    if (!snt) {
        ESP_LOGW(TAG, "cannot open %s: %s", snt_path, strerror(errno));
        return ESP_FAIL;
    }

    FILE *snt2 = NULL;
    if (snt_path2) {
        snt2 = fopen(snt_path2, "rb");
        if (!snt2) {
            ESP_LOGW(TAG, "cannot open %s: %s", snt_path2, strerror(errno));
            fclose(snt);
            return ESP_FAIL;
        }
    }

    snt_header_t hdr;
    if (snt_read_header(snt, &hdr) != ESP_OK) {
        fclose(snt);
        if (snt2) fclose(snt2);
        return ESP_FAIL;
    }

    if (snt2) {
        snt_header_t hdr2;
        if (snt_read_header(snt2, &hdr2) != ESP_OK) {
            ESP_LOGW(TAG, "cannot read header from %s", snt_path2);
            fclose(snt); fclose(snt2);
            return ESP_FAIL;
        }
        if (hdr.n_channels != 1 || hdr2.n_channels != 1) {
            ESP_LOGE(TAG, "v2 pair mode requires 1-ch files: %s has %d ch, %s has %d ch",
                     snt_path, hdr.n_channels, snt_path2, hdr2.n_channels);
            fclose(snt); fclose(snt2);
            return ESP_FAIL;
        }
        hdr.n_channels = 2;
    }

    int snt_channels = hdr.n_channels;
    int16_t snt_missing = snt_missing_for(hdr.version);
    if (!is_channel_map_valid(channel_map, n_signals, snt_channels)) {
        ESP_LOGE(TAG, "%s: channel mapping invalid: snt_channels=%d n_signals=%d",
                 snt_path, snt_channels, n_signals);
        fclose(snt);
        if (snt2) fclose(snt2);
        return ESP_FAIL;
    }

    int hz_x10 = hdr.sample_hz_x10;
    int *spr = malloc(n_signals * sizeof(int));
    edf_signal_def_t *sig = malloc(n_signals * sizeof(edf_signal_def_t));
    if (!spr || !sig) {
        ESP_LOGE(TAG, "malloc spr/sig failed");
        free(spr); free(sig);
        fclose(snt);
        if (snt2) fclose(snt2);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < n_signals; i++) {
        spr[i] = (hz_x10 * 60) / 10;
        sig[i] = signals[i];
        sig[i].samples_per_record = spr[i];
    }

    uint32_t total_samples = hdr.sample_count;
    {
        uint32_t avail = snt_available_samples(snt, snt2 ? 1 : snt_channels);
        if (snt2) {
            uint32_t avail2 = snt_available_samples(snt2, 1);
            if (avail2 < avail) avail = avail2;
        }
        if (avail < total_samples) {
            ESP_LOGW(TAG, "%s: header claims %u samples but only %u are on disk "
                     "(torn tail from an interrupted session) — exporting %u",
                     snt_path, (unsigned)total_samples, (unsigned)avail, (unsigned)avail);
            total_samples = avail;
        }
    }

    if (skip_samples > total_samples) {
        ESP_LOGW(TAG, "%s: skip_samples (%u) > sample_count (%u), clamping",
                 snt_path, (unsigned)skip_samples, (unsigned)total_samples);
        skip_samples = total_samples;
    }
    total_samples -= skip_samples;

    if (max_samples > 0 && max_samples < total_samples) {
        ESP_LOGI(TAG, "%s: truncating %u → %u samples (MaskOff end-trim)",
                 snt_path, (unsigned)total_samples, (unsigned)max_samples);
        total_samples = max_samples;
    }
    int total_records = (int)(total_samples / spr[0]);
    if (total_records < 0) total_records = 0;

    if (total_records == 0) {
        ESP_LOGI(TAG, "%s: short session (%u samples < %d spr), writing header-only EDF",
                 snt_path, (unsigned)total_samples, spr[0]);
        char tmp_path[380];
        FILE *edf = edf_open_atomic_file(edf_path, tmp_path, sizeof(tmp_path));
        if (!edf) {
            ESP_LOGE(TAG, "cannot create %s: %s", edf_path, strerror(errno));
            free(spr); free(sig);
            fclose(snt);
            if (snt2) fclose(snt2);
            return ESP_FAIL;
        }
        if (edf_write_header(edf, patient_id, recording_id,
                             start_date, start_time,
                             0, record_dur, "EDF", sig, n_signals) < 0 ||
            edf_finalize_atomic_file(edf, tmp_path, edf_path) != ESP_OK) {
            ESP_LOGE(TAG, "cannot write %s: %s", edf_path, strerror(errno));
            free(spr); free(sig);
            fclose(snt);
            if (snt2) fclose(snt2);
            return ESP_FAIL;
        }
        free(spr); free(sig);
        fclose(snt);
        if (snt2) fclose(snt2);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "converting %s → %s: %u samples (skip %u), %d records, %d ch",
             snt_path, edf_path, (unsigned)total_samples, (unsigned)skip_samples,
             total_records, n_signals);

    if (skip_samples > 0) {
        int primary_channels = snt2 ? 1 : snt_channels;
        long skip_bytes = (long)skip_samples * primary_channels * sizeof(int16_t);
        if (fseek(snt, sizeof(snt_header_t) + skip_bytes, SEEK_SET) != 0) {
            ESP_LOGW(TAG, "%s: fseek skip failed, starting from beginning", snt_path);
            fseek(snt, sizeof(snt_header_t), SEEK_SET);
        }
        if (snt2) {
            long skip2 = (long)skip_samples * sizeof(int16_t);
            if (fseek(snt2, sizeof(snt_header_t) + skip2, SEEK_SET) != 0) {
                ESP_LOGW(TAG, "%s: fseek skip failed, starting from beginning", snt_path2);
                fseek(snt2, sizeof(snt_header_t), SEEK_SET);
            }
        }
    }

    char tmp_path[380];
    FILE *edf = edf_open_atomic_file(edf_path, tmp_path, sizeof(tmp_path));
    if (!edf) {
        ESP_LOGE(TAG, "cannot create %s: %s", edf_path, strerror(errno));
        fclose(snt);
        if (snt2) fclose(snt2);
        free(spr); free(sig);
        return ESP_FAIL;
    }

    int header_bytes = edf_write_header(edf, patient_id, recording_id,
                                        start_date, start_time,
                                        total_records, record_dur,
                                        "EDF", sig, n_signals);
    if (header_bytes < 0) {
        ESP_LOGE(TAG, "edf_write_header failed for %s", edf_path);
        free(spr); free(sig);
        edf_discard_atomic_file(edf, tmp_path);
        fclose(snt);
        if (snt2) fclose(snt2);
        return ESP_FAIL;
    }

    int samples_per_record = spr[0];
    int record_data_samples = samples_per_record * n_signals;
    size_t record_bytes = record_data_samples * sizeof(int16_t);
    size_t raw_record_bytes = samples_per_record * snt_channels * sizeof(int16_t);

    int16_t *raw = malloc(raw_record_bytes);
    int16_t *record_buf = heap_caps_malloc(record_bytes, MALLOC_CAP_SPIRAM);
    if (!record_buf) {
        record_buf = malloc(record_bytes);
    }
    if (!raw || !record_buf) {
        ESP_LOGE(TAG, "malloc record buffers failed");
        free(raw); free(record_buf); free(spr); free(sig);
        edf_discard_atomic_file(edf, tmp_path);
        fclose(snt);
        if (snt2) fclose(snt2);
        return ESP_ERR_NO_MEM;
    }

    for (int rec = 0; rec < total_records; rec++) {
        memset(raw, 0, raw_record_bytes);
        size_t avail = raw_record_bytes;
        if (rec == total_records - 1) {
            uint32_t remaining = total_samples - (uint32_t)rec * spr[0];
            if (remaining < (uint32_t)samples_per_record) {
                avail = remaining * snt_channels * sizeof(int16_t);
            }
        }
        if (avail > 0) {
            if (snt2) {
                int n_samp = (int)(avail / (snt_channels * sizeof(int16_t)));
                size_t ch_bytes = (size_t)n_samp * sizeof(int16_t);
                int16_t *flow_buf = malloc(ch_bytes);
                int16_t *press_buf = malloc(ch_bytes);
                if (!flow_buf || !press_buf) {
                    ESP_LOGW(TAG, "v2 interleave malloc failed at record %d", rec);
                    free(flow_buf); free(press_buf);
                    free(raw); free(record_buf); free(spr); free(sig);
                    edf_discard_atomic_file(edf, tmp_path);
                    fclose(snt); fclose(snt2);
                    return ESP_FAIL;
                }
                if (fread(flow_buf, 1, ch_bytes, snt) != ch_bytes ||
                    fread(press_buf, 1, ch_bytes, snt2) != ch_bytes) {
                    ESP_LOGW(TAG, "v2 short read at record %d", rec);
                    free(flow_buf); free(press_buf);
                    free(raw); free(record_buf); free(spr); free(sig);
                    edf_discard_atomic_file(edf, tmp_path);
                    fclose(snt); fclose(snt2);
                    return ESP_FAIL;
                }
                for (int s = 0; s < n_samp; s++) {
                    raw[s * 2]     = flow_buf[s];
                    raw[s * 2 + 1] = press_buf[s];
                }
                free(flow_buf); free(press_buf);
            } else {
                if (fread(raw, 1, avail, snt) != avail) {
                    ESP_LOGW(TAG, "short read at record %d", rec);
                    free(raw); free(record_buf); free(spr); free(sig);
                    edf_discard_atomic_file(edf, tmp_path);
                    fclose(snt);
                    return ESP_FAIL;
                }
            }
        }

        int avail_samples = (int)(avail / (snt_channels * sizeof(int16_t)));
        for (int ch = 0; ch < n_signals; ch++) {
            int snt_ch = channel_map ? channel_map[ch] : ch;
            double pmin = sig[ch].phys_min;
            double pspan = sig[ch].phys_max - sig[ch].phys_min;
            double dmin = sig[ch].dig_min;
            double dspan = sig[ch].dig_max - sig[ch].dig_min;
            double k = (pspan != 0.0) ? (dspan / pspan) : 0.0;
            bool passthrough = sig[ch].invalid_passthrough;
            for (int s = 0; s < samples_per_record; s++) {
                if (s < avail_samples) {
                    int16_t stored = raw[s * snt_channels + snt_ch];
                    if (passthrough && stored == snt_missing) {
                        record_buf[ch * samples_per_record + s] = -1;
                        continue;
                    }
                    double phys = stored / 100.0;
                    double dig = dmin + (phys - pmin) * k;
                    int idig = (int)(dig < 0 ? dig - 0.5 : dig + 0.5);
                    if (!passthrough) {
                        idig = clamp_i16(idig, sig[ch].dig_min, sig[ch].dig_max);
                    } else {
                        idig = clamp_i16(idig, INT16_MIN, INT16_MAX);
                    }
                    record_buf[ch * samples_per_record + s] = (int16_t)idig;
                } else {
                    record_buf[ch * samples_per_record + s] = passthrough ? -1 : 0;
                }
            }
        }

        if (!edf_write_all(edf, record_buf, record_bytes)) {
            free(raw); free(record_buf); free(spr); free(sig);
            edf_discard_atomic_file(edf, tmp_path);
            fclose(snt);
            if (snt2) fclose(snt2);
            return ESP_FAIL;
        }

        uint16_t crc = edf_crc16_ccitt((uint8_t *)record_buf, record_bytes);
        int16_t crc_val = (int16_t)crc;
        if (!edf_write_all(edf, &crc_val, sizeof(crc_val))) {
            free(raw); free(record_buf); free(spr); free(sig);
            edf_discard_atomic_file(edf, tmp_path);
            fclose(snt);
            if (snt2) fclose(snt2);
            return ESP_FAIL;
        }
    }

    free(raw);
    free(record_buf);
    free(spr);
    free(sig);

    if (edf_finalize_atomic_file(edf, tmp_path, edf_path) != ESP_OK) {
        ESP_LOGE(TAG, "cannot finalize %s: %s", edf_path, strerror(errno));
        fclose(snt);
        if (snt2) fclose(snt2);
        return ESP_FAIL;
    }
    fclose(snt);
    if (snt2) fclose(snt2);
    ESP_LOGI(TAG, "EDF conversion complete: %s", edf_path);
    return ESP_OK;
}
