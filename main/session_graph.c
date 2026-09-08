/*
 * SomnoTrace - Session graph data HTTP endpoint
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

#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "sd_storage.h"
#include "edf_gen.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "psram_task.h"

static const char *TAG = "session_graph";

#define SNT_MAGIC  0x534E5442u
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  tier;
    uint8_t  n_channels;
    uint8_t  sample_bytes;
    uint16_t sample_hz_x10;
    uint16_t reserved;
    int64_t  start_epoch_ms;
    uint32_t sample_count;
    uint32_t reserved2;
} snt_hdr_t;

#define SNT_HDR_SIZE sizeof(snt_hdr_t)   /* 28 bytes (packed) */
#define GRAPH_MAX_POINTS 50000  /* upper bound for ?points=all */

/* ── Physical-unit scaling factors ────────────────────────────────────
 * Raw .snt files store int16 digital values.  These scale factors convert
 * digital values to physical units matching the EDF signal definitions in
 * edf_gen.c.  The graph API emits pre-scaled physical values so the
 * frontend can display them directly without additional conversion.
 *
 * Formula: phys = (dig - dig_min) * (phys_max - phys_min)
 *                                  / (dig_max - dig_min)  + phys_min
 * For all channels below dig_min=0 simplifies to: phys = dig * scale.
 * Flow is the exception: dig_min=-1000, phys_min=-2.0.
 */

/* BRP: Flow.40ms  — phys [-2.0, 3.0] dig [-1000, 1500] → 0.002 L/s per digit
 *      Press.40ms — phys [0.0, 40.0]  dig [0, 2000]     → 0.02 cmH2O per digit */
#define BRP_FLOW_SCALE   0.002f    /* L/s per digit */
#define BRP_FLOW_OFFSET  (-2.0f)   /* phys_min */
#define BRP_FLOW_DIG_MIN (-1000)   /* dig_min */
#define BRP_PRESS_SCALE  0.02f     /* cmH2O per digit */

/* PLD ch3: Leak — phys [0.0, 2.0] dig [0, 100] → 0.02 L/s per digit */
#define PLD_LEAK_SCALE   0.02f     /* L/s per digit */

/* URL-decode a query parameter value.  Returns decoded length or -1 on overflow. */
static int graph_url_decode(char *dst, const char *src, int max_len)
{
    int i = 0;
    while (*src && i < max_len - 1) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], '\0' };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    if (i >= max_len) return -1;
    dst[i] = '\0';
    return i;
}

/* Extract a query parameter from the request.
 * Tries httpd_req_get_url_query_str first, then falls back to req->uri.
 *
 * The old implementation had two bugs fixed here:
 * 1. vlen > 1 rejected single-character values (e.g. start_ms with short
 *    numeric strings).  Changed to vlen >= 1.
 * 2. strstr(qbuf, "key=") could match a suffix of another key (e.g.
 *    "some_start_ms=" when searching for "start_ms=").  Now verifies that
 *    the match is at the start of the string or preceded by '&'. */
static bool get_qparam(httpd_req_t *req, const char *key, char *out, int out_len)
{
    const char *uri = req->uri;
    int klen = strlen(key);

    /* First try the official API */
    char qbuf[512];
    int qlen = httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf));
    if (qlen > 0) {
        char key_eq[48];
        snprintf(key_eq, sizeof(key_eq), "%s=", key);
        char *p = qbuf;
        while ((p = strstr(p, key_eq)) != NULL) {
            /* Verify word boundary: must be at start of string or preceded by '&' */
            if (p != qbuf && *(p - 1) != '&') {
                p += klen + 1;
                continue;
            }
            p += klen + 1;
            char *end = strchr(p, '&');
            int vlen = end ? (int)(end - p) : (int)strlen(p);
            if (vlen >= 1 && vlen < out_len) {
                graph_url_decode(out, p, out_len);
                return true;
            }
            break;
        }
    }

    /* Fallback: parse req->uri directly */
    if (uri) {
        const char *qmark = strchr(uri, '?');
        if (qmark) {
            const char *p = qmark + 1;
            while (*p) {
                const char *next_amp = strchr(p, '&');
                int param_len = next_amp ? (int)(next_amp - p) : (int)strlen(p);
                if (param_len > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
                    int vlen = param_len - klen - 1;
                    if (vlen >= 1 && vlen < out_len) {
                        memcpy(out, p + klen + 1, vlen);
                        out[vlen] = '\0';
                        graph_url_decode(out, out, out_len);
                        return true;
                    }
                }
                if (!next_amp) break;
                p = next_amp + 1;
            }
        }
    }

    return false;
}

/* ── Physical-unit conversion helpers ────────────────────────────────
 * Convert raw int16 digital values (stored multiplied by 100) to physical units.
 * Returns the scaled float value, or 0.0 for invalid markers.
 * The sentinel is determined by SNT file version: v1 uses -1, v2 uses INT16_MIN. */
static inline int16_t snt_missing_for(uint8_t version)
{
    return (version >= 2) ? INT16_MIN : -1;
}

static inline float brp_flow_phys(int16_t dig, int16_t missing)
{
    if (dig == missing) return 0.0f;
    /* raw: 100 * L/s -> L/s is dig/100.0. convert L/s to L/min by * 60.0 -> dig * 0.6f */
    return (float)dig * 0.6f;
}

static inline float brp_press_phys(int16_t dig, int16_t missing)
{
    if (dig == missing) return 0.0f;
    /* raw: 100 * cmH2O -> return dig/100.0f */
    return (float)dig * 0.01f;
}

static inline float pld_leak_phys(int16_t dig, int16_t missing)
{
    if (dig == missing) return 0.0f;
    /* raw: 100 * L/s -> return L/min by * 60.0 -> dig * 0.6f */
    return (float)dig * 0.6f;
}

/* GET /api/sessions?date=YYYYMMDD
 * Returns JSON array of terminal sessions for the given noon-day folder,
 * sorted by start_epoch_ms, with metadata including clock drift and file sizes. */
typedef struct {
    char id[32];
    char state[16];             /* manifest state, reported as-is            */
    bool partial;               /* ended abnormally: data is real but short  */
    int64_t start_epoch_ms;
    int64_t end_epoch_ms;
    int64_t clock_drift_ms;
    bool clock_drift_valid;
    uint32_t brp_samples;
    uint32_t brp_mm_samples;
    uint32_t pld_samples;
    long brp_bytes;
    long brp_mm_bytes;
    int fmt;                    /* session format: 2=flow/press split, 1=legacy brp */
    uint32_t stream_notifications;  /* BLE notifications received */
    uint32_t gap_events;            /* gap detections (compensated) */
    uint32_t gap_missing;           /* total missing notifications compensated */
} session_manifest_t;

