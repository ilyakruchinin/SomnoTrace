/*
 * SomnoTrace - Upload tracking index (per-group, per-backend)
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
 */

#include "upload_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "up_index";

#define STATE_FILE_VERSION  1

/* ── Module state ─────────────────────────────────────────────────── */

static upload_day_t *s_days[UPLOAD_MAX_DAYS_CAP];
static int s_n_days = 0;

static char s_be_names[UPLOAD_MAX_BACKENDS][UPLOAD_BACKEND_ID_LEN];
static int  s_n_backends = 0;

static uint64_t s_bundle_ok_fp[UPLOAD_MAX_BACKENDS];

static bool s_ready = false;

/* ── Small helpers ────────────────────────────────────────────────── */

static void *idx_alloc(size_t n)
{
    /* Day records are a few hundred bytes each and never touched from an
     * ISR, so PSRAM is the right home; fall back to internal RAM. */
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = calloc(1, n);
    return p;
}

static void day_path(uint32_t day, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%08u.json", UPLOAD_STATE_DIR, (unsigned)day);
}

static const char *status_name(uint8_t s)
{
    switch (s) {
    case UG_OK:     return "ok";
    case UG_FAILED: return "failed";
    default:        return "pending";
    }
}

static uint8_t status_from_name(const char *s)
{
    if (!s) return UG_PENDING;
    if (strcmp(s, "ok") == 0) return UG_OK;
    if (strcmp(s, "failed") == 0) return UG_FAILED;
    return UG_PENDING;
}

/* "134405" <-> seconds since midnight. */
static uint32_t hhmmss_to_sec(const char *s)
{
    if (!s || strlen(s) < 6) return 0;
    int h = (s[0] - '0') * 10 + (s[1] - '0');
    int m = (s[2] - '0') * 10 + (s[3] - '0');
    int c = (s[4] - '0') * 10 + (s[5] - '0');
    return (uint32_t)(h * 3600 + m * 60 + c);
}

static void sec_to_hhmmss(uint32_t sec, char *out, size_t out_len)
{
    unsigned h = (unsigned)(sec / 3600);
    if (h > 99) h = 99;
    snprintf(out, out_len, "%02u%02u%02u",
             h, (unsigned)((sec / 60) % 60),
             (unsigned)(sec % 60));
}

/* Kind bitmask <-> readable list, so the state files explain themselves. */
static const struct { const char *name; uint8_t bit; } KINDS[] = {
    { "BRP", UGK_BRP }, { "PLD", UGK_PLD }, { "SA2", UGK_SA2 },
    { "EVE", UGK_EVE }, { "CSL", UGK_CSL },
};
#define N_KINDS (int)(sizeof(KINDS) / sizeof(KINDS[0]))

static void kinds_to_str(uint8_t kinds, char *out, size_t out_len)
{
    out[0] = '\0';
    size_t pos = 0;
    for (int i = 0; i < N_KINDS; i++) {
        if (!(kinds & KINDS[i].bit)) continue;
        int n = snprintf(out + pos, out_len - pos, "%s%s",
                         pos ? "," : "", KINDS[i].name);
        if (n < 0 || (size_t)n >= out_len - pos) break;
        pos += n;
    }
}

static uint8_t kinds_from_str(const char *s)
{
    uint8_t k = 0;
    if (!s) return 0;
    for (int i = 0; i < N_KINDS; i++) {
        if (strstr(s, KINDS[i].name)) k |= KINDS[i].bit;
    }
    return k;
}

/* ── Backend slots ────────────────────────────────────────────────── */

int upload_index_backend_slot(const char *backend_id)
{
    if (!backend_id || !backend_id[0]) return -1;
    for (int i = 0; i < s_n_backends; i++) {
        if (strcmp(s_be_names[i], backend_id) == 0) return i;
    }
    if (s_n_backends >= UPLOAD_MAX_BACKENDS) {
        ESP_LOGE(TAG, "backend slots exhausted, cannot track '%s'", backend_id);
        return -1;
    }
    int slot = s_n_backends++;
    strlcpy(s_be_names[slot], backend_id, UPLOAD_BACKEND_ID_LEN);
    return slot;
}

