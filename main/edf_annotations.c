/*
 * SomnoTrace - EDF+D annotation file generator (EVE.edf and CSL.edf)
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

#include "edf_annotations.h"

static const char *TAG = "edf_ann";

static const char *event_label_map_str(const char *ev_name, bool csl_mode)
{
    if (!ev_name) return NULL;
    if (csl_mode) {
        if (strcmp(ev_name, "CsrStart") == 0 || strcmp(ev_name, "CSRStart") == 0) {
            return "CSR Start";
        }
        if (strcmp(ev_name, "CsrEnd") == 0 || strcmp(ev_name, "CSREnd") == 0) {
            return "CSR End";
        }
        return NULL;
    }
    if (strcmp(ev_name, "HypopneaEnd") == 0)          return "Hypopnea";
    if (strcmp(ev_name, "CentralApneaEnd") == 0)      return "Central Apnea";
    if (strcmp(ev_name, "ObstructiveApneaEnd") == 0)  return "Obstructive Apnea";
    if (strcmp(ev_name, "ApneaEnd") == 0)             return "Apnea";
    if (strcmp(ev_name, "ReraEnd") == 0)              return "Arousal";
    return NULL;
}

typedef struct {
    int64_t onset_sec;
    int64_t dur_sec;
    const char *label;
} snt_event_t;

static esp_err_t generate_annotation_edf(const char *edf_path,
                                         const char *snt_path,
                                         int64_t session_start_ms, int64_t clock_drift_ms,
                                         const char *patient_id, const char *recording_id,
                                         const char *start_date, const char *start_time,
                                         bool csl_mode)
{
    char path[350];
    char tmp_path[380];
    strncpy(path, edf_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    size_t capacity = 128;
    size_t count = 0;
    snt_event_t *ev_list = malloc(capacity * sizeof(snt_event_t));
    if (!ev_list) {
        ESP_LOGE(TAG, "generate_annotation_edf: out of memory");
        return ESP_ERR_NO_MEM;
    }

    FILE *ef = fopen(snt_path, "r");
    if (ef) {
        char line[512];
        while (fgets(line, sizeof(line), ef)) {
            cJSON *msg = cJSON_Parse(line);
            if (!msg) continue;
            cJSON *params = cJSON_GetObjectItem(msg, "params");
            if (params) {
                cJSON *data_id = cJSON_GetObjectItem(params, "dataId");
                if (data_id && cJSON_IsString(data_id)) {
                    if (strcmp(data_id->valuestring, "TherapyEvents-RespiratoryEvents") == 0) {
                        cJSON *events = cJSON_GetObjectItem(params, "events");
                        if (events && cJSON_IsArray(events)) {
                            int n = cJSON_GetArraySize(events);
                            for (int i = 0; i < n; i++) {
                                cJSON *ev = cJSON_GetArrayItem(events, i);
                                if (!ev) continue;
                                cJSON *label_j = cJSON_GetObjectItem(ev, "event");
                                if (!label_j || !cJSON_IsString(label_j)) continue;

                                const char *label = event_label_map_str(label_j->valuestring, csl_mode);
                                if (!label) continue;
                                cJSON *rt_j = cJSON_GetObjectItem(ev, "reportTime");
                                if (!rt_j || !cJSON_IsString(rt_j)) continue;

                                int64_t as11_ts_ms = as11_time_parse_iso8601_ms(rt_j->valuestring);
                                if (as11_ts_ms < 0) continue;

                                int64_t dur_sec = 0;
                                int64_t backdate_sec = 0;
                                if (csl_mode) {
                                    cJSON *bd_j = cJSON_GetObjectItem(ev, "backdateSeconds");
                                    if (bd_j && cJSON_IsNumber(bd_j)) {
                                        backdate_sec = (int64_t)bd_j->valuedouble;
                                        if (backdate_sec < 0) backdate_sec = 0;
                                    }
                                    /* CSL.edf uses zero-duration boundary markers */
                                    dur_sec = 0;
                                } else {
                                    cJSON *dur_j = cJSON_GetObjectItem(ev, "durationSeconds");
                                    if (dur_j && cJSON_IsNumber(dur_j)) {
                                        dur_sec = dur_j->valueint;
                                        if (dur_sec < 0) dur_sec = 0;
                                    }
                                }

                                int64_t event_ntp_ms = as11_ts_ms + clock_drift_ms - (backdate_sec * 1000);
                                int64_t onset_sec = (event_ntp_ms - session_start_ms) / 1000;
                                if (onset_sec < 0) onset_sec = 0;

                                if (count >= capacity) {
                                    size_t new_cap = capacity * 2;
                                    snt_event_t *tmp = realloc(ev_list, new_cap * sizeof(snt_event_t));
                                    if (tmp) {
                                        ev_list = tmp;
                                        capacity = new_cap;
                                    } else {
                                        break;
                                    }
                                }
                                ev_list[count].onset_sec = onset_sec;
                                ev_list[count].dur_sec = dur_sec;
                                ev_list[count].label = label;
                                count++;
                            }
                        }
                    }
                }
            }
            cJSON_Delete(msg);
        }
        fclose(ef);
    } else {
        ESP_LOGW(TAG, "generate_annotation_edf: cannot open %s: %s", snt_path, strerror(errno));
    }

    /* Sort events by onset_sec */
    for (size_t i = 0; i + 1 < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (ev_list[j].onset_sec < ev_list[i].onset_sec) {
                snt_event_t tmp = ev_list[i];
                ev_list[i] = ev_list[j];
                ev_list[j] = tmp;
            }
        }
    }

    /* Deduplicate identical onset and label */
    size_t dedup_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (dedup_count > 0 &&
            ev_list[dedup_count - 1].onset_sec == ev_list[i].onset_sec &&
            strcmp(ev_list[dedup_count - 1].label, ev_list[i].label) == 0) {
            continue;
        }
        ev_list[dedup_count++] = ev_list[i];
    }
    if (dedup_count < count) {
        ESP_LOGI(TAG, "%s: deduplicated %zu duplicate events (%zu → %zu)",
                 csl_mode ? "CSL.edf" : "EVE.edf", count - dedup_count,
                 count, dedup_count);
    }
    count = dedup_count;

    int total_records = 1 + (int)count;

    ESP_LOGI(TAG, "%s: %d events from %s, %d total records",
             csl_mode ? "CSL.edf" : "EVE.edf", (int)count, snt_path, total_records);

    edf_signal_def_t eve_sigs[1];
    eve_sigs[0] = g_annotation_signal;
    eve_sigs[0].samples_per_record = 31;  /* 62 bytes / 2 bytes per sample */

    FILE *edf = edf_open_atomic_file(path, tmp_path, sizeof(tmp_path));
    if (!edf) {
        ESP_LOGE(TAG, "cannot create %s: %s", path, strerror(errno));
        free(ev_list);
        return ESP_FAIL;
    }

    int header_bytes = edf_write_header(edf, patient_id, recording_id,
                                        start_date, start_time,
                                        total_records, "0.00",
                                        "EDF+D", eve_sigs, 1);
    if (header_bytes < 0) {
        ESP_LOGE(TAG, "generate_annotation_edf: edf_write_header failed");
        edf_discard_atomic_file(edf, tmp_path);
        free(ev_list);
        return ESP_FAIL;
    }

    /* Record 0: "Recording starts" marker */
    {
        uint8_t payload[62];
        memset(payload, 0, 62);
        int p = 0;
        payload[p++] = '+';
        payload[p++] = '0';
        payload[p++] = 0x14;  /* TAL separator */
        payload[p++] = 0x14;  /* empty duration */
        payload[p++] = 0x00;  /* end of first TAL */
        payload[p++] = '+';
        payload[p++] = '0';
        payload[p++] = 0x15;  /* duration separator */
        payload[p++] = '0';
        payload[p++] = 0x14;  /* TAL separator */
        const char *label = "Recording starts";
        memcpy(payload + p, label, strlen(label));
        p += strlen(label);
        payload[p++] = 0x14;
        payload[p++] = 0x00;

        uint16_t crc = edf_crc16_ccitt(payload, 62);
        uint8_t crc_bytes[2] = { (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };
        if (!edf_write_all(edf, payload, 62) ||
            !edf_write_all(edf, crc_bytes, 2)) {
            ESP_LOGE(TAG, "%s: write failed at Recording starts record",
                     csl_mode ? "CSL.edf" : "EVE.edf");
            edf_discard_atomic_file(edf, tmp_path);
            free(ev_list);
            return ESP_FAIL;
        }
    }

    /* Subsequent records: one event per record */
    for (size_t i = 0; i < count; i++) {
        uint8_t payload[62];
        memset(payload, 0, 62);
        int p = 0;
        payload[p++] = '+';
        payload[p++] = '0';
        payload[p++] = 0x14;
        payload[p++] = 0x14;
        payload[p++] = 0x00;

        char onset_str[24];
        snprintf(onset_str, sizeof(onset_str), "+%lld", (long long)ev_list[i].onset_sec);
        memcpy(payload + p, onset_str, strlen(onset_str));
        p += strlen(onset_str);
        payload[p++] = 0x15;
        char dur_str[24];
        snprintf(dur_str, sizeof(dur_str), "%lld", (long long)ev_list[i].dur_sec);
        memcpy(payload + p, dur_str, strlen(dur_str));
        p += strlen(dur_str);
        payload[p++] = 0x14;
        size_t max_copy = 62 - p - 3;
        size_t label_len = strlen(ev_list[i].label);
        if (label_len > max_copy) label_len = max_copy;
        memcpy(payload + p, ev_list[i].label, label_len);
        p += label_len;
        payload[p++] = 0x14;
        payload[p++] = 0x00;

        uint16_t crc = edf_crc16_ccitt(payload, 62);
        uint8_t crc_bytes[2] = { (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };
        if (!edf_write_all(edf, payload, 62) ||
            !edf_write_all(edf, crc_bytes, 2)) {
            ESP_LOGE(TAG, "%s: write failed at event %zu",
                     csl_mode ? "CSL.edf" : "EVE.edf", i);
            edf_discard_atomic_file(edf, tmp_path);
            free(ev_list);
            return ESP_FAIL;
        }
    }

    free(ev_list);
    if (edf_finalize_atomic_file(edf, tmp_path, path) != ESP_OK) {
        ESP_LOGE(TAG, "cannot finalize %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "%s generated: %s (%d events)", csl_mode ? "CSL.edf" : "EVE.edf", path, (int)count);
    return ESP_OK;
}

esp_err_t edf_generate_eve_edf(const char *out_path, const char *events_snt_path,
                               int64_t session_start_ms, int64_t clock_drift_ms,
                               const char *patient_id, const char *recording_id,
                               const char *start_date, const char *start_time)
{
    return generate_annotation_edf(out_path, events_snt_path,
                                  session_start_ms, clock_drift_ms,
                                  patient_id, recording_id,
                                  start_date, start_time, false);
}

esp_err_t edf_generate_csl_edf(const char *out_path, const char *events_snt_path,
                               int64_t session_start_ms, int64_t clock_drift_ms,
                               const char *patient_id, const char *recording_id,
                               const char *start_date, const char *start_time)
{
    return generate_annotation_edf(out_path, events_snt_path,
                                  session_start_ms, clock_drift_ms,
                                  patient_id, recording_id,
                                  start_date, start_time, true);
}