static int compare_session_start(const void *a, const void *b)
{
    const session_manifest_t *sa = (const session_manifest_t *)a;
    const session_manifest_t *sb = (const session_manifest_t *)b;
    if (sa->start_epoch_ms < sb->start_epoch_ms) return -1;
    if (sa->start_epoch_ms > sb->start_epoch_ms) return 1;
    return 0;
}

static long file_size_stat(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return st.st_size;
    return 0;
}

esp_err_t sessions_list_handler(httpd_req_t *req)
{
    char date[16];
    if (!get_qparam(req, "date", date, sizeof(date))) {
        ESP_LOGW(TAG, "sessions_list: missing date parameter");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing date parameter");
        return ESP_FAIL;
    }
    if (strlen(date) != 8) {
        ESP_LOGW(TAG, "sessions_list: invalid date length=%d value='%s'", (int)strlen(date), date);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid date length");
        return ESP_FAIL;
    }

    char day_dir[300];
    snprintf(day_dir, sizeof(day_dir), "%s/%s", SD_STREAMS_DIR, date);

    DIR *dd = opendir(day_dir);
    if (!dd) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "[]", 2);
        return ESP_OK;
    }

    /* Collect completed sessions into an array for sorting */
    int cap = 8, count = 0;
    session_manifest_t *sessions = heap_caps_calloc(cap, sizeof(session_manifest_t), MALLOC_CAP_SPIRAM);
    if (!sessions) sessions = calloc(cap, sizeof(session_manifest_t));
    if (!sessions) { closedir(dd); httpd_resp_send_500(req); return ESP_FAIL; }

    struct dirent *fent;
    while ((fent = readdir(dd)) != NULL) {
        const char *suffix = "_session.json";
        int slen = strlen(suffix);
        int flen = strlen(fent->d_name);
        if (flen <= slen || strcmp(fent->d_name + flen - slen, suffix) != 0)
            continue;

        char session_id[32];
        int plen = flen - slen;
        if (plen <= 0 || plen >= (int)sizeof(session_id)) continue;
        memcpy(session_id, fent->d_name, plen);
        session_id[plen] = '\0';

        char jpath[600];
        snprintf(jpath, sizeof(jpath), "%s/%s", day_dir, fent->d_name);
        FILE *f = fopen(jpath, "r");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long fsz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsz <= 0 || fsz > 4096) { fclose(f); continue; }
        char *buf = heap_caps_malloc((size_t)fsz + 1, MALLOC_CAP_SPIRAM);
        if (!buf) buf = malloc((size_t)fsz + 1);
        if (!buf) { fclose(f); continue; }
        fread(buf, 1, fsz, f);
        buf[fsz] = '\0';
        fclose(f);

        cJSON *j = cJSON_Parse(buf);
        free(buf);
        if (!j) continue;

        cJSON *j_st = cJSON_GetObjectItem(j, "start_epoch_ms");
        cJSON *j_en = cJSON_GetObjectItem(j, "end_epoch_ms");
        cJSON *j_state = cJSON_GetObjectItem(j, "state");
        cJSON *j_brp = cJSON_GetObjectItem(j, "brp_samples");
        cJSON *j_brp_mm = cJSON_GetObjectItem(j, "brp_mm_samples");
        cJSON *j_pld = cJSON_GetObjectItem(j, "pld_samples");
        cJSON *j_drift = cJSON_GetObjectItem(j, "clock_drift_ms");
        cJSON *j_drift_valid = cJSON_GetObjectItem(j, "clock_drift_valid");
        cJSON *j_fmt = cJSON_GetObjectItem(j, "fmt");
        cJSON *j_notif = cJSON_GetObjectItem(j, "stream_notifications");
        cJSON *j_gaps = cJSON_GetObjectItem(j, "gap_events");
        cJSON *j_missing = cJSON_GetObjectItem(j, "gap_missing");

        if (!j_st || !cJSON_IsNumber(j_st)) { cJSON_Delete(j); continue; }
        if ((int64_t)j_st->valuedouble < 946684800000LL) { cJSON_Delete(j); continue; }

        /* Which sessions the Web UI may show.
         *
         * This used to accept only "completed", which meant a night that ended
         * in a crash was exported to EDF (and uploaded to SleepHQ/SMB) while
         * being invisible in our own UI — the graphs showed less than the files
         * we published.  The states below are all *terminal* and have real
         * captured data, so they are shown and flagged as partial:
         *
         *   interrupted  reconstructed by crash recovery
         *   timed_out    stream went silent, session closed by the watchdog
         *   rotated      superseded by a new session
         *   split        closed at a day boundary
         *
         * Still excluded: a session that is currently recording (its manifest
         * is not final, and its .snt files are being appended to while the
         * graph would be reading them), and "storage_failed", where the raw
         * data itself is not trustworthy. */
        const char *state = (j_state && cJSON_IsString(j_state))
                          ? j_state->valuestring : "unknown";
        bool completed   = (strcmp(state, "completed") == 0);
        bool partial     = (strcmp(state, "interrupted") == 0 ||
                            strcmp(state, "timed_out") == 0 ||
                            strcmp(state, "rotated") == 0 ||
                            strcmp(state, "split") == 0);
        if (!completed && !partial) { cJSON_Delete(j); continue; }

        if (count >= cap) {
            cap *= 2;
            session_manifest_t *tmp = realloc(sessions, cap * sizeof(session_manifest_t));
            if (!tmp) { cJSON_Delete(j); break; }
            sessions = tmp;
        }

        session_manifest_t *s = &sessions[count];
        memset(s, 0, sizeof(*s));
        strlcpy(s->id, session_id, sizeof(s->id));
        strlcpy(s->state, state, sizeof(s->state));
        s->partial = partial;
        s->start_epoch_ms = (int64_t)j_st->valuedouble;
        s->end_epoch_ms = j_en ? (int64_t)j_en->valuedouble : 0;
        s->brp_samples = j_brp ? (uint32_t)j_brp->valuedouble : 0;
        s->brp_mm_samples = j_brp_mm ? (uint32_t)j_brp_mm->valuedouble : 0;
        s->pld_samples = j_pld ? (uint32_t)j_pld->valuedouble : 0;
        s->clock_drift_ms = j_drift ? (int64_t)j_drift->valuedouble : 0;
        s->clock_drift_valid = j_drift_valid ? cJSON_IsTrue(j_drift_valid) : (j_drift != NULL);
        s->fmt = j_fmt ? (int)j_fmt->valuedouble : 1;  /* default v1 if absent */
        s->stream_notifications = j_notif ? (uint32_t)j_notif->valuedouble : 0;
        s->gap_events = j_gaps ? (uint32_t)j_gaps->valuedouble : 0;
        s->gap_missing = j_missing ? (uint32_t)j_missing->valuedouble : 0;

        cJSON_Delete(j);

        /* Stat the .snt files for sizes.
         * v2: flow.snt + flow_mm.snt; v1 (backwards compat): brp.snt + brp_mm.snt */
        char fpath[600];
        if (s->fmt >= 2) {
            snprintf(fpath, sizeof(fpath), "%s/%s_flow.snt", day_dir, session_id);
            s->brp_bytes = file_size_stat(fpath);
            snprintf(fpath, sizeof(fpath), "%s/%s_flow_mm.snt", day_dir, session_id);
            s->brp_mm_bytes = file_size_stat(fpath);
        } else {
            snprintf(fpath, sizeof(fpath), "%s/%s_brp.snt", day_dir, session_id);
            s->brp_bytes = file_size_stat(fpath);
            snprintf(fpath, sizeof(fpath), "%s/%s_brp_mm.snt", day_dir, session_id);
            s->brp_mm_bytes = file_size_stat(fpath);
        }

        count++;
    }
    closedir(dd);

    /* Sort by start_epoch_ms */
    if (count > 1)
        qsort(sessions, count, sizeof(session_manifest_t), compare_session_start);

    /* Build JSON response with dynamic buffer */
    int json_cap = count * 400 + 64;
    char *json = heap_caps_malloc(json_cap, MALLOC_CAP_SPIRAM);
    if (!json) { free(sessions); httpd_resp_send_500(req); return ESP_FAIL; }
    int pos = 0;
    pos += snprintf(json + pos, json_cap - pos, "[");

    for (int i = 0; i < count; i++) {
        session_manifest_t *s = &sessions[i];
        if (pos > json_cap - 400) {
            json_cap *= 2;
            char *tmp = heap_caps_realloc(json, json_cap, MALLOC_CAP_SPIRAM);
            if (!tmp) { free(json); free(sessions); httpd_resp_send_500(req); return ESP_FAIL; }
            json = tmp;
        }
        if (i > 0) pos += snprintf(json + pos, json_cap - pos, ",");
        pos += snprintf(json + pos, json_cap - pos,
            "{\"id\":\"%s\",\"state\":\"%s\",\"partial\":%s,"
            "\"start_epoch_ms\":%lld,\"end_epoch_ms\":%lld,"
            "\"clock_drift_ms\":%lld,\"clock_drift_valid\":%s,"
            "\"brp_samples\":%u,\"brp_mm_samples\":%u,\"pld_samples\":%u,"
            "\"brp_bytes\":%ld,\"brp_mm_bytes\":%ld,"
            "\"fmt\":%d,\"stream_notifications\":%u,\"gap_events\":%u,\"gap_missing\":%u}",
            s->id, s->state, s->partial ? "true" : "false",
            (long long)s->start_epoch_ms, (long long)s->end_epoch_ms,
            (long long)s->clock_drift_ms, s->clock_drift_valid ? "true" : "false",
            (unsigned)s->brp_samples, (unsigned)s->brp_mm_samples, (unsigned)s->pld_samples,
            s->brp_bytes, s->brp_mm_bytes,
            s->fmt, (unsigned)s->stream_notifications, (unsigned)s->gap_events, (unsigned)s->gap_missing);
    }

    pos += snprintf(json + pos, json_cap - pos, "]");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, pos);
    free(json);
    free(sessions);
    return ESP_OK;
}

