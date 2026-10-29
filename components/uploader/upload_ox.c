/*
 * SomnoTrace - Canonical oximetry upload discovery and state
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

#include "upload_ox.h"
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

static const char *TAG = "up_ox";

#define OX_RECORDINGS_DIR SD_MOUNT_POINT "/.somnotrace/oximetry/recordings"
#define OX_STATE_DIR      UPLOAD_STATE_DIR "/ox"
#define OX_STATE_LEGACY   UPLOAD_STATE_DIR "/oximetry.json"
#define OX_STATE_LEGACY_DONE UPLOAD_STATE_DIR "/oximetry.json.migrated"
#define OX_MAX_BACKENDS_LOCAL UPLOAD_MAX_BACKENDS

/* ── Per-recording state, modelled on the CPAP day index ──────────────
 *
 * One JSON file per recording under upload_state/ox/<recording_id>.json,
 * backends keyed by NAME — the same convention upload_index.c day files use,
 * so a change in backend registration order cannot renumber existing state:
 *
 *   {"v":1,"id":"20260925033858","day":"20260924","gen":1,"fp":"1d29…",
 *    "be":{"smb":{"s":"ok","t":1788603621},"sleephq":{"s":"ok",…}}}
 *
 * In RAM the states are a linked list grown on demand.  The pre-rewrite
 * design was a fixed UPLOAD_OX_MAX_UNITS-slot array: once it filled, every
 * further recording read as pending forever while its "uploaded" mark was
 * silently dropped — the same file went to SleepHQ on every pass.  A list
 * plus per-recording files removes the capacity question entirely.
 *
 * s_lock guards the list: progress and summary queries run on the httpd and
 * MQTT tasks, everything else on the scheduler task. */
typedef struct ox_state_s {
    struct ox_state_s *next;
    uint8_t  dirty;         /* needs its file rewritten                   */
    uint8_t  in_window;     /* inside the newest max_days on the last
                             * reconcile — pending/progress ignore the rest */
    char     id[UPLOAD_OX_ID_LEN];
    char     day[12];       /* noon-day folder — for existence checks     */
    uint32_t generation;
    uint64_t fingerprint;
    upload_unit_t backend[OX_MAX_BACKENDS_LOCAL];
    char     remote[OX_MAX_BACKENDS_LOCAL][64];
} ox_state_t;

static ox_state_t *s_states;        /* head of the list, PSRAM nodes     */
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_loaded;

static bool ox_lock_init(void)
{
    if (s_lock) return true;
    taskENTER_CRITICAL(&s_init_mux);
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    taskEXIT_CRITICAL(&s_init_mux);
    return s_lock != NULL;
}

static void ox_lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void ox_unlock(void) { xSemaphoreGive(s_lock); }

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

static ox_state_t *state_find_id(const char *id)
{
    for (ox_state_t *u = s_states; u; u = u->next)
        if (strcmp(u->id, id) == 0) return u;
    return NULL;
}

/* Keep s_states sorted by u->day descending so all units for a day are
 * contiguous and window/progress queries run in a single O(N) pass. */
static void state_insert_locked(ox_state_t *u)
{
    ox_state_t **pp = &s_states;
    while (*pp && strcmp((*pp)->day, u->day) > 0)
        pp = &(*pp)->next;
    u->next = *pp;
    *pp = u;
}

static void state_reposition_locked(ox_state_t *u)
{
    for (ox_state_t **pp = &s_states; *pp; pp = &(*pp)->next) {
        if (*pp == u) {
            *pp = u->next;
            break;
        }
    }
    state_insert_locked(u);
}

/* Look up the state for a recording, optionally creating it.  One node per
 * recording_id: a generation bump (re-conversion) resets the node in place —
 * the unit file is per recording, not per generation.  Scalar args, not a
 * ref: upload_ox_ref_t is ~1.3 KB of path buffers and several callers run
 * this in a loop on modest task stacks. */
