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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "cJSON.h"

#define OX_RECORDINGS_DIR SD_MOUNT_POINT "/.somnotrace/oximetry/recordings"
#define OX_STATE_PATH UPLOAD_STATE_DIR "/oximetry.json"
#define OX_STATE_TMP  UPLOAD_STATE_DIR "/oximetry.json.tmp"
#define OX_MAX_BACKENDS_LOCAL UPLOAD_MAX_BACKENDS

typedef struct {
    bool used;
    char id[UPLOAD_OX_ID_LEN];
    uint32_t generation;
    uint64_t fingerprint;
    upload_unit_t backend[OX_MAX_BACKENDS_LOCAL];
    char remote[OX_MAX_BACKENDS_LOCAL][64];
} ox_state_t;

static ox_state_t s_states[UPLOAD_OX_MAX_UNITS];
static bool s_loaded;

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

static int state_find(const char *id, uint32_t generation)
{
    for (int i = 0; i < UPLOAD_OX_MAX_UNITS; i++)
        if (s_states[i].used && s_states[i].generation == generation &&
            strcmp(s_states[i].id, id) == 0) return i;
    return -1;
}

static int state_get(const upload_ox_ref_t *ref, bool create)
{
    int i = state_find(ref->recording_id, ref->generation);
    if (i >= 0 || !create) return i;
    for (i = 0; i < UPLOAD_OX_MAX_UNITS; i++) {
        if (s_states[i].used) continue;
        memset(&s_states[i], 0, sizeof(s_states[i]));
        s_states[i].used = true;
        strlcpy(s_states[i].id, ref->recording_id, sizeof(s_states[i].id));
        s_states[i].generation = ref->generation;
        s_states[i].fingerprint = ref->fingerprint;
        return i;
    }
    return -1;
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

static cJSON *read_state(void)
{
    FILE *f = fopen(OX_STATE_PATH, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 256 * 1024) { fclose(f); return NULL; }
    char *buf = heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)size, f); fclose(f); buf[n] = '\0';
    cJSON *root = n == (size_t)size ? cJSON_Parse(buf) : NULL;
    free(buf); return root;
}

esp_err_t upload_ox_init(void)
{
    if (s_loaded) return ESP_OK;
    memset(s_states, 0, sizeof(s_states));
    cJSON *root = read_state();
    cJSON *units = root ? cJSON_GetObjectItem(root, "units") : NULL;
    if (units && cJSON_IsArray(units)) {
        cJSON *item; cJSON_ArrayForEach(item, units) {
            cJSON *id = cJSON_GetObjectItem(item, "recording_id");
            cJSON *gen = cJSON_GetObjectItem(item, "generation");
            cJSON *fp = cJSON_GetObjectItem(item, "fingerprint");
            if (!cJSON_IsString(id) || !cJSON_IsNumber(gen) ||
                (!cJSON_IsNumber(fp) && !cJSON_IsString(fp))) continue;
            upload_ox_ref_t dummy; memset(&dummy, 0, sizeof(dummy));
            strlcpy(dummy.recording_id, id->valuestring, sizeof(dummy.recording_id));
            dummy.generation = (uint32_t)gen->valuedouble;
            int slot = state_get(&dummy, true); if (slot < 0) continue;
            s_states[slot].fingerprint = cJSON_IsString(fp)
                ? strtoull(fp->valuestring, NULL, 16)
                : (uint64_t)fp->valuedouble;
            cJSON *bes = cJSON_GetObjectItem(item, "backends");
            if (!bes || !cJSON_IsArray(bes)) continue;
            cJSON *be; cJSON_ArrayForEach(be, bes) {
                cJSON *bi = cJSON_GetObjectItem(be, "slot");
                cJSON *bs = cJSON_GetObjectItem(be, "status");
                if (!cJSON_IsNumber(bi) || !cJSON_IsString(bs) || bi->valueint < 0 || bi->valueint >= OX_MAX_BACKENDS_LOCAL) continue;
                int b = bi->valueint;
                s_states[slot].backend[b].status = status_parse(bs->valuestring);
                cJSON *at = cJSON_GetObjectItem(be, "attempts");
                cJSON *lt = cJSON_GetObjectItem(be, "last_try_s");
                if (cJSON_IsNumber(at)) s_states[slot].backend[b].attempts = (uint8_t)at->valueint;
                if (cJSON_IsNumber(lt)) s_states[slot].backend[b].last_try_s = (uint32_t)lt->valuedouble;
                cJSON *ri = cJSON_GetObjectItem(be, "remote_id");
                if (cJSON_IsString(ri)) strlcpy(s_states[slot].remote[b], ri->valuestring, sizeof(s_states[slot].remote[b]));
            }
        }
    }
    if (root) cJSON_Delete(root);
    s_loaded = true;
    return ESP_OK;
}