static void resolve_day_dir(httpd_req_t *req, const char *session_id,
                            char *day_dir, size_t day_dir_len);

/* GET /api/days — list noon-days that have session data, with a session count.
 * Cheap: reads the streams/ directory and counts *_session.json per day. */
static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

esp_err_t days_list_handler(httpd_req_t *req)
{
    DIR *dd = opendir(SD_STREAMS_DIR);
    if (!dd) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "[]", 2);
        return ESP_OK;
    }

    /* Collect day-folder names (exactly 8 digits) first, then close before
     * re-opening each for counting (keep one dir handle open at a time). */
    int cap = 64, n = 0;
    char **days = heap_caps_calloc(cap, sizeof(char *), MALLOC_CAP_SPIRAM);
    if (!days) days = calloc(cap, sizeof(char *));
    if (!days) { closedir(dd); httpd_resp_send_500(req); return ESP_FAIL; }
    struct dirent *ent;
    while ((ent = readdir(dd)) != NULL) {
        if (strlen(ent->d_name) != 8) continue;
        bool alldig = true;
        for (int i = 0; i < 8; i++) if (ent->d_name[i] < '0' || ent->d_name[i] > '9') { alldig = false; break; }
        if (!alldig) continue;
        if (n >= cap) {
            cap *= 2;
            char **tmp = realloc(days, cap * sizeof(char *));
            if (!tmp) break;
            days = tmp;
        }
        days[n] = strdup(ent->d_name);
        if (days[n]) n++;
    }
    closedir(dd);

    qsort(days, n, sizeof(char *), cmp_str);

    int json_cap = n * 48 + 64;
    char *json = heap_caps_malloc(json_cap, MALLOC_CAP_SPIRAM);
    if (!json) { for (int i = 0; i < n; i++) free(days[i]); free(days); httpd_resp_send_500(req); return ESP_FAIL; }
    int pos = 0;
    pos += snprintf(json + pos, json_cap - pos, "[");
    for (int i = 0; i < n; i++) {
        /* Count *_session.json entries in this day folder. */
        char day_path[300];
        snprintf(day_path, sizeof(day_path), "%s/%s", SD_STREAMS_DIR, days[i]);
        int sessions = 0;
        DIR *d2 = opendir(day_path);
        if (d2) {
            struct dirent *e2;
            const char *suffix = "_session.json";
            int slen = strlen(suffix);
            while ((e2 = readdir(d2)) != NULL) {
                int fl = strlen(e2->d_name);
                if (fl > slen && strcmp(e2->d_name + fl - slen, suffix) == 0) sessions++;
            }
            closedir(d2);
        }
        if (sessions == 0) continue;   /* skip empty/partial day folders */
        pos += snprintf(json + pos, json_cap - pos, "%s{\"day\":\"%s\",\"sessions\":%d}",
                        (pos > 1 ? "," : ""), days[i], sessions);
        free(days[i]);
        days[i] = NULL;
    }
    pos += snprintf(json + pos, json_cap - pos, "]");
    for (int i = 0; i < n; i++) free(days[i]);
    free(days);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, pos);
    free(json);
    return ESP_OK;
}