const char *upload_index_backend_name(int slot)
{
    if (slot < 0 || slot >= s_n_backends) return "?";
    return s_be_names[slot];
}

int upload_index_backend_count(void)
{
    return s_n_backends;
}

/* ── Day / group access ───────────────────────────────────────────── */

/* Days are kept sorted newest-first so the progress window and pruning are
 * simple slices. */
static int find_day_idx(uint32_t day)
{
    for (int i = 0; i < s_n_days; i++) {
        if (s_days[i]->day == day) return i;
    }
    return -1;
}

upload_day_t *upload_index_day(uint32_t day, bool create)
{
    int i = find_day_idx(day);
    if (i >= 0) return s_days[i];
    if (!create) return NULL;

    if (s_n_days >= UPLOAD_MAX_DAYS_CAP) {
        /* Drop the oldest to make room; it is outside any sane window. */
        free(s_days[s_n_days - 1]);
        s_n_days--;
    }

    upload_day_t *d = idx_alloc(sizeof(upload_day_t));
    if (!d) {
        ESP_LOGE(TAG, "out of memory allocating day %08u", (unsigned)day);
        return NULL;
    }
    d->day = day;
    d->n_groups = 0;
    d->dirty = true;

    /* Insert keeping the newest-first order. */
    int pos = 0;
    while (pos < s_n_days && s_days[pos]->day > day) pos++;
    for (int k = s_n_days; k > pos; k--) s_days[k] = s_days[k - 1];
    s_days[pos] = d;
    s_n_days++;
    return d;
}

upload_group_t *upload_index_group(upload_day_t *d, uint32_t prefix_sec,
                                   bool create)
{
    if (!d) return NULL;
    for (int i = 0; i < d->n_groups; i++) {
        if (d->groups[i].prefix_sec == prefix_sec) return &d->groups[i];
    }
    if (!create) return NULL;
    if (d->n_groups >= UPLOAD_MAX_GROUPS_PER_DAY) {
        ESP_LOGW(TAG, "day %08u has more than %d groups — ignoring extras",
                 (unsigned)d->day, UPLOAD_MAX_GROUPS_PER_DAY);
        return NULL;
    }
    /* Keep groups in chronological order so uploads follow the night. */
    int pos = 0;
    while (pos < d->n_groups && d->groups[pos].prefix_sec < prefix_sec) pos++;
    for (int k = d->n_groups; k > pos; k--) d->groups[k] = d->groups[k - 1];
    memset(&d->groups[pos], 0, sizeof(upload_group_t));
    d->groups[pos].prefix_sec = prefix_sec;
    d->n_groups++;
    d->dirty = true;
    return &d->groups[pos];
}

void upload_index_drop_group(upload_day_t *d, uint32_t prefix_sec)
{
    if (!d) return;
    for (int i = 0; i < d->n_groups; i++) {
        if (d->groups[i].prefix_sec != prefix_sec) continue;
        for (int k = i; k < d->n_groups - 1; k++) d->groups[k] = d->groups[k + 1];
        d->n_groups--;
        d->dirty = true;
        return;
    }
}

int upload_index_day_count(void) { return s_n_days; }

upload_day_t *upload_index_day_at(int i)
{
    if (i < 0 || i >= s_n_days) return NULL;
    return s_days[i];
}

esp_err_t upload_index_forget_day(uint32_t day)
{
    char path[160];
    day_path(day, path, sizeof(path));
    unlink(path);

    int i = find_day_idx(day);
    if (i >= 0) {
        free(s_days[i]);
        for (int k = i; k < s_n_days - 1; k++) s_days[k] = s_days[k + 1];
        s_n_days--;
    }
    ESP_LOGI(TAG, "forgot day %08u — will re-upload", (unsigned)day);
    return ESP_OK;
}

/* ── Persistence ──────────────────────────────────────────────────── */