esp_err_t upload_ox_save(void)
{
    if (!s_loaded) return ESP_ERR_INVALID_STATE;
    cJSON *root = cJSON_CreateObject(); if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON *units = cJSON_AddArrayToObject(root, "units");
    for (int i = 0; i < UPLOAD_OX_MAX_UNITS; i++) {
        if (!s_states[i].used) continue;
        cJSON *u = cJSON_CreateObject();
        cJSON_AddStringToObject(u, "recording_id", s_states[i].id);
        cJSON_AddNumberToObject(u, "generation", s_states[i].generation);
        char fp[17];
        snprintf(fp, sizeof(fp), "%016llx", (unsigned long long)s_states[i].fingerprint);
        cJSON_AddStringToObject(u, "fingerprint", fp);
        cJSON *bes = cJSON_AddArrayToObject(u, "backends");
        for (int b = 0; b < OX_MAX_BACKENDS_LOCAL; b++) {
            if (s_states[i].backend[b].status == UG_PENDING && s_states[i].backend[b].attempts == 0 && !s_states[i].remote[b][0]) continue;
            cJSON *be = cJSON_CreateObject();
            cJSON_AddNumberToObject(be, "slot", b);
            cJSON_AddStringToObject(be, "status", status_name(s_states[i].backend[b].status));
            cJSON_AddNumberToObject(be, "attempts", s_states[i].backend[b].attempts);
            cJSON_AddNumberToObject(be, "last_try_s", s_states[i].backend[b].last_try_s);
            if (s_states[i].remote[b][0]) cJSON_AddStringToObject(be, "remote_id", s_states[i].remote[b]);
            cJSON_AddItemToArray(bes, be);
        }
        cJSON_AddItemToArray(units, u);
    }
    char *text = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    if (!text) return ESP_ERR_NO_MEM;
    FILE *f = fopen(OX_STATE_TMP, "w");
    bool ok = f && fwrite(text, 1, strlen(text), f) == strlen(text) && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (f) fclose(f);
    cJSON_free(text);
    if (ok) { unlink(OX_STATE_PATH); /* FATFS cannot rename-over */ ok = (rename(OX_STATE_TMP, OX_STATE_PATH) == 0); }
    if (!ok) { unlink(OX_STATE_TMP); return ESP_FAIL; }
    return ESP_OK;
}

static int scan_day(const char *day, upload_ox_ref_t *out, int max_out)
{
    char day_path[UPLOAD_OX_PATH_LEN];
    if (!valid_day(day) || !join2(day_path, sizeof(day_path), OX_RECORDINGS_DIR, day)) return 0;
    DIR *d = opendir(day_path); if (!d) return 0;
    int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < max_out) {
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

int upload_ox_scan(upload_ox_ref_t *out, int max_out)
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

    int n = 0;
    for (int i = 0; i < nd && n < max_out; i++) {
        n += scan_day(days[i], &out[n], max_out - n);
    }
    free(days);
    return n;
}