/* GET /api/summary?date=YYYYMMDD — per-day summary metrics from the spool. */
esp_err_t summary_handler(httpd_req_t *req)
{
    char date[16];
    if (!get_qparam(req, "date", date, sizeof(date)) || strlen(date) != 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing/invalid date");
        return ESP_FAIL;
    }
    char *json = NULL;
    esp_err_t e = edf_gen_summary_json(date, &json);
    if (e != ESP_OK || !json) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"error\":\"no summary\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}

/* GET /api/session/settings?session=ID&date=YYYYMMDD — therapy mode + pressure
 * settings for the pressure panel (CPAP flat lines). Parses the session's
 * settings.json (SettingProfiles). */
esp_err_t session_settings_handler(httpd_req_t *req)
{
    char session_id[32];
    if (!get_qparam(req, "session", session_id, sizeof(session_id))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing session");
        return ESP_FAIL;
    }
    char day_dir[300];
    resolve_day_dir(req, session_id, day_dir, sizeof(day_dir));

    char path[400];
    snprintf(path, sizeof(path), "%s/%s_settings.json", day_dir, session_id);
    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"error\":\"no settings\"}");
        return ESP_OK;
    }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz <= 0 || fsz > 32 * 1024) { fclose(f); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "bad settings"); return ESP_FAIL; }
    char *buf = heap_caps_malloc(fsz + 1, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); httpd_resp_send_500(req); return ESP_FAIL; }
    size_t rd = fread(buf, 1, fsz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "parse error"); return ESP_FAIL; }

    cJSON *sp = cJSON_GetObjectItem(root, "SettingProfiles");
    cJSON *ap = sp ? cJSON_GetObjectItem(sp, "ActiveProfiles") : NULL;
    cJSON *tp = sp ? cJSON_GetObjectItem(sp, "TherapyProfiles") : NULL;
    cJSON *fp = sp ? cJSON_GetObjectItem(sp, "FeatureProfiles") : NULL;

    cJSON *out = cJSON_CreateObject();

    cJSON *tpn = ap ? cJSON_GetObjectItem(ap, "TherapyProfile") : NULL;
    if (tpn && cJSON_IsString(tpn)) cJSON_AddStringToObject(out, "mode", tpn->valuestring);
    else cJSON_AddNullToObject(out, "mode");

    cJSON *v;
    if (tp) {
        cJSON *cpap = cJSON_GetObjectItem(tp, "CpapProfile");
        if (cpap) {
            if ((v = cJSON_GetObjectItem(cpap, "SetPressure")) && cJSON_IsNumber(v))
                cJSON_AddNumberToObject(out, "cpap_set", v->valuedouble);
        }
        cJSON *autoset = cJSON_GetObjectItem(tp, "AutoSetProfile");
        if (autoset) {
            if ((v = cJSON_GetObjectItem(autoset, "MinPressure")) && cJSON_IsNumber(v))
                cJSON_AddNumberToObject(out, "auto_min", v->valuedouble);
            if ((v = cJSON_GetObjectItem(autoset, "MaxPressure")) && cJSON_IsNumber(v))
                cJSON_AddNumberToObject(out, "auto_max", v->valuedouble);
        }
    }
    if (fp) {
        cJSON *epr = cJSON_GetObjectItem(fp, "EprFeature");
        if (epr) {
            if ((v = cJSON_GetObjectItem(epr, "EprEnable")) && cJSON_IsString(v))
                cJSON_AddBoolToObject(out, "epr_enable", strcmp(v->valuestring, "On") == 0);
            if ((v = cJSON_GetObjectItem(epr, "EprPressure")) && cJSON_IsNumber(v))
                cJSON_AddNumberToObject(out, "epr_level", v->valuedouble);
            if ((v = cJSON_GetObjectItem(epr, "EprType")) && cJSON_IsString(v))
                cJSON_AddStringToObject(out, "epr_type", v->valuestring);
        }
    }
    cJSON_Delete(root);

    char *js = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (!js) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, js, strlen(js));
    free(js);
    return ESP_OK;
}

/* Resolve the noon-day streams directory for a session.
 *
 * The session writer stores .snt files in a noon-day folder: sessions
 * before noon are placed in the previous calendar day's folder.  The
 * session ID encodes the real clock time (YYYYMMDD_HHMMSS), so for a
 * session at 04:35 on 20260717 the folder is 20260716/, not 20260717/.
 *
 * The frontend passes the noon-day as a 'date' query parameter (which it
 * already knows from the /api/sessions?date= call).  This is the
 * authoritative folder name.  If 'date' is missing (e.g. direct API call),
 * we fall back to deriving the folder from the session ID prefix — which
 * is wrong for pre-noon sessions, but is the best we can do. */
static void resolve_day_dir(httpd_req_t *req, const char *session_id,
                            char *day_dir, size_t day_dir_len)
{
    char date[16];
    if (get_qparam(req, "date", date, sizeof(date)) && strlen(date) == 8) {
        snprintf(day_dir, day_dir_len, "%s/%s", SD_STREAMS_DIR, date);
    } else {
        /* Fallback: derive from session ID prefix (incorrect for pre-noon
         * sessions, but maintains backward compatibility). */
        snprintf(day_dir, day_dir_len, "%s/%.8s", SD_STREAMS_DIR, session_id);
    }
}

/* GET /api/session/graph?session=ID&channel=flow|leak&date=YYYYMMDD
 *   &points=800           (overview — server decimates)
 *   &start_ms=X&end_ms=Y  (zoom — raw L0 for time range)
 *
 * The 'date' parameter is the noon-day folder name (YYYYMMDD) where the
 * session's .snt files reside.  See resolve_day_dir() for details.
 *
 * All values are emitted in physical units (L/s, cmH2O) — the server
 * applies digital-to-physical scaling so the frontend can render directly.
 * Time values are emitted as epoch milliseconds (int64).
 *
 * Flow overview: brp_mm.snt (L1 MinMax, 1Hz, 4×int16/record)
 *   → {"t":[...],"flow_min":[...],"flow_max":[...],"press_min":[...],"press_max":[...]}
 * Leak overview: pld.snt (L0, 0.5Hz, 12×int16/record, ch=3)
 *   → {"t":[...],"leak":[...]}
 * Flow zoom: brp.snt (L0, 25Hz, 2×int16/record)
 *   → {"t":[...],"flow":[...],"press":[...]}  */
