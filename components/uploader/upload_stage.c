/*
 * SomnoTrace - SomnoStage CSV upload discovery and state
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3, or (at your option) any later version.
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

#include "upload_stage.h"
#include "upload_paths.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "cJSON.h"
#include "upload_park.h"

static const char *TAG = "up_stage";

/* These MUST mirror main/sd_storage.h and the path constants in
 * somno_ml_worker.c — the component does not depend on either. */
#define STAGE_EXPORT_DIR  SD_STAGES_DIR                       /* stages/<day>/<id>.csv */
#define STAGE_STATE_DIR   UPLOAD_STATE_DIR "/stage"
#define OX_RECORDINGS_DIR SD_MOUNT_POINT "/.somnotrace/oximetry/recordings"
#define SD_STREAMS_DIR    SD_MOUNT_POINT "/.somnotrace/sessions/streams"
#define STAGE_MAX_BACKENDS UPLOAD_MAX_BACKENDS

/* ── Per-unit state — the same convention as upload_ox.c ──────────────
 *
 * One JSON file per unit under upload_state/stage/<unit_id>.json, backends
 * keyed by NAME so registration order cannot renumber existing state:
 *
 *   {"v":1,"id":"20260925231000","day":"20260925","fp":"1d29…",
 *    "be":{"smb":{"s":"ok","t":1788603621}}}
 *
 * s_lock guards the list, same reasoning as the ox store. */
typedef struct stage_state_s {
    struct stage_state_s *next;
    uint8_t  dirty;         /* needs its file rewritten                    */
    uint8_t  in_window;     /* seen inside the newest max_days on the last
                             * reconcile — progress/pending ignore the rest */
    char     id[UPLOAD_STAGE_ID_LEN];
    char     day[12];       /* export day folder — for existence checks    */
    uint64_t fingerprint;   uint64_t sst_fingerprint;
    upload_unit_t backend[STAGE_MAX_BACKENDS];
    char     remote[STAGE_MAX_BACKENDS][64];
} stage_state_t;

static stage_state_t *s_states;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_loaded;

static bool st_lock_init(void)
{
    if (s_lock) return true;
    taskENTER_CRITICAL(&s_init_mux);
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    taskEXIT_CRITICAL(&s_init_mux);
    return s_lock != NULL;
}

static void st_lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void st_unlock(void) { xSemaphoreGive(s_lock); }

static bool safe_component(const char *s, size_t max_len)
{
    if (!s || !s[0] || strlen(s) >= max_len) return false;
    if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p == '/' || *p == '\\' || *p < 0x20) return false;
    return true;
}

static bool valid_day(const char *s)
{
    if (!s || strlen(s) != 8) return false;
    for (int i = 0; i < 8; i++) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

static bool join2(char *out, size_t n, const char *a, const char *b)
{
    size_t al = strlen(a), bl = strlen(b);
    if (al + bl + 2 > n) return false;
    memcpy(out, a, al); out[al] = '/'; memcpy(out + al + 1, b, bl + 1);
    return true;
}

static cJSON *read_json_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 16 * 1024) { fclose(f); return NULL; }
    char *buf = heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)size, f); fclose(f); buf[n] = '\0';
    cJSON *root = n == (size_t)size ? cJSON_Parse(buf) : NULL;
    free(buf); return root;
}

/* Content fingerprint: name + size + CRC32 of the CSV.  Content-based on
 * purpose — a rescore that converges to identical stages does NOT re-upload
 * (a size+mtime key would), while any real hypnogram change does. */