int upload_ox_reconcile(upload_ox_ref_t *out, int max_out, int max_days)
{
    int n = upload_ox_scan(out, max_out);
    if (max_days < 1) max_days = 1;
    int kept = 0, days_seen = 0; char last_day[12] = {0};
    for (int i = 0; i < n; i++) {
        if (strcmp(last_day, out[i].day) != 0) {
            strlcpy(last_day, out[i].day, sizeof(last_day)); days_seen++;
        }
        if (days_seen > max_days) continue;
        if (kept != i) out[kept] = out[i];
        int slot = state_get(&out[kept], true); if (slot < 0) { kept++; continue; }
        if (s_states[slot].fingerprint != out[kept].fingerprint) {
            s_states[slot].fingerprint = out[kept].fingerprint;
            memset(s_states[slot].backend, 0, sizeof(s_states[slot].backend));
            memset(s_states[slot].remote, 0, sizeof(s_states[slot].remote));
        }
        kept++;
    }
    upload_ox_save(); return kept;
}

int upload_ox_status(const upload_ox_ref_t *ref, int backend_slot)
{
    if (!ref || backend_slot < 0 || backend_slot >= OX_MAX_BACKENDS_LOCAL) return UG_PENDING;
    int i = state_get(ref, true); return i < 0 ? UG_PENDING : s_states[i].backend[backend_slot].status;
}

void upload_ox_mark(const upload_ox_ref_t *ref, int backend_slot,
                   upload_unit_status_t status, const char *remote_id)
{
    if (!ref || backend_slot < 0 || backend_slot >= OX_MAX_BACKENDS_LOCAL) return;
    int i = state_get(ref, true); if (i < 0) return;
    s_states[i].backend[backend_slot].status = status;
    s_states[i].backend[backend_slot].last_try_s = (uint32_t)time(NULL);
    if (status != UG_OK) s_states[i].backend[backend_slot].attempts++;
    if (remote_id) strlcpy(s_states[i].remote[backend_slot], remote_id, sizeof(s_states[i].remote[backend_slot]));
    upload_ox_save();
}

int upload_ox_pending(const upload_ox_ref_t *refs, int n_refs, int backend_slot)
{
    int n = 0; for (int i = 0; i < n_refs; i++) if (upload_ox_status(&refs[i], backend_slot) != UG_OK) n++; return n;
}

int upload_ox_cached_pending(int backend_slot)
{
    if (!s_loaded) upload_ox_init();
    if (!s_loaded || backend_slot < 0 || backend_slot >= OX_MAX_BACKENDS_LOCAL) return 0;
    int n = 0;
    for (int i = 0; i < UPLOAD_OX_MAX_UNITS; i++) {
        if (s_states[i].used && s_states[i].backend[backend_slot].status != UG_OK) {
            n++;
        }
    }
    return n;
}

char *upload_ox_status_json(void)
{
    upload_ox_ref_t *refs = heap_caps_malloc(sizeof(upload_ox_ref_t) * UPLOAD_OX_MAX_UNITS, MALLOC_CAP_SPIRAM);
    if (!refs) return NULL;
    int n = upload_ox_scan(refs, UPLOAD_OX_MAX_UNITS);
    cJSON *arr = cJSON_CreateArray(); if (!arr) { free(refs); return NULL; }
    for (int i = 0; i < n; i++) {
        cJSON *u = cJSON_CreateObject();
        cJSON_AddStringToObject(u, "recording_id", refs[i].recording_id);
        cJSON_AddStringToObject(u, "day", refs[i].day);
        cJSON_AddNumberToObject(u, "generation", refs[i].generation);
        cJSON *bes = cJSON_AddArrayToObject(u, "backends");
        for (int b = 0; b < OX_MAX_BACKENDS_LOCAL; b++) {
            cJSON *be = cJSON_CreateObject();
            cJSON_AddNumberToObject(be, "slot", b);
            cJSON_AddStringToObject(be, "id", upload_index_backend_name(b));
            cJSON_AddStringToObject(be, "status", status_name(upload_ox_status(&refs[i], b)));
            cJSON_AddItemToArray(bes, be);
        }
        cJSON_AddItemToArray(arr, u);
    }
    char *out = cJSON_PrintUnformatted(arr); cJSON_Delete(arr); free(refs); return out;
}