esp_err_t session_graph_handler(httpd_req_t *req)
{
    char session_id[32], channel[16], points_str[16];
    char start_ms_str[32], end_ms_str[32];

    if (!get_qparam(req, "session", session_id, sizeof(session_id)) ||
        !get_qparam(req, "channel", channel, sizeof(channel))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing session or channel");
        return ESP_FAIL;
    }

    char day_dir[300];
    resolve_day_dir(req, session_id, day_dir, sizeof(day_dir));

    bool is_zoom = get_qparam(req, "start_ms", start_ms_str, sizeof(start_ms_str)) &&
                   get_qparam(req, "end_ms", end_ms_str, sizeof(end_ms_str));

    bool full_points = false;
    int target_points = 800;
    if (get_qparam(req, "points", points_str, sizeof(points_str))) {
        if (strcmp(points_str, "all") == 0) {
            full_points = true;
            target_points = GRAPH_MAX_POINTS;
        } else {
            target_points = atoi(points_str);
            if (target_points < 10) target_points = 10;
            if (target_points > GRAPH_MAX_POINTS) target_points = GRAPH_MAX_POINTS;
        }
    }

    /* Determine which .snt file to read.
     * Flow overview prefers *_brp_mm.snt (L1 MinMax) and falls back to *_brp.snt (L0 raw).
     * Flow zoom always uses *_brp.snt. Leak uses *_pld.snt. */
    char file_path[400];
    char alt_path[400];
    bool have_alt = false;

    if (strcmp(channel, "flow") == 0) {
        if (is_zoom) {
            snprintf(file_path, sizeof(file_path), "%s/%s_brp.snt", day_dir, session_id);
        } else {
            snprintf(file_path, sizeof(file_path), "%s/%s_brp_mm.snt", day_dir, session_id);
            snprintf(alt_path, sizeof(alt_path), "%s/%s_brp.snt", day_dir, session_id);
            have_alt = true;
        }
    } else if (strcmp(channel, "leak") == 0) {
        snprintf(file_path, sizeof(file_path), "%s/%s_pld.snt", day_dir, session_id);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown channel");
        return ESP_FAIL;
    }

    FILE *f = fopen(file_path, "rb");
    if (!f && have_alt) {
        ESP_LOGI(TAG, "graph: falling back to L0 raw: %s", alt_path);
        f = fopen(alt_path, "rb");
        if (f) strlcpy(file_path, alt_path, sizeof(file_path));
    }
    if (!f) {
        ESP_LOGW(TAG, "graph: file not found: %s", file_path);
        httpd_resp_set_type(req, "application/json");
        if (strcmp(channel, "flow") == 0) {
            httpd_resp_sendstr(req, "{\"t\":[],\"flow\":[],\"press\":[],\"error\":\"data file not found\"}");
        } else {
            httpd_resp_sendstr(req, "{\"t\":[],\"leak\":[],\"error\":\"data file not found\"}");
        }
        return ESP_OK;
    }

    snt_hdr_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != SNT_MAGIC) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "invalid SNT header");
        return ESP_FAIL;
    }

    int64_t start_ms = hdr.start_epoch_ms;
    uint32_t sample_count = hdr.sample_count;
    int n_ch = hdr.n_channels;
    int rec_bytes = n_ch * hdr.sample_bytes;
    int ms_per_rec = 10000 / hdr.sample_hz_x10;
    int16_t missing = snt_missing_for(hdr.version);

    /* If the caller asked for every point, target the file's sample count
     * but cap it so we do not try to build a multi-megabyte JSON buffer. */
    if (full_points) {
        target_points = (sample_count > 0 && sample_count < (uint32_t)GRAPH_MAX_POINTS)
                        ? (int)sample_count : GRAPH_MAX_POINTS;
    }

    /* Compute target window: full session or zoomed range */
    uint32_t start_sample = 0;
    uint32_t n_samples = sample_count;
    if (is_zoom && ms_per_rec > 0) {
        int64_t zoom_start = strtoll(start_ms_str, NULL, 10);
        int64_t zoom_end = strtoll(end_ms_str, NULL, 10);
        int64_t offset_ms = zoom_start - start_ms;
        if (offset_ms < 0) offset_ms = 0;
        start_sample = (uint32_t)(offset_ms / ms_per_rec);
        int64_t dur_ms = zoom_end - zoom_start;
        if (dur_ms <= 0) dur_ms = ms_per_rec;
        n_samples = (uint32_t)(dur_ms / ms_per_rec);
        if (start_sample >= sample_count) {
            n_samples = 0;
        } else if (start_sample + n_samples > sample_count) {
            n_samples = sample_count - start_sample;
        }
    }

    /* Allocate JSON output buffer in PSRAM based on whether we decimate or emit raw */
    bool emit_raw = (is_zoom && n_samples <= (uint32_t)(target_points * 3));
    int max_pts = emit_raw ? (n_samples + 64) : (target_points * 2 + 64);
    int json_cap = max_pts * 48 + 256;
    char *json = heap_caps_malloc(json_cap, MALLOC_CAP_SPIRAM);
    if (!json) { fclose(f); httpd_resp_send_500(req); return ESP_FAIL; }
    int pos = 0;

    if (strcmp(channel, "flow") == 0) {
        if (emit_raw && n_samples > 0) {
            /* ── Flow raw window: emit un-decimated L0 samples ── */
            int16_t *buf = heap_caps_malloc(n_samples * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
            if (!buf) { fclose(f); free(json); httpd_resp_send_500(req); return ESP_FAIL; }

            long file_offset = SNT_HDR_SIZE + (long)start_sample * rec_bytes;
            fseek(f, file_offset, SEEK_SET);
            size_t got = fread(buf, 2 * sizeof(int16_t), n_samples, f);
            n_samples = (uint32_t)got;

            pos += snprintf(json + pos, json_cap - pos, "{\"t\":[");
            for (uint32_t i = 0; i < n_samples; i++) {
                int64_t t = start_ms + (int64_t)(start_sample + i) * ms_per_rec;
                if (i > 0) pos += snprintf(json + pos, json_cap - pos, ",");
                pos += snprintf(json + pos, json_cap - pos, "%lld", (long long)t);
            }
            pos += snprintf(json + pos, json_cap - pos, "],\"flow\":[");
            for (uint32_t i = 0; i < n_samples; i++) {
                if (i > 0) pos += snprintf(json + pos, json_cap - pos, ",");
                pos += snprintf(json + pos, json_cap - pos, "%.2f", brp_flow_phys(buf[i * 2], missing));
            }
            pos += snprintf(json + pos, json_cap - pos, "],\"press\":[");
            for (uint32_t i = 0; i < n_samples; i++) {
                if (i > 0) pos += snprintf(json + pos, json_cap - pos, ",");
                pos += snprintf(json + pos, json_cap - pos, "%.2f", brp_press_phys(buf[i * 2 + 1], missing));
            }
            pos += snprintf(json + pos, json_cap - pos, "]}");
            free(buf);
        } else {
            /* ── Flow decimated: L1 or L0 MinMax envelope over requested window ── */
            int group = n_samples / target_points;
            if (group < 1) group = 1;
            int n_out = n_samples / group;
            if (n_out > target_points) n_out = target_points;

            float *flow = heap_caps_malloc(n_out * sizeof(float) * 2, MALLOC_CAP_SPIRAM);
        if (!flow) flow = malloc(n_out * sizeof(float) * 2);
            if (!flow) { fclose(f); free(json); httpd_resp_send_500(req); return ESP_FAIL; }
            float *press = flow + n_out;

            bool is_l1 = (hdr.tier == 1 && n_ch == 4);
            fseek(f, SNT_HDR_SIZE + (long)start_sample * rec_bytes, SEEK_SET);

            int out = 0; uint32_t ri = 0;
            while (ri < n_samples && out < n_out) {
                float g_fmn = 1e9f, g_fmx = -1e9f;
                float g_pmn = 1e9f, g_pmx = -1e9f;
                for (int g = 0; g < group && ri < n_samples; g++) {
                    if (is_l1) {
                        int16_t rec[4];
                        if (fread(rec, sizeof(int16_t), 4, f) != 4) { ri = n_samples; break; }
                        if (rec[0] != missing) { float v = brp_flow_phys(rec[0], missing); if (v < g_fmn) g_fmn = v; }
                        if (rec[1] != missing) { float v = brp_flow_phys(rec[1], missing); if (v > g_fmx) g_fmx = v; }
                        if (rec[2] != missing) { float v = brp_press_phys(rec[2], missing); if (v < g_pmn) g_pmn = v; }
                        if (rec[3] != missing) { float v = brp_press_phys(rec[3], missing); if (v > g_pmx) g_pmx = v; }
                    } else {
                        int16_t rec[2];
                        if (fread(rec, sizeof(int16_t), 2, f) != 2) { ri = n_samples; break; }
                        if (rec[0] != missing) {
                            float v = brp_flow_phys(rec[0], missing);
                            if (v < g_fmn) g_fmn = v;
                            if (v > g_fmx) g_fmx = v;
                        }
                        if (rec[1] != missing) {
                            float v = brp_press_phys(rec[1], missing);
                            if (v < g_pmn) g_pmn = v;
                            if (v > g_pmx) g_pmx = v;
                        }
                    }
                    ri++;
                }
                if (g_fmn > 1e8f) { g_fmn = 0.0f; }
                if (g_fmx < -1e8f) { g_fmx = 0.0f; }
                if (g_pmn > 1e8f) { g_pmn = 0.0f; }
                if (g_pmx < -1e8f) { g_pmx = 0.0f; }
                /* Preserve direction by choosing the signed extreme with the
                 * largest magnitude.  Pressure is shown as the mid-point. */
                flow[out] = (fabsf(g_fmx) >= fabsf(g_fmn)) ? g_fmx : g_fmn;
                press[out] = (g_pmn + g_pmx) * 0.5f;
                out++;
            }

            pos += snprintf(json + pos, json_cap - pos, "{\"t\":[");
            for (int i = 0; i < out; i++) {
                int64_t t = start_ms + (int64_t)(start_sample + i * group) * ms_per_rec;
                if (i > 0) pos += snprintf(json + pos, json_cap - pos, ",");
                pos += snprintf(json + pos, json_cap - pos, "%lld", (long long)t);
            }
            pos += snprintf(json + pos, json_cap - pos, "],\"flow\":[");
            for (int i = 0; i < out; i++) {
                if (i > 0) pos += snprintf(json + pos, json_cap - pos, ",");
                pos += snprintf(json + pos, json_cap - pos, "%.2f", flow[i]);
            }
            pos += snprintf(json + pos, json_cap - pos, "],\"press\":[");
            for (int i = 0; i < out; i++) {
                if (i > 0) pos += snprintf(json + pos, json_cap - pos, ",");
                pos += snprintf(json + pos, json_cap - pos, "%.2f", press[i]);
            }
            pos += snprintf(json + pos, json_cap - pos, "]}");
            free(flow);
        }
    } else if (strcmp(channel, "leak") == 0) {
        if (emit_raw && n_samples > 0) {
            /* ── Leak raw window: emit un-decimated samples from _pld.snt ── */
            int16_t *buf = heap_caps_malloc(n_samples * 12 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
            if (!buf) { fclose(f); free(json); httpd_resp_send_500(req); return ESP_FAIL; }

            long file_offset = SNT_HDR_SIZE + (long)start_sample * rec_bytes;
            fseek(f, file_offset, SEEK_SET);
            size_t got = fread(buf, 12 * sizeof(int16_t), n_samples, f);
            n_samples = (uint32_t)got;

            pos += snprintf(json + pos, json_cap - pos, "{\"t\":[");
            for (uint32_t i = 0; i < n_samples; i++) {
                int64_t t = start_ms + (int64_t)(start_sample + i) * ms_per_rec;
                if (i > 0) pos += snprintf(json + pos, json_cap - pos, ",");
                pos += snprintf(json + pos, json_cap - pos, "%lld", (long long)t);
            }
            pos += snprintf(json + pos, json_cap - pos, "],\"leak\":[");
            for (uint32_t i = 0; i < n_samples; i++) {
                if (i > 0) pos += snprintf(json + pos, json_cap - pos, ",");
                pos += snprintf(json + pos, json_cap - pos, "%.2f", pld_leak_phys(buf[i * 12 + 3], missing));
            }
            pos += snprintf(json + pos, json_cap - pos, "]}");
            free(buf);
        } else {
            /* ── Leak decimated: PLD channel 3 envelope over requested window ── */
            int group = n_samples / target_points;
            if (group < 1) group = 1;
            int n_out = n_samples / group;
            if (n_out > target_points) n_out = target_points;

            float *dec = heap_caps_malloc(n_out * sizeof(float), MALLOC_CAP_SPIRAM);
        if (!dec) dec = malloc(n_out * sizeof(float));
            if (!dec) { fclose(f); free(json); httpd_resp_send_500(req); return ESP_FAIL; }

            fseek(f, SNT_HDR_SIZE + (long)start_sample * rec_bytes, SEEK_SET);

            int16_t rec12[12];
            int out = 0; uint32_t ri = 0;
            while (ri < n_samples && out < n_out) {
                float last_val = 0.0f;
                for (int g = 0; g < group && ri < n_samples; g++) {
                    if (fread(rec12, sizeof(int16_t), 12, f) != 12) { ri = n_samples; break; }
                    if (rec12[3] != missing) last_val = pld_leak_phys(rec12[3], missing);
                    ri++;
                }
                dec[out] = last_val;
                out++;
            }

            pos += snprintf(json + pos, json_cap - pos, "{\"t\":[");
            for (int i = 0; i < out; i++) {
                int64_t t = start_ms + (int64_t)(start_sample + i * group) * ms_per_rec;
                if (i > 0) pos += snprintf(json + pos, json_cap - pos, ",");
                pos += snprintf(json + pos, json_cap - pos, "%lld", (long long)t);
            }
            pos += snprintf(json + pos, json_cap - pos, "],\"leak\":[");
            for (int i = 0; i < out; i++) {
                if (i > 0) pos += snprintf(json + pos, json_cap - pos, ",");
                pos += snprintf(json + pos, json_cap - pos, "%.2f", dec[i]);
            }
            pos += snprintf(json + pos, json_cap - pos, "]}");
            free(dec);
        }
    } else {
        /* Unsupported channel */
        pos += snprintf(json + pos, json_cap - pos, "{\"error\":\"unsupported channel\"}");
    }

    fclose(f);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, pos);
    free(json);
    return ESP_OK;
}