static uint64_t file_fp(uint64_t h, const char *name, const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return h;
    if (!h) h = 1469598103934665603ULL;
    for (const char *p = name; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
    h ^= (uint64_t)st.st_size; h *= 1099511628211ULL;
    FILE *f = fopen(path, "rb");
    if (!f) return h;
    uint8_t buf[1024]; uint32_t crc = 0; size_t nr;
    while ((nr = fread(buf, 1, sizeof(buf), f)) > 0) crc = esp_rom_crc32_le(crc, buf, nr);
    fclose(f);
    h ^= crc; h *= 1099511628211ULL;
    return h;
}

static stage_state_t *state_find_id(const char *id)
{
    for (stage_state_t *u = s_states; u; u = u->next)
        if (strcmp(u->id, id) == 0) return u;
    return NULL;
}

static void state_insert_locked(stage_state_t *u)
{
    stage_state_t **pp = &s_states;
    while (*pp && strcmp((*pp)->day, u->day) > 0)
        pp = &(*pp)->next;
    u->next = *pp;
    *pp = u;
}

static void state_reposition_locked(stage_state_t *u)
{
    for (stage_state_t **pp = &s_states; *pp; pp = &(*pp)->next) {
        if (*pp == u) {
            *pp = u->next;
            break;
        }
    }
    state_insert_locked(u);
}

static stage_state_t *state_get(const char *id, const char *day, uint64_t fp,
                                bool create)
{
    stage_state_t *u = state_find_id(id);
    if (u) {
        if (!u->day[0] && day && day[0]) {
            strlcpy(u->day, day, sizeof(u->day));
            u->dirty = true;
            state_reposition_locked(u);
        }
        return u;
    }
    if (!create) return NULL;
    u = heap_caps_calloc(1, sizeof(*u), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!u) u = calloc(1, sizeof(*u));
    if (!u) return NULL;
    strlcpy(u->id, id, sizeof(u->id));
    if (day) strlcpy(u->day, day, sizeof(u->day));
    u->fingerprint = fp;
    state_insert_locked(u);
    return u;
}

static const char *status_name(uint8_t status)
{
    return status == UG_OK ? "ok" : status == UG_FAILED ? "failed" : "pending";
}

static uint8_t status_parse(const char *name)
{
    if (name && strcmp(name, "ok") == 0) return UG_OK;
    if (name && strcmp(name, "failed") == 0) return UG_FAILED;
    return UG_PENDING;
}

/* ── Persistence ──────────────────────────────────────────────────── */

static bool unit_path(char *out, size_t n, const char *id)
{
    return snprintf(out, n, "%s/%s.json", STAGE_STATE_DIR, id) < (int)n;
}

static esp_err_t write_json_atomic(const char *path, const char *json)
{
    char tmp[UPLOAD_STAGE_PATH_LEN];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return ESP_ERR_INVALID_SIZE;

    size_t len = strlen(json);
    bool ok = false;
    FILE *f = fopen(tmp, "w");
    if (f) {
        ok = (fwrite(json, 1, len, f) == len);
        if (ok && fflush(f) != 0) ok = false;
        if (ok && fsync(fileno(f)) != 0) ok = false;
        if (fclose(f) != 0) ok = false;
    }
    if (ok) {
        unlink(path);                     /* FATFS cannot rename-over */
        if (rename(tmp, path) != 0) ok = false;
    }
    if (!ok) {
        unlink(tmp);
        ESP_LOGE(TAG, "failed to write %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t save_unit(stage_state_t *u)
{
    char path[UPLOAD_STAGE_PATH_LEN];
    if (!unit_path(path, sizeof(path), u->id)) return ESP_ERR_INVALID_SIZE;

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "id", u->id);
    if (u->day[0]) cJSON_AddStringToObject(root, "day", u->day);
    char fp[17];
    snprintf(fp, sizeof(fp), "%016llx", (unsigned long long)u->fingerprint);
    cJSON_AddStringToObject(root, "fp", fp);

    /* Only backends with a recorded outcome are written — a missing backend
     * reads back as pending/0, so a backend that gains stage support later
     * backfills automatically. */
    cJSON *be = NULL;
    for (int b = 0; b < upload_index_backend_count() && b < STAGE_MAX_BACKENDS; b++) {
        const upload_unit_t *un = &u->backend[b];
        const char *bname = upload_index_backend_name(b);
        if (!bname ||
            (un->status == UG_PENDING && un->attempts == 0 && !u->remote[b][0])) continue;
        if (!be) be = cJSON_AddObjectToObject(root, "be");
        cJSON *bo = cJSON_AddObjectToObject(be, bname);
        cJSON_AddStringToObject(bo, "s", status_name(un->status));
        if (un->attempts)   cJSON_AddNumberToObject(bo, "a", un->attempts);
        if (un->last_try_s) cJSON_AddNumberToObject(bo, "t", (double)un->last_try_s);
        if (u->remote[b][0]) cJSON_AddStringToObject(bo, "r", u->remote[b]);
    }

    if (u->sst_fingerprint) {
        char sfp[17];
        snprintf(sfp, sizeof(sfp), "%016llx", (unsigned long long)u->sst_fingerprint);
        cJSON_AddStringToObject(root, "sst_fp", sfp);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t ret = write_json_atomic(path, json);
    cJSON_free(json);
    return ret;
}

static uint8_t s_slot_fmt[STAGE_MAX_BACKENDS];

void upload_stage_set_backend_fmt(int backend_slot, upload_stage_fmt_t fmt)
{
    if (backend_slot < 0 || backend_slot >= STAGE_MAX_BACKENDS) return;
    s_slot_fmt[backend_slot] = (fmt == UPLOAD_STAGE_FMT_SST)
                             ? (uint8_t)UPLOAD_STAGE_FMT_SST
                             : (uint8_t)UPLOAD_STAGE_FMT_CSV;
}

upload_stage_fmt_t upload_stage_get_backend_fmt(int backend_slot)
{
    if (backend_slot < 0 || backend_slot >= STAGE_MAX_BACKENDS)
        return UPLOAD_STAGE_FMT_CSV;
    return (upload_stage_fmt_t)s_slot_fmt[backend_slot];
}

bool upload_stage_ref_has_fmt(const upload_stage_ref_t *ref,
                              upload_stage_fmt_t fmt)
{
    if (!ref) return false;
    if (fmt == UPLOAD_STAGE_FMT_SST)
        return ref->sst_path[0] != '\0' && ref->sst_fingerprint != 0;
    return ref->csv_path[0] != '\0' && ref->fingerprint != 0;
}

void upload_stage_sst_filename(const upload_stage_ref_t *ref,
                               char *out, size_t n)
{
    if (!out || n == 0) return;
    if (!ref || !ref->unit_id[0]) { out[0] = '\0'; return; }
    snprintf(out, n, "%s_stages.sst", ref->unit_id);
}

static uint64_t ref_fp_for_slot(const upload_stage_ref_t *ref, int slot)
{
    return (s_slot_fmt[slot] == UPLOAD_STAGE_FMT_SST)
         ? ref->sst_fingerprint : ref->fingerprint;
}

static uint64_t state_fp_for_slot(const stage_state_t *u, int slot)
{
    return (s_slot_fmt[slot] == UPLOAD_STAGE_FMT_SST)
         ? u->sst_fingerprint : u->fingerprint;
}

static void load_unit_file(const char *path)
{
    cJSON *root = read_json_file(path);
    if (!root) return;

    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *fp = cJSON_GetObjectItem(root, "fp");
    cJSON *sfp = cJSON_GetObjectItem(root, "sst_fp");
    if (!cJSON_IsString(id) || !safe_component(id->valuestring, UPLOAD_STAGE_ID_LEN) ||
        (!cJSON_IsString(fp) && !cJSON_IsString(sfp))) {
        ESP_LOGW(TAG, "%s is not a valid unit file — re-deriving state", path);
        cJSON_Delete(root);
        return;
    }

    cJSON *day = cJSON_GetObjectItem(root, "day");
    stage_state_t *u = state_get(id->valuestring,
                                 cJSON_IsString(day) ? day->valuestring : NULL,
                                 0, true);
    if (!u) { cJSON_Delete(root); return; }
    if (cJSON_IsString(fp))
        u->fingerprint = strtoull(fp->valuestring, NULL, 16);
    if (cJSON_IsString(sfp))
        u->sst_fingerprint = strtoull(sfp->valuestring, NULL, 16);

    cJSON *be = cJSON_GetObjectItem(root, "be");
    for (cJSON *bo = be ? be->child : NULL; bo; bo = bo->next) {
        if (!bo->string) continue;
        int slot = upload_index_backend_slot(bo->string);
        if (slot < 0 || slot >= STAGE_MAX_BACKENDS) continue;
        cJSON *s = cJSON_GetObjectItem(bo, "s");
        cJSON *a = cJSON_GetObjectItem(bo, "a");
        cJSON *t = cJSON_GetObjectItem(bo, "t");
        cJSON *r = cJSON_GetObjectItem(bo, "r");
        u->backend[slot].status = status_parse(cJSON_IsString(s) ? s->valuestring : NULL);
        if (cJSON_IsNumber(a)) u->backend[slot].attempts = (uint8_t)a->valueint;
        if (cJSON_IsNumber(t)) u->backend[slot].last_try_s = (uint32_t)t->valuedouble;
        if (cJSON_IsString(r)) strlcpy(u->remote[slot], r->valuestring, sizeof(u->remote[slot]));
    }
    u->dirty = false;
    cJSON_Delete(root);
}

/* Flush every dirty unit.  Caller must hold s_lock. */
static esp_err_t save_all_locked(void)
{
    esp_err_t last = ESP_OK;
    for (stage_state_t *u = s_states; u; u = u->next) {
        if (!u->dirty) continue;
        if (save_unit(u) == ESP_OK) u->dirty = false;
        else last = ESP_FAIL;
    }
    return last;
}

/* State units should always carry their day, but a corrupt/future file may
 * not.  One pass at init resolves day-less units by locating <id>.csv under
 * the export tree; still day-less afterwards means the CSV is gone and the
 * unit is evicted rather than counting as pending forever.  Same root-guard
 * as reconcile: an unreadable export tree proves nothing. */
static void resolve_orphan_days_locked(void)
{
    bool any_orphan = false;
    for (stage_state_t *u = s_states; u; u = u->next)
        if (!u->day[0]) { any_orphan = true; break; }
    if (!any_orphan) return;

    struct stat rst;
    if (stat(STAGE_EXPORT_DIR, &rst) != 0 || !S_ISDIR(rst.st_mode)) return;

    DIR *root = opendir(STAGE_EXPORT_DIR);
    if (!root) return;
    struct dirent *e;
    while ((e = readdir(root))) {
        if (!valid_day(e->d_name)) continue;
        char dpath[UPLOAD_STAGE_PATH_LEN];
        if (!join2(dpath, sizeof(dpath), STAGE_EXPORT_DIR, e->d_name)) continue;
        DIR *dd = opendir(dpath);
        if (!dd) continue;
        struct dirent *de;
        while ((de = readdir(dd))) {
            size_t l = strlen(de->d_name);
            if (l <= 4 || strcmp(de->d_name + l - 4, ".csv") != 0) continue;
            char id[UPLOAD_STAGE_ID_LEN];
            if (l - 4 >= sizeof(id)) continue;
            memcpy(id, de->d_name, l - 4);
            id[l - 4] = '\0';
            stage_state_t *u = state_find_id(id);
            if (u && !u->day[0]) {
                strlcpy(u->day, e->d_name, sizeof(u->day));
                u->dirty = true;
                state_reposition_locked(u);
            }
        }
        closedir(dd);
    }
    closedir(root);

    stage_state_t **pp = &s_states;
    while (*pp) {
        stage_state_t *u = *pp;
        if (u->day[0]) {
            pp = &u->next;
            continue;
        }
        char path[UPLOAD_STAGE_PATH_LEN];
        if (unit_path(path, sizeof(path), u->id)) unlink(path);
        *pp = u->next;
        free(u);
    }
}

static int s_window_days;

static void apply_window_locked(int max_days)
{
    if (max_days < 1) max_days = 1;
    int days_seen = 0;
    const char *last_day = "";
    for (stage_state_t *u = s_states; u; u = u->next) {
        if (!u->day[0]) { u->in_window = false; continue; }
        if (strcmp(u->day, last_day) != 0) {
            last_day = u->day;
            days_seen++;
        }
        u->in_window = (days_seen <= max_days);
    }
    s_window_days = max_days;
}

esp_err_t upload_stage_init(void)
{
    if (!st_lock_init()) return ESP_ERR_NO_MEM;
    st_lock();
    if (s_loaded) { st_unlock(); return ESP_OK; }

    mkdir(UPLOAD_STATE_DIR, 0775);
    mkdir(STAGE_STATE_DIR, 0775);

    DIR *d = opendir(STAGE_STATE_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            const char *n = e->d_name;
            size_t l = strlen(n);
            char path[UPLOAD_STAGE_PATH_LEN];
            if (l < 6 || strcmp(n + l - 5, ".json") != 0) {
                /* Residue from a power loss between tmp write and rename. */
                if (l > 9 && strcmp(n + l - 9, ".json.tmp") == 0 &&
                    snprintf(path, sizeof(path), "%s/%s", STAGE_STATE_DIR, n) <
                        (int)sizeof(path))
                    unlink(path);
                continue;
            }
            if (snprintf(path, sizeof(path), "%s/%s", STAGE_STATE_DIR, n) >=
                (int)sizeof(path)) continue;
            load_unit_file(path);
        }
        closedir(d);
    }

    resolve_orphan_days_locked();
    apply_window_locked(UPLOAD_DEFAULT_MAX_DAYS);
    save_all_locked();

    s_loaded = true;
    st_unlock();
    return ESP_OK;
}

/* Validate an SST1 binary sidecar (mirrors somno_sst_header_t in
 * components/somno_ml/include/somno_ml.h without introducing a component
 * dependency).  Refuses INSUFFICIENT (0x04) and still-growing PARTIAL (0x01)
 * stubs, truncated files, files > 256 KB, and files with no scored stage. */
#define SST_MAGIC            0x31545353u /* "SST1" */
#define SST_VERSION          1u
#define SST_FLAG_PARTIAL     0x01u
#define SST_FLAG_INSUFFICIENT 0x04u
#define SST_HEADER_BYTES     64u
#define SST_MAX_EPOCHS       100000u
#define SST_MAX_BYTES        (256u * 1024u)

static uint16_t rd_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int64_t rd_i64_le(const uint8_t *p)
{
    uint64_t lo = rd_u32_le(p);
    uint64_t hi = rd_u32_le(p + 4);
    return (int64_t)(lo | (hi << 32));
}

/* ── Unit-id local stamp / server offset derivation ─────────────────── */

/* Days since 1970-01-01 for a proleptic Gregorian date (Hinnant). */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int)doe - 719468;
}

/* The unit id's local wall-clock stamp expressed as "milliseconds as if UTC",
 * 0 when the id carries no local time.  Ring ids are 14 digits
 * (YYYYMMDDhhmmss, ring local); AS11 ids are as11_YYYYMMDD_HHMMSS (session
 * start, device local). */
static int64_t unit_id_stamp_ms(const char *unit_id)
{
    int y, mo, d, h, mi, sec;
    char date[9], time[7];

    if (!unit_id || !unit_id[0]) return 0;

    if (strncmp(unit_id, "as11_", 5) == 0) {
        const char *p = unit_id + 5;
        if (strlen(p) < 15 || p[8] != '_') return 0;
        memcpy(date, p, 8); date[8] = '\0';
        memcpy(time, p + 9, 6); time[6] = '\0';
    } else {
        if (strlen(unit_id) < 14) return 0;
        for (int i = 0; i < 14; i++)
            if (unit_id[i] < '0' || unit_id[i] > '9') return 0;
        memcpy(date, unit_id, 8); date[8] = '\0';
        memcpy(time, unit_id + 8, 6); time[6] = '\0';
    }
    if (sscanf(date, "%4d%2d%2d", &y, &mo, &d) != 3) return 0;
    if (sscanf(time, "%2d%2d%2d", &h, &mi, &sec) != 3) return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60)
        return 0;

    return days_from_civil(y, mo, d) * 86400000LL +
           (int64_t)h * 3600000LL + (int64_t)mi * 60000LL + (int64_t)sec * 1000LL;
}