static esp_err_t write_json_atomic(const char *path, const char *json)
{
    char tmp[192];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

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

esp_err_t upload_index_save_day(upload_day_t *d)
{
    if (!d || !d->dirty) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", STATE_FILE_VERSION);

    char daystr[16];
    snprintf(daystr, sizeof(daystr), "%08u", (unsigned)d->day);
    cJSON_AddStringToObject(root, "day", daystr);

    cJSON *garr = cJSON_AddArrayToObject(root, "groups");
    for (int i = 0; i < d->n_groups; i++) {
        const upload_group_t *g = &d->groups[i];
        cJSON *go = cJSON_CreateObject();

        char pfx[8];
        sec_to_hhmmss(g->prefix_sec, pfx, sizeof(pfx));
        cJSON_AddStringToObject(go, "p", pfx);

        char kinds[32];
        kinds_to_str(g->kinds, kinds, sizeof(kinds));
        cJSON_AddStringToObject(go, "k", kinds);
        cJSON_AddNumberToObject(go, "n", g->n_files);

        /* Only backends with a recorded outcome are written; a missing
         * backend reads back as pending/0, which keeps files small and makes
         * "never attempted" visually obvious. */
        cJSON *be = NULL;
        for (int b = 0; b < s_n_backends; b++) {
            const upload_unit_t *u = &g->be[b];
            if (u->status == UG_PENDING && u->attempts == 0) continue;
            if (!be) be = cJSON_AddObjectToObject(go, "be");
            cJSON *bo = cJSON_AddObjectToObject(be, s_be_names[b]);
            cJSON_AddStringToObject(bo, "s", status_name(u->status));
            if (u->attempts) cJSON_AddNumberToObject(bo, "a", u->attempts);
            if (u->last_try_s) cJSON_AddNumberToObject(bo, "t", (double)u->last_try_s);
        }
        cJSON_AddItemToArray(garr, go);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    char path[160];
    day_path(d->day, path, sizeof(path));
    esp_err_t ret = write_json_atomic(path, json);
    free(json);

    if (ret == ESP_OK) d->dirty = false;
    return ret;
}

esp_err_t upload_index_save_all(void)
{
    esp_err_t last = ESP_OK;
    for (int i = 0; i < s_n_days; i++) {
        esp_err_t r = upload_index_save_day(s_days[i]);
        if (r != ESP_OK) last = r;
    }
    return last;
}

/* Read one day file into the index.  A malformed file is treated as absent:
 * the scan will re-derive the day, at worst re-uploading it. */
static void load_day_file(uint32_t day)
{
    char path[160];
    day_path(day, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 64 * 1024) { fclose(f); return; }

    char *buf = heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, size, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "%s is not valid JSON — day will be re-derived", path);
        cJSON_Delete(root);
        return;
    }

    cJSON *groups = cJSON_GetObjectItem(root, "groups");
    if (!groups || !cJSON_IsArray(groups)) { cJSON_Delete(root); return; }

    upload_day_t *d = upload_index_day(day, true);
    if (!d) { cJSON_Delete(root); return; }

    int n = cJSON_GetArraySize(groups);
    for (int i = 0; i < n; i++) {
        cJSON *go = cJSON_GetArrayItem(groups, i);
        if (!go) continue;
        cJSON *p = cJSON_GetObjectItem(go, "p");
        if (!p || !cJSON_IsString(p)) continue;

        upload_group_t *g = upload_index_group(d, hhmmss_to_sec(p->valuestring),
                                               true);
        if (!g) continue;

        cJSON *k = cJSON_GetObjectItem(go, "k");
        cJSON *nf = cJSON_GetObjectItem(go, "n");
        g->kinds = (k && cJSON_IsString(k)) ? kinds_from_str(k->valuestring) : 0;
        g->n_files = (nf && cJSON_IsNumber(nf)) ? (uint8_t)nf->valueint : 0;

        cJSON *be = cJSON_GetObjectItem(go, "be");
        if (!be) continue;
        for (cJSON *bo = be->child; bo; bo = bo->next) {
            if (!bo->string) continue;
            int slot = upload_index_backend_slot(bo->string);
            if (slot < 0) continue;
            cJSON *s = cJSON_GetObjectItem(bo, "s");
            cJSON *a = cJSON_GetObjectItem(bo, "a");
            cJSON *t = cJSON_GetObjectItem(bo, "t");
            g->be[slot].status = status_from_name(s && cJSON_IsString(s)
                                                 ? s->valuestring : NULL);
            g->be[slot].attempts = (a && cJSON_IsNumber(a)) ? (uint8_t)a->valueint : 0;
            g->be[slot].last_try_s = (t && cJSON_IsNumber(t))
                                     ? (uint32_t)t->valuedouble : 0;
        }
    }

    cJSON_Delete(root);
    d->dirty = false;
}