typedef struct {
    httpd_req_t *req;
    char file_path[400];
    long range_start;
    long send_size;
} session_file_async_t;

/* Large-file downloads are handled by a single persistent worker task fed by
 * a queue. A single worker inherently serialises transfers (the dashboard
 * downloads files sequentially in JS anyway), and — crucially — its stack is
 * allocated once from PSRAM, so downloads no longer consume ~6 KB of internal
 * RAM per request. The httpd worker enqueues a request and returns
 * immediately; ownership of each `session_file_async_t*` passes to the worker,
 * which frees it and completes the async handler. */
#define SNT_CHUNK_SIZE   8192    /* 8 KB chunks — smaller than the 32 KB TCP
                                  * send buffer so lwIP can fit several
                                  * in-flight segments between BLE coex slots */
#define SNT_QUEUE_DEPTH  6       /* pending requests before we reject with 503 */
#define SNT_WORKER_STACK 8192
static QueueHandle_t s_download_queue = NULL;

/* Stream one file to its (async) request using the caller-provided chunk
 * buffer. Does not free/complete the transfer — the worker owns that. */
static void snt_send_file(session_file_async_t *transfer, char *buf)
{
    FILE *f = fopen(transfer->file_path, "rb");
    if (!f) { httpd_resp_send_500(transfer->req); return; }

    fseek(f, transfer->range_start, SEEK_SET);
    long remaining = transfer->send_size;
    size_t n;
    /* Instrumentation: separate SD-read time from TCP-send time so we can
     * pinpoint whether download stalls originate from FATFS/SD I/O or from
     * lwIP/Wi-Fi send backpressure. */
    int64_t t_read_us = 0, t_send_us = 0;
    int64_t worst_read_us = 0, worst_send_us = 0;
    uint32_t n_chunks = 0;
    int64_t t_start = esp_timer_get_time();
    while (remaining > 0) {
        int64_t r0 = esp_timer_get_time();
        n = fread(buf, 1, (size_t)remaining < SNT_CHUNK_SIZE ? (size_t)remaining : SNT_CHUNK_SIZE, f);
        int64_t r1 = esp_timer_get_time();
        if (n == 0) break;
        int64_t read_us = r1 - r0;
        t_read_us += read_us;
        if (read_us > worst_read_us) worst_read_us = read_us;
        if (read_us > 150000) {
            ESP_LOGW(TAG, "snt_download: slow READ %lld ms (chunk %u)",
                     (long long)(read_us / 1000), (unsigned)n_chunks);
        }

        int64_t s0 = esp_timer_get_time();
        esp_err_t serr = httpd_resp_send_chunk(transfer->req, buf, (ssize_t)n);
        int64_t s1 = esp_timer_get_time();
        if (serr != ESP_OK) { fclose(f); return; }
        int64_t send_us = s1 - s0;
        t_send_us += send_us;
        if (send_us > worst_send_us) worst_send_us = send_us;
        if (send_us > 150000) {
            ESP_LOGW(TAG, "snt_download: slow SEND %lld ms (chunk %u)",
                     (long long)(send_us / 1000), (unsigned)n_chunks);
        }

        remaining -= n;
        n_chunks++;
    }
    int64_t total_ms = (esp_timer_get_time() - t_start) / 1000;
    long sent = transfer->send_size - remaining;
    ESP_LOGI(TAG, "snt_download: %ld bytes in %lld ms (%ld KB/s) | read=%lld ms (worst %lld) "
                  "send=%lld ms (worst %lld) chunks=%u",
             sent, (long long)total_ms,
             total_ms > 0 ? (long)(sent / total_ms) : 0,
             (long long)(t_read_us / 1000), (long long)(worst_read_us / 1000),
             (long long)(t_send_us / 1000), (long long)(worst_send_us / 1000),
             (unsigned)n_chunks);
    httpd_resp_send_chunk(transfer->req, NULL, 0);
    fclose(f);
}