/* Epoch 0 of an SST1 file (UTC ms), 0 when unreadable. */
static int64_t sst_start_ms(const char *path)
{
    if (!path || !path[0]) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t hdr[SST_HEADER_BYTES];
    size_t n = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (n != sizeof(hdr) || rd_u32_le(hdr + 0) != SST_MAGIC ||
        rd_u16_le(hdr + 4) != SST_VERSION)
        return 0;
    return rd_i64_le(hdr + 40);
}

bool upload_stage_needs_timezone(const upload_stage_ref_t *ref)
{
    if (!ref || !ref->sst_path[0]) return false;

    const int64_t stamp = unit_id_stamp_ms(ref->unit_id);
    if (stamp <= 0) return true;            /* id carries no local time */

    const int64_t start = sst_start_ms(ref->sst_path);
    if (start <= 0) return false;           /* filtered by validate_sst_file */

    /* Mirror the server: snap the implied offset to 15 minutes, then require
     * epoch 0 to land within five minutes of the id's local stamp. */
    const int64_t step = 15 * 60 * 1000;
    int64_t off = stamp - start;
    int64_t snapped = (off >= 0 ? (off + step / 2) / step
                                : -((-off + step / 2) / step)) * step;
    int64_t diff = start + snapped - stamp;
    if (diff < 0) diff = -diff;
    return diff > 5 * 60 * 1000;
}