/* ── Bundle state ─────────────────────────────────────────────────── */

static void load_bundle_state(void)
{
    memset(s_bundle_ok_fp, 0, sizeof(s_bundle_ok_fp));

    FILE *f = fopen(UPLOAD_BUNDLE_STATE_PATH, "r");
    if (!f) return;
    char buf[512];
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return;
    for (cJSON *it = root->child; it; it = it->next) {
        if (!it->string || !cJSON_IsString(it)) continue;
        int slot = upload_index_backend_slot(it->string);
        if (slot < 0) continue;
        /* Stored as a hex string: a uint64 does not survive a JSON double. */
        s_bundle_ok_fp[slot] = strtoull(it->valuestring, NULL, 16);
    }
    cJSON_Delete(root);
}

static void save_bundle_state(void)
{
    cJSON *root = cJSON_CreateObject();
    for (int b = 0; b < s_n_backends; b++) {
        char hex[24];
        snprintf(hex, sizeof(hex), "%016llx",
                 (unsigned long long)s_bundle_ok_fp[b]);
        cJSON_AddStringToObject(root, s_be_names[b], hex);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;
    write_json_atomic(UPLOAD_BUNDLE_STATE_PATH, json);
    free(json);
}

uint64_t upload_index_bundle_ok_fp(int slot)
{
    if (slot < 0 || slot >= UPLOAD_MAX_BACKENDS) return 0;
    return s_bundle_ok_fp[slot];
}

void upload_index_set_bundle_ok(int slot, uint64_t fp)
{
    if (slot < 0 || slot >= UPLOAD_MAX_BACKENDS) return;
    if (s_bundle_ok_fp[slot] == fp) return;
    s_bundle_ok_fp[slot] = fp;
    save_bundle_state();
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

esp_err_t upload_index_init(void)
{
    if (s_ready) return ESP_OK;
    mkdir(UPLOAD_STATE_DIR, 0775);
    s_n_days = 0;
    s_ready = true;
    return ESP_OK;
}

esp_err_t upload_index_load(int max_days)
{
    if (!s_ready) upload_index_init();
    if (max_days <= 0) max_days = UPLOAD_DEFAULT_MAX_DAYS;
    if (max_days > UPLOAD_MAX_DAYS_CAP) max_days = UPLOAD_MAX_DAYS_CAP;

    /* Collect day numbers present on disk. */
    static uint32_t found[UPLOAD_MAX_DAYS_CAP];
    int n_found = 0;

    DIR *dir = opendir(UPLOAD_STATE_DIR);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && n_found < UPLOAD_MAX_DAYS_CAP) {
            size_t len = strlen(ent->d_name);
            if (len != 13 || strcmp(ent->d_name + 8, ".json") != 0) continue;
            bool digits = true;
            for (int i = 0; i < 8; i++) {
                if (ent->d_name[i] < '0' || ent->d_name[i] > '9') digits = false;
            }
            if (!digits) continue;
            found[n_found++] = (uint32_t)strtoul(ent->d_name, NULL, 10);
        }
        closedir(dir);
    }

    /* Newest first. */
    for (int i = 1; i < n_found; i++) {
        uint32_t v = found[i];
        int j = i - 1;
        while (j >= 0 && found[j] < v) { found[j + 1] = found[j]; j--; }
        found[j + 1] = v;
    }

    int loaded = 0;
    for (int i = 0; i < n_found; i++) {
        if (loaded < max_days) {
            load_day_file(found[i]);
            loaded++;
        } else {
            /* Outside the window: prune so the directory cannot grow without
             * bound as history accumulates. */
            char path[160];
            day_path(found[i], path, sizeof(path));
            unlink(path);
        }
    }

    load_bundle_state();

    ESP_LOGI(TAG, "loaded %d day(s) of upload state (window %d days)",
             loaded, max_days);
    return ESP_OK;
}