static void snt_download_worker(void *arg)
{
    (void)arg;
    /* One reusable chunk buffer in PSRAM for the worker's lifetime. */
    char *buf = heap_caps_malloc(SNT_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    bool hwm_logged = false;
    for (;;) {
        session_file_async_t *transfer = NULL;
        if (xQueueReceive(s_download_queue, &transfer, portMAX_DELAY) != pdTRUE || !transfer)
            continue;
        if (!buf) buf = heap_caps_malloc(SNT_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
        if (!buf) {
            httpd_resp_send_500(transfer->req);
        } else {
            snt_send_file(transfer, buf);
        }
        httpd_req_async_handler_complete(transfer->req);
        free(transfer);

        /* One-shot high-water mark after the first download to verify the
         * 8 KB PSRAM stack is sufficient for the FATFS read + lwIP send path. */
        if (!hwm_logged) {
            hwm_logged = true;
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "snt_download: stack high-water = %u bytes",
                     (unsigned)(hwm * sizeof(StackType_t)));
        }
    }
}

/* Called before httpd starts. Creates the download queue and the single
 * persistent worker task on a PSRAM stack (the worker only reads SD and writes
 * to sockets — no flash writes — so an external-memory stack is safe under
 * CONFIG_SPI_FLASH_AUTO_SUSPEND). Falls back to an internal stack. */
void session_graph_init(void)
{
    if (s_download_queue) return;
    s_download_queue = xQueueCreate(SNT_QUEUE_DEPTH, sizeof(session_file_async_t *));
    if (!s_download_queue) {
        ESP_LOGE(TAG, "failed to create download queue");
        return;
    }
    static StackType_t *stack = NULL;
    static StaticTask_t *tcb = NULL;
    stack = heap_caps_malloc(SNT_WORKER_STACK, MALLOC_CAP_SPIRAM);
    tcb   = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    TaskHandle_t h = NULL;
    if (stack && tcb) {
        h = xTaskCreateStaticPinnedToCore(snt_download_worker, "snt_download",
                                          SNT_WORKER_STACK, NULL, 4, stack, tcb,
                                          tskNO_AFFINITY);
    } else {
        ESP_LOGW(TAG, "snt worker PSRAM stack alloc failed, using internal stack");
        free(stack); free(tcb); stack = NULL; tcb = NULL;
        h = psram_task_create(snt_download_worker, "snt_download", SNT_WORKER_STACK, NULL, 4, tskNO_AFFINITY, NULL, NULL);
    }
    if (!h) ESP_LOGE(TAG, "failed to create snt_download worker");
    else ESP_LOGI(TAG, "snt_download worker started (PSRAM stack=%s)", stack ? "yes" : "no");
}

/* GET /api/session/file?session=ID&date=YYYYMMDD&type=brp|brp_mm|pld|sa2|events
 * Streams the raw .snt file as application/octet-stream so the dashboard
 * can pull the whole file once and cache/decimate it in the browser.
 * Supports HTTP Range requests for parallel chunked downloads. */
esp_err_t session_file_handler(httpd_req_t *req)
{
    char session_id[32], type[16] = "brp";
    if (!get_qparam(req, "session", session_id, sizeof(session_id))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing session");
        return ESP_FAIL;
    }
    get_qparam(req, "type", type, sizeof(type));

    char day_dir[300];
    resolve_day_dir(req, session_id, day_dir, sizeof(day_dir));

    /* Only expose the .snt files we actually generate. "events" is the
     * live-captured JSONL event stream (respiratory events, therapy
     * start/stop), fetched by the dashboard for the event overlay.
     * v2 format: "flow" and "flow_mm" replace "brp" and "brp_mm".
     * v1 types are kept for backwards compatibility with old sessions. */
    if (strcmp(type, "brp") != 0 && strcmp(type, "brp_mm") != 0 &&
        strcmp(type, "flow") != 0 && strcmp(type, "flow_mm") != 0 &&
        strcmp(type, "pld") != 0 && strcmp(type, "sa2") != 0 &&
        strcmp(type, "events") != 0 && strcmp(type, "press") != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid type");
        return ESP_FAIL;
    }

    char file_path[400];
    snprintf(file_path, sizeof(file_path), "%s/%s_%s.snt", day_dir, session_id, type);

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "session_file: file not found: %s", file_path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
        return ESP_FAIL;
    }

    /* Determine file size */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");

    /* Parse Range header if present */
    long range_start = 0, range_end = file_size - 1;
    bool has_range = false;
    char range_hdr[64];
    if (httpd_req_get_hdr_value_str(req, "Range", range_hdr, sizeof(range_hdr)) == ESP_OK) {
        /* Parse "bytes=start-end" */
        if (strncmp(range_hdr, "bytes=", 6) == 0) {
            char *p = range_hdr + 6;
            char *dash = strchr(p, '-');
            if (dash) {
                *dash = '\0';
                range_start = atol(p);
                if (*(dash + 1) != '\0') {
                    range_end = atol(dash + 1);
                }
                if (range_start < 0) range_start = 0;
                if (range_end >= file_size) range_end = file_size - 1;
                if (range_start <= range_end) {
                    has_range = true;
                    /* Set Content-Range header */
                    char cr[80];
                    snprintf(cr, sizeof(cr), "bytes %ld-%ld/%ld",
                             range_start, range_end, file_size);
                    httpd_resp_set_hdr(req, "Content-Range", cr);
                    httpd_resp_set_status(req, "206 Partial Content");
                }
            }
        }
    }

    long send_size = range_end - range_start + 1;

    /* HEAD requests: send only headers, no body. */
    if (req->method == HTTP_HEAD) {
        char cl[24];
        snprintf(cl, sizeof(cl), "%ld", send_size);
        httpd_resp_set_hdr(req, "Content-Length", cl);
        httpd_resp_send_chunk(req, NULL, 0);
        fclose(f);
        return ESP_OK;
    }

    /* For very small files/ranges (<= 2 KB), read entire payload into stack
     * and send in one shot with Content-Length — avoids async task overhead.
     * Anything larger goes through the async path to avoid blocking the
     * single-threaded httpd task during send(). */
    if (send_size <= 2048) {
        char sbuf[2048];
        fseek(f, has_range ? range_start : 0, SEEK_SET);
        size_t got = fread(sbuf, 1, send_size, f);
        fclose(f);
        char cl[24];
        snprintf(cl, sizeof(cl), "%ld", (long)got);
        httpd_resp_set_hdr(req, "Content-Length", cl);
        httpd_resp_send(req, sbuf, (ssize_t)got);
        return ESP_OK;
    }

    /* Close connection after transfer — prevents the browser from holding
     * an idle keep-alive socket that occupies one of the httpd's limited
     * (10) open sockets.  The browser will open a fresh connection for the
     * next request. */
    httpd_resp_set_hdr(req, "Connection", "close");

    session_file_async_t *transfer = heap_caps_calloc(1, sizeof(*transfer), MALLOC_CAP_SPIRAM);
    if (!transfer) transfer = calloc(1, sizeof(*transfer));
    httpd_req_t *async_req = NULL;
    if (!transfer || httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        free(transfer);
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    transfer->req = async_req;
    memcpy(transfer->file_path, file_path, sizeof(transfer->file_path));
    transfer->range_start = range_start;
    transfer->send_size = send_size;
    fclose(f);

    /* Hand off to the persistent download worker. Ownership of `transfer` and
     * completion of the async handler pass to the worker. If the queue is full
     * (too many pending downloads), reject and clean up here. */
    if (!s_download_queue ||
        xQueueSend(s_download_queue, &transfer, 0) != pdTRUE) {
        ESP_LOGW(TAG, "snt_download: queue unavailable/full, rejecting request");
        httpd_req_async_handler_complete(async_req);
        free(transfer);
        return ESP_FAIL;
    }
    return ESP_OK;
}