static bool validate_sst_file(const char *path)
{
    if (!path || !path[0]) return false;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (st.st_size <= (off_t)SST_HEADER_BYTES ||
        st.st_size > (off_t)SST_MAX_BYTES) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[SST_HEADER_BYTES];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return false;
    }
    uint32_t magic = rd_u32_le(hdr + 0);
    uint16_t version = rd_u16_le(hdr + 4);
    uint8_t flags = hdr[7];
    uint32_t n_epochs = rd_u32_le(hdr + 36);
    int64_t start_ms = rd_i64_le(hdr + 40);
    uint32_t epoch_sec = rd_u32_le(hdr + 48);

    if (magic != SST_MAGIC || version != SST_VERSION ||
        (flags & (SST_FLAG_PARTIAL | SST_FLAG_INSUFFICIENT)) != 0 ||
        n_epochs == 0 || n_epochs > SST_MAX_EPOCHS ||
        epoch_sec == 0 || epoch_sec > 3600u || start_ms <= 0 ||
        (uint64_t)st.st_size != (uint64_t)SST_HEADER_BYTES + 2ULL * n_epochs) {
        fclose(f);
        return false;
    }

    uint8_t buf[512];
    uint32_t rem = n_epochs;
    bool any_stage = false;
    while (rem > 0) {
        size_t chunk = rem < sizeof(buf) ? rem : sizeof(buf);
        if (fread(buf, 1, chunk, f) != chunk) {
            fclose(f);
            return false;
        }
        for (size_t i = 0; i < chunk; i++) {
            if (buf[i] < 4) { any_stage = true; break; }
        }
        if (any_stage) break;
        rem -= (uint32_t)chunk;
    }
    fclose(f);
    return any_stage;
}