static ox_state_t *state_get(const char *id, const char *day, uint32_t gen,
                             uint64_t fp, bool create)
{
    ox_state_t *u = state_find_id(id);
    if (u) {
        if (u->generation != gen) {
            u->generation = gen;
            u->fingerprint = fp;
            memset(u->backend, 0, sizeof(u->backend));
            memset(u->remote, 0, sizeof(u->remote));
            u->dirty = true;
        }
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
    u->generation = gen;
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
    return snprintf(out, n, "%s/%s.json", OX_STATE_DIR, id) < (int)n;
}

static esp_err_t write_json_atomic(const char *path, const char *json)
{
    char tmp[UPLOAD_OX_PATH_LEN];
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

static esp_err_t save_unit(ox_state_t *u)
{
    char path[UPLOAD_OX_PATH_LEN];
    if (!unit_path(path, sizeof(path), u->id)) return ESP_ERR_INVALID_SIZE;

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "id", u->id);
    if (u->day[0]) cJSON_AddStringToObject(root, "day", u->day);
    cJSON_AddNumberToObject(root, "gen", (double)u->generation);
    char fp[17];
    snprintf(fp, sizeof(fp), "%016llx", (unsigned long long)u->fingerprint);
    cJSON_AddStringToObject(root, "fp", fp);

    /* Only backends with a recorded outcome are written — the CPAP day-file
     * convention: a missing backend reads back as pending/0. */
    cJSON *be = NULL;
    for (int b = 0; b < upload_index_backend_count() && b < OX_MAX_BACKENDS_LOCAL; b++) {
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

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t ret = write_json_atomic(path, json);
    cJSON_free(json);
    return ret;
}

static void load_unit_file(const char *path)
{
    cJSON *root = read_json_file(path);
    if (!root) return;

    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *gen = cJSON_GetObjectItem(root, "gen");
    cJSON *fp = cJSON_GetObjectItem(root, "fp");
    if (!cJSON_IsString(id) || !safe_component(id->valuestring, UPLOAD_OX_ID_LEN) ||
        !cJSON_IsNumber(gen) || !cJSON_IsString(fp)) {
        ESP_LOGW(TAG, "%s is not a valid unit file — recording re-derives state", path);
        cJSON_Delete(root);
        return;
    }

    cJSON *day = cJSON_GetObjectItem(root, "day");
    ox_state_t *u = state_get(id->valuestring,
                            cJSON_IsString(day) ? day->valuestring : NULL,
                            (uint32_t)gen->valuedouble, 0, true);
    if (!u) { cJSON_Delete(root); return; }
    u->fingerprint = strtoull(fp->valuestring, NULL, 16);

    cJSON *be = cJSON_GetObjectItem(root, "be");
    for (cJSON *bo = be ? be->child : NULL; bo; bo = bo->next) {
        if (!bo->string) continue;
        int slot = upload_index_backend_slot(bo->string);
        if (slot < 0 || slot >= OX_MAX_BACKENDS_LOCAL) continue;
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
    for (ox_state_t *u = s_states; u; u = u->next) {
        if (!u->dirty) continue;
        if (save_unit(u) == ESP_OK) u->dirty = false;
        else last = ESP_FAIL;
    }
    return last;
}

/* Legacy single-file state (numeric backend slots, cap 64).  Imported once at
 * init, then renamed aside — kept rather than deleted so a migration fault is
 * recoverable. */
static void migrate_legacy(void)
{
    FILE *f = fopen(OX_STATE_LEGACY, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 256 * 1024) { fclose(f); return; }
    char *buf = heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return; }
    size_t nr = fread(buf, 1, (size_t)size, f); fclose(f); buf[nr] = '\0';
    cJSON *root = (nr == (size_t)size) ? cJSON_Parse(buf) : NULL;
    free(buf);
    if (!root) return;

    int imported = 0;
    cJSON *units = cJSON_GetObjectItem(root, "units");
    cJSON *item;
    cJSON_ArrayForEach(item, units) {
        cJSON *id = cJSON_GetObjectItem(item, "recording_id");
        cJSON *gen = cJSON_GetObjectItem(item, "generation");
        cJSON *fp = cJSON_GetObjectItem(item, "fingerprint");
        if (!cJSON_IsString(id) || !cJSON_IsNumber(gen) ||
            (!cJSON_IsNumber(fp) && !cJSON_IsString(fp))) continue;
        if (state_find_id(id->valuestring)) continue;   /* new format wins */

        ox_state_t *u = state_get(id->valuestring, NULL,
                                  (uint32_t)gen->valuedouble, 0, true);
        if (!u) continue;
        u->fingerprint = cJSON_IsString(fp)
            ? strtoull(fp->valuestring, NULL, 16)
            : (uint64_t)fp->valuedouble;

        cJSON *bes = cJSON_GetObjectItem(item, "backends");
        cJSON *be;
        cJSON_ArrayForEach(be, bes) {
            cJSON *bi = cJSON_GetObjectItem(be, "slot");
            cJSON *bs = cJSON_GetObjectItem(be, "status");
            if (!cJSON_IsNumber(bi) || !cJSON_IsString(bs) ||
                bi->valueint < 0 || bi->valueint >= OX_MAX_BACKENDS_LOCAL) continue;
            int b = bi->valueint;
            u->backend[b].status = status_parse(bs->valuestring);
            cJSON *at = cJSON_GetObjectItem(be, "attempts");
            cJSON *lt = cJSON_GetObjectItem(be, "last_try_s");
            cJSON *ri = cJSON_GetObjectItem(be, "remote_id");
            if (cJSON_IsNumber(at)) u->backend[b].attempts = (uint8_t)at->valueint;
            if (cJSON_IsNumber(lt)) u->backend[b].last_try_s = (uint32_t)lt->valuedouble;
            if (cJSON_IsString(ri)) strlcpy(u->remote[b], ri->valuestring, sizeof(u->remote[b]));
        }
        u->dirty = true;
        imported++;
    }
    cJSON_Delete(root);
    if (!imported) return;

    ESP_LOGI(TAG, "migrating %d legacy oximetry unit(s) to per-recording files",
             imported);
    if (save_all_locked() == ESP_OK &&
        rename(OX_STATE_LEGACY, OX_STATE_LEGACY_DONE) == 0) {
        ESP_LOGI(TAG, "legacy oximetry state migrated");
    } else {
        /* Retry next boot: state_find_id() dedup makes re-import harmless,
         * and the legacy file is left untouched so no state is lost. */
        ESP_LOGW(TAG, "legacy oximetry state migration deferred (SD write failed)");
    }
}

/* Legacy units carry no day, so reconcile's existence check can never reach
 * them.  One pass at init locates each day-less unit's recording dir under
 * the recordings root; a unit still day-less afterwards belongs to a deleted
 * recording and is evicted, else it would count as pending forever.  Same
 * root-guard as reconcile: an unreadable recordings tree proves nothing. */
static void resolve_orphan_days_locked(void)
{
    bool any_orphan = false;
    for (ox_state_t *u = s_states; u; u = u->next)
        if (!u->day[0]) { any_orphan = true; break; }
    if (!any_orphan) return;

    struct stat rst;
    if (stat(OX_RECORDINGS_DIR, &rst) != 0 || !S_ISDIR(rst.st_mode)) return;

    DIR *root = opendir(OX_RECORDINGS_DIR);
    if (!root) return;
    struct dirent *e;
    while ((e = readdir(root))) {
        if (!valid_day(e->d_name)) continue;
        char dpath[UPLOAD_OX_PATH_LEN];
        if (!join2(dpath, sizeof(dpath), OX_RECORDINGS_DIR, e->d_name)) continue;
        DIR *dd = opendir(dpath);
        if (!dd) continue;
        struct dirent *de;
        while ((de = readdir(dd))) {
            ox_state_t *u = state_find_id(de->d_name);
            if (u && !u->day[0]) {
                strlcpy(u->day, e->d_name, sizeof(u->day));
                u->dirty = true;
                state_reposition_locked(u);
            }
        }
        closedir(dd);
    }
    closedir(root);

    ox_state_t **pp = &s_states;
    while (*pp) {
        ox_state_t *u = *pp;
        if (u->day[0]) {
            pp = &u->next;
            continue;
        }
        char path[UPLOAD_OX_PATH_LEN];
        if (unit_path(path, sizeof(path), u->id)) unlink(path);
        *pp = u->next;
        free(u);
    }
}

/* Mark only the newest max_days distinct day folders in s_states as
 * in_window.  Used at init (before the first card scan) and if max_days
 * changes between scans so progress never over-counts older persisted
 * history at boot and then jumps backward on the first reconcile. */
static int s_window_days;

static void apply_window_locked(int max_days)
{
    if (max_days < 1) max_days = 1;
    int days_seen = 0;
    const char *last_day = "";
    for (ox_state_t *u = s_states; u; u = u->next) {
        if (!u->day[0]) { u->in_window = false; continue; }
        if (strcmp(u->day, last_day) != 0) {
            last_day = u->day;
            days_seen++;
        }
        u->in_window = (days_seen <= max_days);
    }
    s_window_days = max_days;
}

esp_err_t upload_ox_init(void)
{
    if (!ox_lock_init()) return ESP_ERR_NO_MEM;
    ox_lock();
    if (s_loaded) { ox_unlock(); return ESP_OK; }

    mkdir(UPLOAD_STATE_DIR, 0775);
    mkdir(OX_STATE_DIR, 0775);

    DIR *d = opendir(OX_STATE_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            const char *n = e->d_name;
            size_t l = strlen(n);
            char path[UPLOAD_OX_PATH_LEN];
            if (l < 6 || strcmp(n + l - 5, ".json") != 0) {
                /* Residue from a power loss between tmp write and rename. */
                if (l > 9 && strcmp(n + l - 9, ".json.tmp") == 0 &&
                    snprintf(path, sizeof(path), "%s/%s", OX_STATE_DIR, n) <
                        (int)sizeof(path))
                    unlink(path);
                continue;
            }
            if (snprintf(path, sizeof(path), "%s/%s", OX_STATE_DIR, n) >=
                (int)sizeof(path)) continue;
            load_unit_file(path);
        }
        closedir(d);
    }

    migrate_legacy();
    resolve_orphan_days_locked();
    apply_window_locked(UPLOAD_DEFAULT_MAX_DAYS);
    save_all_locked();

    s_loaded = true;
    ox_unlock();
    return ESP_OK;
}

esp_err_t upload_ox_save(void)
{
    if (!s_loaded) return ESP_ERR_INVALID_STATE;
    ox_lock();
    esp_err_t ret = save_all_locked();
    ox_unlock();
    return ret;
}

static int scan_day(const char *day, upload_ox_ref_t *out, int max_out,
                    bool *truncated)
{
    char day_path[UPLOAD_OX_PATH_LEN];
    if (!valid_day(day) || !join2(day_path, sizeof(day_path), OX_RECORDINGS_DIR, day)) return 0;
    DIR *d = opendir(day_path); if (!d) return 0;
    int n = 0; struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!safe_component(e->d_name, UPLOAD_OX_ID_LEN)) continue;
        char root[UPLOAD_OX_PATH_LEN]; if (!join2(root, sizeof(root), day_path, e->d_name)) continue;
        char pointer[UPLOAD_OX_PATH_LEN]; if (!join2(pointer, sizeof(pointer), root, "recording.json")) continue;
        cJSON *p = read_json_file(pointer);
        if (!p) continue;
        cJSON *state = cJSON_GetObjectItem(p, "state");
        cJSON *id = cJSON_GetObjectItem(p, "recording_id");
        cJSON *gen = cJSON_GetObjectItem(p, "active_generation");
        cJSON *uploadable = cJSON_GetObjectItem(p, "uploadable");
        if (!cJSON_IsString(state) || strcmp(state->valuestring, "ready") != 0 ||
            cJSON_IsFalse(uploadable) || !cJSON_IsString(id) || !cJSON_IsNumber(gen) ||
            !safe_component(id->valuestring, UPLOAD_OX_ID_LEN)) { cJSON_Delete(p); continue; }
        if (n >= max_out) {
            if (truncated) *truncated = true;
            cJSON_Delete(p);
            break;
        }
        upload_ox_ref_t *r = &out[n]; memset(r, 0, sizeof(*r));
        strlcpy(r->day, day, sizeof(r->day)); strlcpy(r->recording_id, id->valuestring, sizeof(r->recording_id));
        r->generation = (uint32_t)gen->valuedouble; strlcpy(r->root_path, root, sizeof(r->root_path));
        char manifest_path[UPLOAD_OX_PATH_LEN]; char gen_name[64];
        snprintf(gen_name, sizeof(gen_name), "generations/%u/manifest.json", (unsigned)r->generation);
        if (join2(manifest_path, sizeof(manifest_path), root, gen_name)) {
            cJSON *gm = read_json_file(manifest_path);
            cJSON *sources = gm ? cJSON_GetObjectItem(gm, "source") : NULL;
            cJSON *src = sources ? cJSON_GetArrayItem(sources, 0) : NULL;
            cJSON *orig = src ? cJSON_GetObjectItem(src, "original_name") : NULL;
            if (cJSON_IsString(orig)) strlcpy(r->source_name, orig->valuestring, sizeof(r->source_name));
            if (gm) cJSON_Delete(gm);
        }
        if (!r->source_name[0]) strlcpy(r->source_name, "source.bin", sizeof(r->source_name));
        const char *rels[] = { "recording.json", "source/source.bin", "source/source.vld" };
        char gen_dir[32]; snprintf(gen_dir, sizeof(gen_dir), "generations/%u", (unsigned)r->generation);
        char rel_manifest[UPLOAD_OX_REL_LEN];
        snprintf(rel_manifest, sizeof(rel_manifest), "%s/manifest.json", gen_dir);
        const char *all[] = { rels[0], rels[1], rels[2], rel_manifest };
        for (int i = 0; i < 4; i++) {
            if (!all[i]) continue;
            char local[UPLOAD_OX_PATH_LEN]; if (!join2(local, sizeof(local), root, all[i])) continue;
            struct stat st; if (stat(local, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            strlcpy(r->local_paths[r->n_files], local, sizeof(r->local_paths[r->n_files]));
            strlcpy(r->relative_paths[r->n_files], all[i], sizeof(r->relative_paths[r->n_files])); r->n_files++;
            r->fingerprint = file_fp(r->fingerprint, all[i], local);
        }
        cJSON_Delete(p);
        if (r->n_files >= 2) n++;
    }
    closedir(d); return n;
}

static int scan_days(upload_ox_ref_t *out, int max_out, int max_days)
{
    if (!out || max_out <= 0) return 0;
    upload_ox_init();
    DIR *root = opendir(OX_RECORDINGS_DIR);
    if (!root) return 0;

    char (*days)[12] = heap_caps_calloc(UPLOAD_MAX_DAYS_CAP, sizeof(*days), MALLOC_CAP_SPIRAM);
    if (!days) {
        closedir(root);
        return 0;
    }

    int nd = 0;
    struct dirent *e;
    while ((e = readdir(root)) != NULL) {
        if (!valid_day(e->d_name)) continue;

        if (nd < UPLOAD_MAX_DAYS_CAP) {
            /* Insertion sort descending into days[0..nd] */
            int j = nd - 1;
            while (j >= 0 && strcmp(days[j], e->d_name) < 0) {
                memcpy(days[j + 1], days[j], sizeof(days[0]));
                j--;
            }
            strlcpy(days[j + 1], e->d_name, sizeof(days[0]));
            nd++;
        } else {
            /* Buffer full: days[UPLOAD_MAX_DAYS_CAP - 1] is the oldest kept day.
             * If e->d_name is older than or equal to the oldest entry, skip. */
            if (strcmp(e->d_name, days[UPLOAD_MAX_DAYS_CAP - 1]) <= 0) {
                continue;
            }
            /* Replace the oldest entry and shift up to sorted position. */
            int j = UPLOAD_MAX_DAYS_CAP - 2;
            while (j >= 0 && strcmp(days[j], e->d_name) < 0) {
                memcpy(days[j + 1], days[j], sizeof(days[0]));
                j--;
            }
            strlcpy(days[j + 1], e->d_name, sizeof(days[0]));
        }
    }
    closedir(root);

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
        ESP_LOGW(TAG, "more than %d recordings on the card — oldest day(s) "
                 "not visible to the uploader this pass", max_out);
    free(days);
    return n;
}

int upload_ox_scan(upload_ox_ref_t *out, int max_out)
{
    return scan_days(out, max_out, 0);
}

int upload_ox_reconcile(upload_ox_ref_t *out, int max_out, int max_days)
{
    if (max_days < 1) max_days = 1;
    int n = scan_days(out, max_out, max_days);
    if (!ox_lock_init()) {
        ESP_LOGE(TAG, "state lock unavailable — oximetry pass skipped");
        return 0;
    }
    ox_lock();
    /* Window membership is recomputed every reconcile: only recordings in
     * the newest max_days distinct day folders count toward the pending and
     * progress numbers — anything older stays tracked but out of view. */
    for (ox_state_t *u = s_states; u; u = u->next) u->in_window = false;
    int kept = 0, days_seen = 0; char last_day[12] = {0};
    for (int i = 0; i < n; i++) {
        if (strcmp(last_day, out[i].day) != 0) {
            strlcpy(last_day, out[i].day, sizeof(last_day)); days_seen++;
        }
        if (days_seen > max_days) continue;
        /* State is keyed by recording_id alone — the same id appearing under
         * a second day folder would make every pass alternate the stored
         * fingerprint between the two copies and reset backends forever.
         * Refs arrive sorted by day descending, so first sight is the newest
         * (canonical) copy; later duplicates are dropped before they can
         * touch state. */
        int dup_at = -1;
        for (int k = 0; k < kept; k++) {
            if (strcmp(out[k].recording_id, out[i].recording_id) == 0) {
                dup_at = k;
                break;
            }
        }
        if (dup_at >= 0) {
            ESP_LOGW(TAG, "recording %s exists under %s and %s — %s wins",
                     out[i].recording_id, out[dup_at].day, out[i].day,
                     out[dup_at].day);
            continue;
        }
        if (kept != i) out[kept] = out[i];
        ox_state_t *u = state_get(out[kept].recording_id, out[kept].day,
                                  out[kept].generation, out[kept].fingerprint,
                                  true);
        if (u) {
            u->in_window = true;
            if (u->fingerprint != out[kept].fingerprint) {
                /* Content changed since last seen — re-upload for all backends. */
                u->fingerprint = out[kept].fingerprint;
                memset(u->backend, 0, sizeof(u->backend));
                memset(u->remote, 0, sizeof(u->remote));
                u->dirty = true;
            }
        }
        kept++;
    }

    /* Evict states whose recording no longer exists on the card — otherwise
     * deleted recordings linger forever (and count as pending in summaries).
     * Guarded on the recordings ROOT being readable: if the card directory
     * itself is unavailable, the per-recording stat would falsely report
     * every recording as gone and wipe all state. */
    struct stat rst;
    if (stat(OX_RECORDINGS_DIR, &rst) == 0 && S_ISDIR(rst.st_mode)) {
        ox_state_t **pp = &s_states;
        while (*pp) {
            ox_state_t *u = *pp;
            bool gone = false;
            if (u->day[0]) {
                char dir[UPLOAD_OX_PATH_LEN];
                snprintf(dir, sizeof(dir), "%s/%s/%s",
                         OX_RECORDINGS_DIR, u->day, u->id);
                struct stat st;
                if (stat(dir, &st) == 0) {
                    if (!S_ISDIR(st.st_mode)) gone = true;
                } else if (errno == ENOENT || errno == ENOTDIR) {
                    gone = true;
                }
            }
            if (gone) {
                char path[UPLOAD_OX_PATH_LEN];
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
    ox_unlock();
    return kept;
}

int upload_ox_status(const upload_ox_ref_t *ref, int backend_slot)
{
    if (!ref || backend_slot < 0 || backend_slot >= OX_MAX_BACKENDS_LOCAL ||
        !ox_lock_init()) return UG_PENDING;
    /* Lookup only — never creates.  Reconcile is the legitimate creator; a
     * reader (e.g. the httpd status endpoint) must not mint states. */
    ox_lock();
    ox_state_t *u = state_find_id(ref->recording_id);
    int s = (u && u->generation == ref->generation)
          ? u->backend[backend_slot].status : UG_PENDING;
    ox_unlock();
    return s;
}

void upload_ox_mark(const upload_ox_ref_t *ref, int backend_slot,
                   upload_unit_status_t status, const char *remote_id)
{
    if (!ref || backend_slot < 0 || backend_slot >= OX_MAX_BACKENDS_LOCAL ||
        !ox_lock_init()) return;
    ox_lock();
    ox_state_t *u = state_get(ref->recording_id, ref->day, ref->generation,
                              ref->fingerprint, true);
    if (!u) {
        /* Only reachable on allocation failure — log it: the previous silent
         * drop is what let a full table re-upload the same file forever. */
        ESP_LOGW(TAG, "no state for %s — mark for slot %d dropped",
                 ref->recording_id, backend_slot);
        ox_unlock();
        return;
    }
    u->backend[backend_slot].status = status;
    u->backend[backend_slot].last_try_s = (uint32_t)time(NULL);
    if (status != UG_OK) u->backend[backend_slot].attempts++;
    if (remote_id) strlcpy(u->remote[backend_slot], remote_id,
                           sizeof(u->remote[backend_slot]));
    /* The mark is the durability-critical write: persist immediately rather
     * than waiting for the next reconcile's flush. */
    if (save_unit(u) != ESP_OK)
        u->dirty = true;   /* stays dirty; next save_all_locked() retries */
    else
        u->dirty = false;
    ox_unlock();
}

/* Same park semantics as EDF groups (upload_park.h): a unit that has failed
 * UPLOAD_MAX_GROUP_ATTEMPTS times stops counting as pending and is skipped by
 * dispatch until the parking goes stale — one permanently bad or rejected
 * recording must not retry forever.  Parked state follows the stored unit:
 * a generation or fingerprint change resets status/attempts and unparks. */
bool upload_ox_parked(const upload_ox_ref_t *ref, int backend_slot)
{
    if (!ref || backend_slot < 0 || backend_slot >= OX_MAX_BACKENDS_LOCAL ||
        !ox_lock_init()) return false;
    ox_lock();
    ox_state_t *u = state_find_id(ref->recording_id);
    bool parked = u && u->generation == ref->generation &&
                  upload_park_active(u->backend[backend_slot].status == UG_FAILED,
                                     u->backend[backend_slot].attempts,
                                     u->backend[backend_slot].last_try_s,
                                     (uint32_t)time(NULL));
    ox_unlock();
    return parked;
}

int upload_ox_pending(const upload_ox_ref_t *refs, int n_refs, int backend_slot)
{
    int n = 0;
    for (int i = 0; i < n_refs; i++)
        if (upload_ox_status(&refs[i], backend_slot) != UG_OK &&
            !upload_ox_parked(&refs[i], backend_slot)) n++;
    return n;
}

int upload_ox_cached_pending(int backend_slot)
{
    /* No lazy init here: upload_ox_init() does SD I/O and JSON printing now,
     * too heavy for whatever task a summary query happens to arrive on (the
     * MQTT task has 8 KB).  The scheduler inits at startup; before that the
     * honest answer is "nothing loaded".  Only in-window units count — the
     * last reconcile fixed membership; recordings beyond the upload window
     * are tracked but are not outstanding work. */
    if (!s_loaded || backend_slot < 0 || backend_slot >= OX_MAX_BACKENDS_LOCAL) return 0;
    ox_lock();
    int n = 0;
    uint32_t now = (uint32_t)time(NULL);
    for (ox_state_t *u = s_states; u; u = u->next) {
        const upload_unit_t *un = &u->backend[backend_slot];
        if (u->in_window && un->status != UG_OK &&
            !upload_park_active(un->status == UG_FAILED, un->attempts,
                                un->last_try_s, now)) n++;
    }
    ox_unlock();
    return n;
}

void upload_ox_backend_progress(int backend_slot, int max_days,
                                int *out_done, int *out_total)
{
    int done = 0, total = 0;
    if (s_loaded && backend_slot >= 0 && backend_slot < OX_MAX_BACKENDS_LOCAL) {
        ox_lock();
        if (max_days > 0 && max_days != s_window_days)
            apply_window_locked(max_days);
        /* Count distinct in-window days in a single pass (s_states is kept
         * sorted by day descending): a day is done when every recording in it
         * is UG_OK, matching CPAP's day-bundle semantics. */
        const char *cur_day = NULL;
        bool day_ok = true;
        for (ox_state_t *u = s_states; u; u = u->next) {
            if (!u->in_window || !u->day[0]) continue;
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
        ox_unlock();
    }
    if (out_done) *out_done = done;
    if (out_total) *out_total = total;
}

esp_err_t upload_ox_clear(void)
{
    if (!ox_lock_init()) return ESP_ERR_NO_MEM;
    ox_lock();

    DIR *d = opendir(OX_STATE_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char path[UPLOAD_OX_PATH_LEN];
            if (snprintf(path, sizeof(path), "%s/%s", OX_STATE_DIR, e->d_name) <
                (int)sizeof(path)) {
                unlink(path);
            }
        }
        closedir(d);
    }

    while (s_states) {
        ox_state_t *next = s_states->next;
        free(s_states);
        s_states = next;
    }
    s_window_days = 0;

    ESP_LOGI(TAG, "oximetry upload state cleared");
    ox_unlock();
    return ESP_OK;
}

char *upload_ox_status_json(void)
{
    if (!s_loaded || !ox_lock_init()) return strdup("[]");
    ox_lock();
    cJSON *arr = cJSON_CreateArray();
    if (!arr) { ox_unlock(); return NULL; }
    for (ox_state_t *s = s_states; s; s = s->next) {
        cJSON *u = cJSON_CreateObject();
        cJSON_AddStringToObject(u, "recording_id", s->id);
        cJSON_AddStringToObject(u, "day", s->day);
        cJSON_AddNumberToObject(u, "generation", s->generation);
        cJSON *bes = cJSON_AddArrayToObject(u, "backends");
        for (int b = 0; b < OX_MAX_BACKENDS_LOCAL; b++) {
            cJSON *be = cJSON_CreateObject();
            cJSON_AddNumberToObject(be, "slot", b);
            cJSON_AddStringToObject(be, "id", upload_index_backend_name(b));
            cJSON_AddStringToObject(be, "status", status_name(s->backend[b].status));
            cJSON_AddItemToArray(bes, be);
        }
        cJSON_AddItemToArray(arr, u);
    }
    ox_unlock();
    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return out;
}