esp_err_t upload_index_clear(void)
{
    DIR *dir = opendir(UPLOAD_STATE_DIR);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            /* Sized for the worst case NAME_MAX entry so the compiler can
             * see the concatenation always fits. */
            char path[sizeof(UPLOAD_STATE_DIR) + 260];
            snprintf(path, sizeof(path), "%s/%s", UPLOAD_STATE_DIR, ent->d_name);
            unlink(path);
        }
        closedir(dir);
    }

    for (int i = 0; i < s_n_days; i++) free(s_days[i]);
    s_n_days = 0;
    memset(s_bundle_ok_fp, 0, sizeof(s_bundle_ok_fp));

    ESP_LOGI(TAG, "upload state cleared");
    return ESP_OK;
}

/* ── Aggregates ───────────────────────────────────────────────────── */

void upload_index_backend_progress(int slot, int max_days,
                                   int *out_days_done, int *out_days_total)
{
    int done = 0, total = 0;
    if (max_days <= 0) max_days = UPLOAD_DEFAULT_MAX_DAYS;

    for (int i = 0; i < s_n_days && i < max_days; i++) {
        upload_day_t *d = s_days[i];
        if (d->n_groups == 0) continue;
        total++;
        bool all_ok = true;
        for (int g = 0; g < d->n_groups; g++) {
            if (d->groups[g].be[slot].status != UG_OK) { all_ok = false; break; }
        }
        if (all_ok) done++;
    }
    if (out_days_done) *out_days_done = done;
    if (out_days_total) *out_days_total = total;
}

int upload_index_backend_pending(int slot, int max_days)
{
    int n = 0;
    if (max_days <= 0) max_days = UPLOAD_DEFAULT_MAX_DAYS;
    for (int i = 0; i < s_n_days && i < max_days; i++) {
        upload_day_t *d = s_days[i];
        for (int g = 0; g < d->n_groups; g++) {
            if (d->groups[g].be[slot].status != UG_OK) n++;
        }
    }
    return n;
}

esp_err_t upload_index_day_to_json(uint32_t day, char **out_json)
{
    if (!out_json) return ESP_ERR_INVALID_ARG;
    upload_day_t *d = upload_index_day(day, false);
    if (!d) {
        *out_json = strdup("{\"error\":\"day not tracked\"}");
        return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
    }

    cJSON *root = cJSON_CreateObject();
    char daystr[16];
    snprintf(daystr, sizeof(daystr), "%08u", (unsigned)d->day);
    cJSON_AddStringToObject(root, "day", daystr);
    cJSON *garr = cJSON_AddArrayToObject(root, "groups");

    for (int i = 0; i < d->n_groups; i++) {
        const upload_group_t *g = &d->groups[i];
        cJSON *go = cJSON_CreateObject();
        char pfx[8], kinds[32];
        sec_to_hhmmss(g->prefix_sec, pfx, sizeof(pfx));
        kinds_to_str(g->kinds, kinds, sizeof(kinds));
        cJSON_AddStringToObject(go, "prefix", pfx);
        cJSON_AddStringToObject(go, "kinds", kinds);
        cJSON_AddNumberToObject(go, "files", g->n_files);
        cJSON *be = cJSON_AddObjectToObject(go, "backends");
        for (int b = 0; b < s_n_backends; b++) {
            cJSON *bo = cJSON_AddObjectToObject(be, s_be_names[b]);
            cJSON_AddStringToObject(bo, "status", status_name(g->be[b].status));
            cJSON_AddNumberToObject(bo, "attempts", g->be[b].attempts);
            cJSON_AddNumberToObject(bo, "last_try_s", (double)g->be[b].last_try_s);
        }
        cJSON_AddItemToArray(garr, go);
    }

    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
}