static bool resolve_sst_candidate_path(const char *day, const char *unit_id,
                                       char *out, size_t cap)
{
    out[0] = '\0';
    if (strncmp(unit_id, "as11_", 5) == 0) {
        return snprintf(out, cap, "%s/%s/%s_stages.sst",
                        SD_STREAMS_DIR, day, unit_id + 5) < (int)cap;
    }
    char rec_dir[128], pj[256];
    if (!join2(rec_dir, sizeof(rec_dir), OX_RECORDINGS_DIR, day) ||
        snprintf(pj, sizeof(pj), "%s/%s/recording.json", rec_dir,
                 unit_id) >= (int)sizeof(pj)) return false;
    cJSON *j = read_json_file(pj);
    if (!j) return false;
    cJSON *g = cJSON_GetObjectItem(j, "active_generation");
    bool ok = cJSON_IsNumber(g) && g->valueint > 0 &&
              snprintf(out, cap, "%s/%s/generations/%d/data/stages.sst",
                       rec_dir, unit_id, g->valueint) < (int)cap;
    if (!ok) out[0] = '\0';
    cJSON_Delete(j);
    return ok;
}

/* Best-effort source .sst resolution for native-format endpoints.
 * Ring units read the recording's active generation; as11_ units map back
 * to the streams day dir they were exported from.  Validates the SST1 header
 * and computes `sst_fingerprint`; clears `sst_path` on failure. */
static void resolve_sst_path(upload_stage_ref_t *r)
{
    r->sst_path[0] = '\0';
    r->sst_fingerprint = 0;
    if (!resolve_sst_candidate_path(r->day, r->unit_id,
                                    r->sst_path, sizeof(r->sst_path)) ||
        !validate_sst_file(r->sst_path)) {
        r->sst_path[0] = '\0';
        return;
    }
    char sname[UPLOAD_STAGE_ID_LEN + 16];
    upload_stage_sst_filename(r, sname, sizeof(sname));
    r->sst_fingerprint = file_fp(0, sname, r->sst_path);
}

static bool day_has_unit(const upload_stage_ref_t *out, int n, const char *id)
{
    for (int i = 0; i < n; i++)
        if (strcmp(out[i].unit_id, id) == 0) return true;
    return false;
}

static int scan_day(const char *day, upload_stage_ref_t *out, int max_out,
                    bool *truncated)
{
    if (!valid_day(day)) return 0;
    int n = 0;

    /* 1. Consumer CSV exports in stages/<day>/<unit_id>.csv */
    char day_path[UPLOAD_STAGE_PATH_LEN];
    if (join2(day_path, sizeof(day_path), STAGE_EXPORT_DIR, day)) {
        DIR *d = opendir(day_path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                size_t l = strlen(e->d_name);
                if (l <= 4 || strcmp(e->d_name + l - 4, ".csv") != 0) continue;
                if (l - 4 >= UPLOAD_STAGE_ID_LEN) continue;
                char id[UPLOAD_STAGE_ID_LEN];
                memcpy(id, e->d_name, l - 4);
                id[l - 4] = '\0';
                if (!safe_component(id, sizeof(id))) continue;
                char local[UPLOAD_STAGE_PATH_LEN];
                if (!join2(local, sizeof(local), day_path, e->d_name)) continue;
                struct stat st;
                if (stat(local, &st) != 0 || !S_ISREG(st.st_mode)) continue;
                if (n >= max_out) {
                    if (truncated) *truncated = true;
                    break;
                }
                upload_stage_ref_t *r = &out[n];
                memset(r, 0, sizeof(*r));
                strlcpy(r->day, day, sizeof(r->day));
                strlcpy(r->unit_id, id, sizeof(r->unit_id));
                strlcpy(r->csv_path, local, sizeof(r->csv_path));
                r->fingerprint = file_fp(0, e->d_name, local);
                resolve_sst_path(r);
                n++;
            }
            closedir(d);
        }
    }

    /* 2. Direct AS11 SST discovery (streams/<day>/<prefix>_stages.sst) in case
     *    a valid sidecar exists without a CSV in STAGE_EXPORT_DIR. */
    if (!(truncated && *truncated) &&
        join2(day_path, sizeof(day_path), SD_STREAMS_DIR, day)) {
        DIR *d = opendir(day_path);
        if (d) {
            static const char suf[] = "_stages.sst";
            const size_t suflen = sizeof(suf) - 1;
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                size_t l = strlen(e->d_name);
                if (l <= suflen || strcmp(e->d_name + l - suflen, suf) != 0)
                    continue;
                size_t plen = l - suflen;
                char id[UPLOAD_STAGE_ID_LEN];
                if (5 + plen >= sizeof(id)) continue;
                memcpy(id, "as11_", 5);
                memcpy(id + 5, e->d_name, plen);
                id[5 + plen] = '\0';
                if (!safe_component(id, sizeof(id)) || day_has_unit(out, n, id))
                    continue;
                char local[UPLOAD_STAGE_PATH_LEN];
                if (!join2(local, sizeof(local), day_path, e->d_name) ||
                    !validate_sst_file(local))
                    continue;
                if (n >= max_out) {
                    if (truncated) *truncated = true;
                    break;
                }
                upload_stage_ref_t *r = &out[n];
                memset(r, 0, sizeof(*r));
                strlcpy(r->day, day, sizeof(r->day));
                strlcpy(r->unit_id, id, sizeof(r->unit_id));
                strlcpy(r->sst_path, local, sizeof(r->sst_path));
                char sname[UPLOAD_STAGE_ID_LEN + 16];
                upload_stage_sst_filename(r, sname, sizeof(sname));
                r->sst_fingerprint = file_fp(0, sname, r->sst_path);
                n++;
            }
            closedir(d);
        }
    }

    /* 3. Direct Ring SST discovery (recordings/<day>/<rec_id>/generations/<g>/data/stages.sst) */
    if (!(truncated && *truncated) &&
        join2(day_path, sizeof(day_path), OX_RECORDINGS_DIR, day)) {
        DIR *d = opendir(day_path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (!safe_component(e->d_name, UPLOAD_STAGE_ID_LEN) ||
                    strncmp(e->d_name, "as11_", 5) == 0 ||
                    day_has_unit(out, n, e->d_name))
                    continue;
                char sst[UPLOAD_STAGE_PATH_LEN];
                if (!resolve_sst_candidate_path(day, e->d_name, sst, sizeof(sst)) ||
                    !validate_sst_file(sst))
                    continue;
                if (n >= max_out) {
                    if (truncated) *truncated = true;
                    break;
                }
                upload_stage_ref_t *r = &out[n];
                memset(r, 0, sizeof(*r));
                strlcpy(r->day, day, sizeof(r->day));
                strlcpy(r->unit_id, e->d_name, sizeof(r->unit_id));
                strlcpy(r->sst_path, sst, sizeof(r->sst_path));
                char sname[UPLOAD_STAGE_ID_LEN + 16];
                upload_stage_sst_filename(r, sname, sizeof(sname));
                r->sst_fingerprint = file_fp(0, sname, r->sst_path);
                n++;
            }
            closedir(d);
        }
    }

    return n;
}

static void collect_days_from_root(const char *root_path, char (*days)[12], int *ndp)
{
    DIR *root = opendir(root_path);
    if (!root) return;
    int nd = *ndp;
    struct dirent *e;
    while ((e = readdir(root)) != NULL) {
        if (!valid_day(e->d_name)) continue;
        bool exists = false;
        for (int k = 0; k < nd; k++) {
            if (strcmp(days[k], e->d_name) == 0) { exists = true; break; }
        }
        if (exists) continue;

        if (nd < UPLOAD_MAX_DAYS_CAP) {
            int j = nd - 1;
            while (j >= 0 && strcmp(days[j], e->d_name) < 0) {
                memcpy(days[j + 1], days[j], sizeof(days[0]));
                j--;
            }
            strlcpy(days[j + 1], e->d_name, sizeof(days[0]));
            nd++;
        } else {
            if (strcmp(e->d_name, days[UPLOAD_MAX_DAYS_CAP - 1]) <= 0)
                continue;
            int j = UPLOAD_MAX_DAYS_CAP - 2;
            while (j >= 0 && strcmp(days[j], e->d_name) < 0) {
                memcpy(days[j + 1], days[j], sizeof(days[0]));
                j--;
            }
            strlcpy(days[j + 1], e->d_name, sizeof(days[0]));
        }
    }
    closedir(root);
    *ndp = nd;
}

static int scan_days(upload_stage_ref_t *out, int max_out, int max_days)
{
    if (!out || max_out <= 0) return 0;
    upload_stage_init();

    char (*days)[12] = heap_caps_calloc(UPLOAD_MAX_DAYS_CAP, sizeof(*days),
                                        MALLOC_CAP_SPIRAM);
    if (!days) days = calloc(UPLOAD_MAX_DAYS_CAP, sizeof(*days));
    if (!days) return 0;

    int nd = 0;
    collect_days_from_root(STAGE_EXPORT_DIR, days, &nd);
    collect_days_from_root(SD_STREAMS_DIR, days, &nd);
    collect_days_from_root(OX_RECORDINGS_DIR, days, &nd);

    int n = 0, i = 0, days_with_data = 0;
    bool truncated = false;
    for (i = 0; i < nd; i++) {
        if (max_days > 0 && days_with_data >= max_days) break;
        if (n >= max_out) { truncated = true; break; }
        int added = scan_day(days[i], &out[n], max_out - n, &truncated);
        if (added > 0) {
            n += added;
            days_with_data++;
        }
        if (truncated) break;
    }
    if (truncated)
        ESP_LOGW(TAG, "more than %d stage exports on the card — oldest day(s) "
                 "not visible to the uploader this pass", max_out);
    free(days);
    return n;
}

int upload_stage_scan(upload_stage_ref_t *out, int max_out)
{
    return scan_days(out, max_out, 0);
}

static void reset_backends_for_fmt_locked(stage_state_t *u, upload_stage_fmt_t fmt)
{
    for (int b = 0; b < STAGE_MAX_BACKENDS; b++) {
        if ((upload_stage_fmt_t)s_slot_fmt[b] == fmt) {
            memset(&u->backend[b], 0, sizeof(u->backend[b]));
            memset(u->remote[b], 0, sizeof(u->remote[b]));
        }
    }
}

int upload_stage_reconcile(upload_stage_ref_t *out, int max_out, int max_days)
{
    if (max_days < 1) max_days = 1;
    int n = scan_days(out, max_out, max_days);
    if (!st_lock_init()) {
        ESP_LOGE(TAG, "state lock unavailable — stage pass skipped");
        return 0;
    }
    st_lock();
    /* Window membership is recomputed every reconcile: only units inside the
     * newest max_days distinct day folders count toward pending/progress. */
    for (stage_state_t *u = s_states; u; u = u->next) u->in_window = false;
    int kept = 0, days_seen = 0;
    char last_day[12] = {0};
    for (int i = 0; i < n; i++) {
        if (strcmp(last_day, out[i].day) != 0) {
            strlcpy(last_day, out[i].day, sizeof(last_day));
            days_seen++;
        }
        if (days_seen > max_days) continue;
        /* State is keyed by unit_id alone — the same id under a second day
         * folder (e.g. a duplicated recording tree) would alternate the
         * stored fingerprint between copies and reset backends every pass.
         * Refs arrive sorted by day descending: first sight is the newest
         * (canonical) copy; later duplicates are dropped before they can
         * touch state. */
        int dup_at = -1;
        for (int k = 0; k < kept; k++) {
            if (strcmp(out[k].unit_id, out[i].unit_id) == 0) {
                dup_at = k;
                break;
            }
        }
        if (dup_at >= 0) {
            ESP_LOGW(TAG, "unit %s exists under %s and %s — %s wins",
                     out[i].unit_id, out[dup_at].day, out[i].day,
                     out[dup_at].day);
            continue;
        }
        if (kept != i) out[kept] = out[i];
        bool existed = (state_find_id(out[kept].unit_id) != NULL);
        stage_state_t *u = state_get(out[kept].unit_id, out[kept].day,
                                     out[kept].fingerprint, true);
        if (u) {
            u->in_window = true;
            if (!existed) {
                u->sst_fingerprint = out[kept].sst_fingerprint;
            } else {
                if (u->fingerprint != out[kept].fingerprint) {
                    /* CSV changed — reset only CSV-consuming backends. */
                    u->fingerprint = out[kept].fingerprint;
                    reset_backends_for_fmt_locked(u, UPLOAD_STAGE_FMT_CSV);
                    u->dirty = true;
                }
                if (u->sst_fingerprint != out[kept].sst_fingerprint) {
                    /* SST changed — reset only SST-consuming backends. */
                    u->sst_fingerprint = out[kept].sst_fingerprint;
                    reset_backends_for_fmt_locked(u, UPLOAD_STAGE_FMT_SST);
                    u->dirty = true;
                }
            }
        }
        kept++;
    }

    /* Evict states whose CSV and SST both no longer exist on the card,
     * guarded on at least one stage source tree being readable. */
    struct stat rst_csv, rst_str, rst_ox;
    bool csv_root_ok = (stat(STAGE_EXPORT_DIR, &rst_csv) == 0 && S_ISDIR(rst_csv.st_mode));
    bool sst_root_ok = (stat(SD_STREAMS_DIR, &rst_str) == 0 && S_ISDIR(rst_str.st_mode)) ||
                       (stat(OX_RECORDINGS_DIR, &rst_ox) == 0 && S_ISDIR(rst_ox.st_mode));
    if (csv_root_ok || sst_root_ok) {
        stage_state_t **pp = &s_states;
        while (*pp) {
            stage_state_t *u = *pp;
            bool csv_gone = true;
            bool sst_gone = true;
            if (u->day[0]) {
                if (csv_root_ok) {
                    char path[UPLOAD_STAGE_PATH_LEN];
                    snprintf(path, sizeof(path), "%s/%s/%s.csv",
                             STAGE_EXPORT_DIR, u->day, u->id);
                    struct stat st;
                    if (stat(path, &st) == 0) {
                        csv_gone = !S_ISREG(st.st_mode);
                    } else if (errno != ENOENT && errno != ENOTDIR) {
                        csv_gone = false;
                    }
                } else {
                    csv_gone = (u->fingerprint == 0);
                }
                if (sst_root_ok && u->sst_fingerprint != 0) {
                    char sst[UPLOAD_STAGE_PATH_LEN];
                    if (resolve_sst_candidate_path(u->day, u->id, sst, sizeof(sst)) &&
                        validate_sst_file(sst))
                        sst_gone = false;
                }
            } else {
                csv_gone = false;
            }
            if (csv_gone && sst_gone) {
                char path[UPLOAD_STAGE_PATH_LEN];
                if (unit_path(path, sizeof(path), u->id)) unlink(path);
                *pp = u->next;
                free(u);
            } else {
                pp = &u->next;
            }
        }
    }

    save_all_locked();
    s_window_days = max_days;
    st_unlock();
    return kept;
}

int upload_stage_status(const upload_stage_ref_t *ref, int backend_slot)
{
    if (!ref || backend_slot < 0 || backend_slot >= STAGE_MAX_BACKENDS ||
        !st_lock_init()) return UG_PENDING;
    uint64_t rfp = ref_fp_for_slot(ref, backend_slot);
    if (rfp == 0) return UG_OK;   /* format not present on this unit */
    /* Lookup only — never creates.  Reconcile is the legitimate creator. */
    st_lock();
    stage_state_t *u = state_find_id(ref->unit_id);
    int s = (u && state_fp_for_slot(u, backend_slot) == rfp)
          ? u->backend[backend_slot].status : UG_PENDING;
    st_unlock();
    return s;
}

void upload_stage_mark(const upload_stage_ref_t *ref, int backend_slot,
                       upload_unit_status_t status, const char *remote_id)
{
    if (!ref || backend_slot < 0 || backend_slot >= STAGE_MAX_BACKENDS ||
        !st_lock_init()) return;
    st_lock();
    stage_state_t *u = state_get(ref->unit_id, ref->day, ref->fingerprint, true);
    if (!u) {
        ESP_LOGW(TAG, "no state for %s — mark for slot %d dropped",
                 ref->unit_id, backend_slot);
        st_unlock();
        return;
    }
    if (s_slot_fmt[backend_slot] == UPLOAD_STAGE_FMT_SST) {
        if (ref->sst_fingerprint) u->sst_fingerprint = ref->sst_fingerprint;
    } else {
        if (ref->fingerprint) u->fingerprint = ref->fingerprint;
    }
    u->backend[backend_slot].status = status;
    u->backend[backend_slot].last_try_s = (uint32_t)time(NULL);
    if (status != UG_OK) u->backend[backend_slot].attempts++;
    if (remote_id) strlcpy(u->remote[backend_slot], remote_id,
                           sizeof(u->remote[backend_slot]));
    /* The mark is the durability-critical write: persist immediately. */
    if (save_unit(u) != ESP_OK)
        u->dirty = true;
    else
        u->dirty = false;
    st_unlock();
}

/* Same park semantics as EDF groups (upload_park.h): a unit that has failed
 * UPLOAD_MAX_GROUP_ATTEMPTS times stops counting as pending and is skipped by
 * dispatch until the parking goes stale — one permanently bad or rejected
 * unit must not retry forever.  A fingerprint change resets status/attempts
 * through reconcile and unparks automatically. */
bool upload_stage_parked(const upload_stage_ref_t *ref, int backend_slot)
{
    if (!ref || backend_slot < 0 || backend_slot >= STAGE_MAX_BACKENDS ||
        !st_lock_init()) return false;
    uint64_t rfp = ref_fp_for_slot(ref, backend_slot);
    if (rfp == 0) return false;   /* format not present on this unit */
    st_lock();
    stage_state_t *u = state_find_id(ref->unit_id);
    bool parked = u && state_fp_for_slot(u, backend_slot) == rfp &&
                  upload_park_active(u->backend[backend_slot].status == UG_FAILED,
                                     u->backend[backend_slot].attempts,
                                     u->backend[backend_slot].last_try_s,
                                     (uint32_t)time(NULL));
    st_unlock();
    return parked;
}

int upload_stage_pending(const upload_stage_ref_t *refs, int n_refs,
                         int backend_slot)
{
    int n = 0;
    for (int i = 0; i < n_refs; i++) {
        if (ref_fp_for_slot(&refs[i], backend_slot) == 0) continue;
        if (upload_stage_status(&refs[i], backend_slot) != UG_OK &&
            !upload_stage_parked(&refs[i], backend_slot)) n++;
    }
    return n;
}

int upload_stage_cached_pending(int backend_slot)
{
    /* Same reasoning as upload_ox_cached_pending: no lazy init — the
     * scheduler inits at startup; before that the honest answer is zero. */
    if (!s_loaded || backend_slot < 0 || backend_slot >= STAGE_MAX_BACKENDS)
        return 0;
    st_lock();
    int n = 0;
    uint32_t now = (uint32_t)time(NULL);
    for (stage_state_t *u = s_states; u; u = u->next) {
        const upload_unit_t *un = &u->backend[backend_slot];
        if (u->in_window && state_fp_for_slot(u, backend_slot) != 0 &&
            un->status != UG_OK &&
            !upload_park_active(un->status == UG_FAILED, un->attempts,
                                un->last_try_s, now)) n++;
    }
    st_unlock();
    return n;
}

void upload_stage_backend_progress(int backend_slot, int max_days,
                                   int *out_done, int *out_total)
{
    int done = 0, total = 0;
    if (s_loaded && backend_slot >= 0 && backend_slot < STAGE_MAX_BACKENDS) {
        st_lock();
        if (max_days > 0 && max_days != s_window_days)
            apply_window_locked(max_days);
        /* Count distinct in-window days that have at least one unit in this
         * backend's format (s_states is kept sorted by day descending): a day
         * is done when every applicable unit in it is UG_OK. */
        const char *cur_day = NULL;
        bool day_ok = true;
        for (stage_state_t *u = s_states; u; u = u->next) {
            if (!u->in_window || !u->day[0] ||
                state_fp_for_slot(u, backend_slot) == 0) continue;
            if (!cur_day || strcmp(u->day, cur_day) != 0) {
                if (cur_day && day_ok) done++;
                cur_day = u->day;
                day_ok = true;
                total++;
            }
            if (u->backend[backend_slot].status != UG_OK)
                day_ok = false;
        }
        if (cur_day && day_ok) done++;
        st_unlock();
    }
    if (out_done) *out_done = done;
    if (out_total) *out_total = total;
}

esp_err_t upload_stage_clear(void)
{
    if (!st_lock_init()) return ESP_ERR_NO_MEM;
    st_lock();

    DIR *d = opendir(STAGE_STATE_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char path[UPLOAD_STAGE_PATH_LEN];
            if (snprintf(path, sizeof(path), "%s/%s", STAGE_STATE_DIR,
                         e->d_name) < (int)sizeof(path))
                unlink(path);
        }
        closedir(d);
    }

    while (s_states) {
        stage_state_t *next = s_states->next;
        free(s_states);
        s_states = next;
    }
    s_window_days = 0;

    ESP_LOGI(TAG, "stage upload state cleared");
    st_unlock();
    return ESP_OK;
}
