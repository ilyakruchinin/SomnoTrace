/*
 * SomnoTrace - Immutable source object and observation store
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

#include "source_store.h"

#include "sd_storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef SOURCE_STORE_HOST
/* Host tests need no crypto library; production uses SHA-256 below. */
#else
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#endif

static char s_root[320];
static uint64_t s_budget = SOURCE_STORE_DEFAULT_BUDGET;
#ifndef SOURCE_STORE_HOST
static portMUX_TYPE s_store_spin = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_store_mutex_buffer;
static SemaphoreHandle_t s_store_mutex;
#endif

bool source_store_lock(void)
{
#ifdef SOURCE_STORE_HOST
    return true;
#else
    portENTER_CRITICAL(&s_store_spin);
    if (!s_store_mutex)
        s_store_mutex = xSemaphoreCreateMutexStatic(&s_store_mutex_buffer);
    portEXIT_CRITICAL(&s_store_spin);
    return s_store_mutex && xSemaphoreTake(s_store_mutex, portMAX_DELAY) == pdTRUE;
#endif
}

void source_store_unlock(void)
{
#ifndef SOURCE_STORE_HOST
    if (s_store_mutex) xSemaphoreGive(s_store_mutex);
#endif
}

static void *store_alloc(size_t n)
{
#ifdef SOURCE_STORE_HOST
    return malloc(n);
#else
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

static void content_hash(const uint8_t *data, size_t len, uint8_t out[32])
{
#ifdef SOURCE_STORE_HOST
    /* Four independent FNV-1a lanes: deterministic test identity only. */
    for (int lane = 0; lane < 4; lane++) {
        uint64_t h = 0xcbf29ce484222325ULL +
                     (uint64_t)lane * 0x9e3779b97f4a7c15ULL;
        for (size_t i = 0; i < len; i++) {
            h ^= data[i];
            h *= 0x100000001b3ULL;
        }
        for (int b = 0; b < 8; b++)
            out[lane * 8 + b] = (uint8_t)(h >> (8 * b));
    }
#else
    mbedtls_sha256(data, len, out, 0);
#endif
}

static void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

static bool id_valid(const char *id)
{
    if (!id || strlen(id) != SOURCE_STORE_ID_HEX) return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
            return false;
    return true;
}

static bool safe_component(const char *s)
{
    if (!s || !s[0] || strlen(s) > 96) return false;
    if (!strcmp(s, ".") || !strcmp(s, "..") || strstr(s, "..")) return false;
    for (const char *p = s; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.'))
            return false;
    }
    return true;
}

static int object_path(const char *id, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/objects/%s", s_root, id);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int observation_path(const char *id, const char *obs_id,
                            char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/observations/%s-%s.json",
                     s_root, id, obs_id);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

static esp_err_t read_file(const char *path, uint8_t **out, size_t *len)
{
    *out = NULL;
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return ESP_FAIL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return ESP_FAIL; }
    uint8_t *buf = store_alloc((size_t)n + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t got = n > 0 ? fread(buf, 1, (size_t)n, f) : 0;
    bool ok = got == (size_t)n;
    fclose(f);
    if (!ok) { free(buf); return ESP_FAIL; }
    buf[n] = '\0';
    *out = buf;
    *len = (size_t)n;
    return ESP_OK;
}

static esp_err_t write_file_atomic(const char *path, const void *data,
                                   size_t len)
{
    char tmp[420];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(tmp, "wb");
    if (!f) return ESP_FAIL;
    bool wr = len == 0 || fwrite(data, 1, len, f) == len;
    bool fl = fflush(f) == 0;
    bool fs = fsync(fileno(f)) == 0;
    bool cl = fclose(f) == 0;
    if (!(wr && fl && fs && cl)) {
        unlink(tmp);
        return ESP_FAIL;
    }
    if (rename(tmp, path) == 0) return ESP_OK;
    if (errno == EEXIST) {
        /* Idempotent publication: keep the existing identical file. */
        uint8_t *old = NULL;
        size_t old_len = 0;
        esp_err_t r = read_file(path, &old, &old_len);
        bool same = r == ESP_OK && old_len == len &&
                    (len == 0 || memcmp(old, data, len) == 0);
        free(old);
        unlink(tmp);
        return same ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    unlink(tmp);
    return ESP_FAIL;
}

static esp_err_t ensure_dir(const char *path)
{
    if (mkdir(path, 0775) == 0) return ESP_OK;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t source_store_set_budget(uint64_t bytes)
{
    if (bytes == 0) return ESP_ERR_INVALID_ARG;
    s_budget = bytes;
    return ESP_OK;
}

esp_err_t source_store_total_bytes(uint64_t *total)
{
    if (!s_root[0] || !total) return ESP_ERR_INVALID_ARG;
    *total = 0;
    char dir_path[360];
    if (snprintf(dir_path, sizeof(dir_path), "%s/objects", s_root) >=
        (int)sizeof(dir_path)) return ESP_ERR_INVALID_ARG;
    DIR *d = opendir(dir_path);
    if (!d) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!id_valid(ent->d_name)) continue;
        char path[420];
        if (snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name) >=
            (int)sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
            *total += (uint64_t)st.st_size;
    }
    closedir(d);
    return ESP_OK;
}

esp_err_t source_store_init(const char *root)
{
    const char *base = root && root[0] ? root : SD_SESSIONS_DIR "/sources";
    int n = snprintf(s_root, sizeof(s_root), "%s", base);
    if (n < 0 || (size_t)n >= sizeof(s_root)) return ESP_ERR_INVALID_ARG;
    char objects[360], observations[360];
    if (snprintf(objects, sizeof(objects), "%s/objects", s_root) >=
            (int)sizeof(objects) ||
        snprintf(observations, sizeof(observations), "%s/observations", s_root) >=
            (int)sizeof(observations))
        return ESP_ERR_INVALID_ARG;
    if (ensure_dir(s_root) != ESP_OK ||
        ensure_dir(objects) != ESP_OK ||
        ensure_dir(observations) != ESP_OK)
        return ESP_FAIL;
    return ESP_OK;
}

esp_err_t source_store_put(const char *kind, const uint8_t *data, size_t len,
                           source_store_put_result_t *out)
{
    if (!s_root[0] || !kind || !kind[0] || !data || len == 0 || !out)
        return ESP_ERR_INVALID_ARG;
    uint8_t hash[32];
    content_hash(data, len, hash);
    char id[SOURCE_STORE_ID_HEX + 1];
    hex_encode(hash, sizeof(hash), id);
    char path[360];
    if (object_path(id, path, sizeof(path)) != 0) return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode) || (uint64_t)st.st_size != len)
            return ESP_ERR_INVALID_CRC;
        esp_err_t v = source_store_verify(id, len);
        if (v != ESP_OK) return v;
        snprintf(out->id, sizeof(out->id), "%s", id);
        out->length = len;
        out->existed = true;
        return ESP_OK;
    }
    if (errno != ENOENT) return ESP_FAIL;
    uint64_t used = 0;
    if (source_store_total_bytes(&used) != ESP_OK) return ESP_FAIL;
    if (used + len > s_budget) return ESP_ERR_NO_MEM;
    esp_err_t w = write_file_atomic(path, data, len);
    if (w != ESP_OK) return w;
    snprintf(out->id, sizeof(out->id), "%s", id);
    out->length = len;
    out->existed = false;
    return ESP_OK;
}

esp_err_t source_store_put_path(const char *kind, const char *path,
                                source_store_put_result_t *out)
{
    if (!kind || !kind[0] || !path || !path[0] || !out)
        return ESP_ERR_INVALID_ARG;
    uint8_t *data = NULL;
    size_t len = 0;
    esp_err_t r = read_file(path, &data, &len);
    if (r != ESP_OK) return r;
    if (len == 0) { free(data); return ESP_ERR_INVALID_SIZE; }
    r = source_store_put(kind, data, len, out);
    free(data);
    return r;
}

esp_err_t source_store_path_matches(const char *id, const char *path)
{
    if (!id_valid(id) || !path || !path[0]) return ESP_ERR_INVALID_ARG;
    uint8_t *data = NULL;
    size_t len = 0;
    esp_err_t r = read_file(path, &data, &len);
    if (r != ESP_OK) return r;
    uint8_t hash[32];
    content_hash(data, len, hash);
    free(data);
    char got[SOURCE_STORE_ID_HEX + 1];
    hex_encode(hash, sizeof(hash), got);
    return strcmp(got, id) == 0 ? ESP_OK : ESP_ERR_INVALID_CRC;
}

esp_err_t source_store_get(const char *id, uint8_t *buf, size_t cap,
                           size_t *len_out)
{
    if (!s_root[0] || !id_valid(id) || !buf || !len_out)
        return ESP_ERR_INVALID_ARG;
    char path[360];
    if (object_path(id, path, sizeof(path)) != 0) return ESP_ERR_INVALID_ARG;
    uint8_t *data = NULL;
    size_t len = 0;
    esp_err_t r = read_file(path, &data, &len);
    if (r != ESP_OK) return r;
    if (len > cap) { free(data); return ESP_ERR_INVALID_SIZE; }
    uint8_t hash[32];
    content_hash(data, len, hash);
    char got[SOURCE_STORE_ID_HEX + 1];
    hex_encode(hash, sizeof(hash), got);
    if (strcmp(got, id) != 0) { free(data); return ESP_ERR_INVALID_CRC; }
    memcpy(buf, data, len);
    free(data);
    *len_out = len;
    return ESP_OK;
}

esp_err_t source_store_verify(const char *id, uint64_t expected_len)
{
    if (!s_root[0] || !id_valid(id)) return ESP_ERR_INVALID_ARG;
    char path[360];
    if (object_path(id, path, sizeof(path)) != 0) return ESP_ERR_INVALID_ARG;
    uint8_t *data = NULL;
    size_t len = 0;
    esp_err_t r = read_file(path, &data, &len);
    if (r != ESP_OK) return r;
    if ((uint64_t)len != expected_len) { free(data); return ESP_ERR_INVALID_SIZE; }
    uint8_t hash[32];
    content_hash(data, len, hash);
    char got[SOURCE_STORE_ID_HEX + 1];
    hex_encode(hash, sizeof(hash), got);
    free(data);
    return strcmp(got, id) == 0 ? ESP_OK : ESP_ERR_INVALID_CRC;
}

esp_err_t source_store_observe(const char *id, const char *observation_id,
                               const char *kind, const cJSON *observation)
{
    if (!s_root[0] || !id_valid(id) || !safe_component(observation_id) ||
        !kind || !kind[0] || !observation)
        return ESP_ERR_INVALID_ARG;
    char object[360], path[420];
    if (object_path(id, object, sizeof(object)) != 0 ||
        observation_path(id, observation_id, path, sizeof(path)) != 0)
        return ESP_ERR_INVALID_ARG;
    struct stat st;
    if (stat(object, &st) != 0 || !S_ISREG(st.st_mode))
        return ESP_ERR_NOT_FOUND;

    cJSON *env = cJSON_CreateObject();
    if (!env) return ESP_ERR_NO_MEM;
    cJSON *copy = cJSON_Duplicate(observation, true);
    bool ok = copy &&
              cJSON_AddNumberToObject(env, "schema", SOURCE_STORE_SCHEMA) &&
              cJSON_AddStringToObject(env, "object", id) &&
              cJSON_AddStringToObject(env, "observation_id", observation_id) &&
              cJSON_AddStringToObject(env, "kind", kind) &&
              cJSON_AddNumberToObject(env, "length", (double)st.st_size) &&
              cJSON_AddItemToObject(env, "observation", copy);
    if (!ok) {
        cJSON_Delete(copy);
        cJSON_Delete(env);
        return ESP_ERR_NO_MEM;
    }
    char *text = cJSON_PrintUnformatted(env);
    cJSON_Delete(env);
    if (!text) return ESP_ERR_NO_MEM;

    if (access(path, F_OK) == 0) {
        uint8_t *old = NULL;
        size_t old_len = 0;
        esp_err_t r = read_file(path, &old, &old_len);
        bool same = r == ESP_OK && old_len == strlen(text) &&
                    memcmp(old, text, old_len) == 0;
        free(old);
        free(text);
        return same ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    esp_err_t w = write_file_atomic(path, text, strlen(text));
    free(text);
    return w;
}

bool source_store_has_observation(const char *id, const char *observation_id,
                                  const char *kind)
{
    if (!s_root[0] || !id_valid(id) || !safe_component(observation_id) || !kind)
        return false;
    char path[420], object_path_buf[360];
    if (observation_path(id, observation_id, path, sizeof(path)) != 0 ||
        object_path(id, object_path_buf, sizeof(object_path_buf)) != 0)
        return false;
    struct stat st;
    if (stat(object_path_buf, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    uint8_t *text = NULL;
    size_t len = 0;
    if (read_file(path, &text, &len) != ESP_OK) return false;
    cJSON *root = cJSON_Parse((char *)text);
    free(text);
    const char *object = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "object"));
    const char *obs = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "observation_id"));
    const char *stored_kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "kind"));
    bool valid = object && !strcmp(object, id) && obs && !strcmp(obs, observation_id) &&
                 stored_kind && !strcmp(stored_kind, kind);
    cJSON_Delete(root);
    return valid;
}

static bool observation_str_eq(const cJSON *obs, const char *key,
                               const char *want)
{
    const cJSON *v = cJSON_GetObjectItem(obs, key);
    return cJSON_IsString(v) && v->valuestring && want &&
           strcmp(v->valuestring, want) == 0;
}

esp_err_t source_store_find(const char *kind, const char *key,
                            const char *size, const char *ts, bool ts_null,
                            char *id_out, size_t id_cap,
                            char *obs_out, size_t obs_cap)
{
    if (!s_root[0] || !kind || !kind[0] || !key || !key[0] ||
        !size || !size[0] || !id_out || id_cap == 0)
        return ESP_ERR_INVALID_ARG;
    if (!ts_null && (!ts || !ts[0])) return ESP_ERR_INVALID_ARG;

    char dir_path[360];
    if (snprintf(dir_path, sizeof(dir_path), "%s/observations", s_root) >=
        (int)sizeof(dir_path))
        return ESP_ERR_INVALID_ARG;
    DIR *d = opendir(dir_path);
    if (!d) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;

    bool found = false, ambiguous = false;
    int64_t best_ms = 0;
    char best_id[SOURCE_STORE_ID_HEX + 1] = "";
    char best_obs[128] = "";
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len <= 5 || strcmp(ent->d_name + len - 5, ".json") != 0) continue;
        char path[420];
        if (snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name) >=
            (int)sizeof(path)) continue;
        uint8_t *text = NULL;
        size_t text_len = 0;
        if (read_file(path, &text, &text_len) != ESP_OK) continue;
        cJSON *j = cJSON_Parse((char *)text);
        free(text);
        if (!j) continue;
        const char *env_kind = cJSON_GetStringValue(
            cJSON_GetObjectItem(j, "kind"));
        const char *object = cJSON_GetStringValue(
            cJSON_GetObjectItem(j, "object"));
        const char *obs_id = cJSON_GetStringValue(
            cJSON_GetObjectItem(j, "observation_id"));
        const cJSON *obs = cJSON_GetObjectItem(j, "observation");
        if (!env_kind || strcmp(env_kind, kind) != 0 ||
            !id_valid(object) || !obs_id || !cJSON_IsObject(obs) ||
            !observation_str_eq(obs, "key", key) ||
            !observation_str_eq(obs, "size", size)) {
            cJSON_Delete(j);
            continue;
        }
        const cJSON *tsv = cJSON_GetObjectItem(obs, "ts");
        if (ts_null ? !cJSON_IsNull(tsv)
                    : (!cJSON_IsString(tsv) || !tsv->valuestring ||
                       strcmp(tsv->valuestring, ts) != 0)) {
            cJSON_Delete(j);
            continue;
        }
        const cJSON *at = cJSON_GetObjectItem(obs, "observed_at_ms");
        int64_t ms = cJSON_IsNumber(at) ? (int64_t)at->valuedouble : 0;
        if (!found || ms > best_ms) {
            found = true;
            ambiguous = false;
            best_ms = ms;
            snprintf(best_id, sizeof(best_id), "%s", object);
            snprintf(best_obs, sizeof(best_obs), "%s", obs_id);
        } else if (ms == best_ms && strcmp(best_id, object) != 0) {
            ambiguous = true;
        }
        cJSON_Delete(j);
    }
    closedir(d);
    if (!found) return ESP_ERR_NOT_FOUND;
    if (ambiguous) return ESP_ERR_INVALID_STATE;
    if (strlen(best_id) >= id_cap ||
        (obs_out && strlen(best_obs) >= obs_cap))
        return ESP_ERR_INVALID_SIZE;
    snprintf(id_out, id_cap, "%s", best_id);
    if (obs_out) snprintf(obs_out, obs_cap, "%s", best_obs);
    return ESP_OK;
}

esp_err_t source_store_copy_to(const char *id, const char *dest)
{
    if (!s_root[0] || !id_valid(id) || !dest || !dest[0])
        return ESP_ERR_INVALID_ARG;
    char path[360];
    if (object_path(id, path, sizeof(path)) != 0) return ESP_ERR_INVALID_ARG;
    uint8_t *data = NULL;
    size_t len = 0;
    esp_err_t r = read_file(path, &data, &len);
    if (r != ESP_OK) return r;
    uint8_t hash[32];
    content_hash(data, len, hash);
    char got[SOURCE_STORE_ID_HEX + 1];
    hex_encode(hash, sizeof(hash), got);
    if (strcmp(got, id) != 0) { free(data); return ESP_ERR_INVALID_CRC; }
    esp_err_t w = write_file_atomic(dest, data, len);
    free(data);
    return w;
}

typedef struct {
    char (*ids)[SOURCE_STORE_ID_HEX + 1];
    int n;
    int cap;
} ref_list_t;

static bool ref_has(const ref_list_t *r, const char *id)
{
    for (int i = 0; i < r->n; i++)
        if (!strcmp(r->ids[i], id)) return true;
    return false;
}

static bool ref_add(ref_list_t *r, const char *id)
{
    if (ref_has(r, id)) return true;
    if (r->n >= r->cap) {
        int cap = r->cap ? r->cap * 2 : 64;
        if (cap > 8192) return false;
        void *p = realloc(r->ids, (size_t)cap * (SOURCE_STORE_ID_HEX + 1));
        if (!p) return false;
        r->ids = p;
        r->cap = cap;
    }
    snprintf(r->ids[r->n], SOURCE_STORE_ID_HEX + 1, "%s", id);
    r->n++;
    return true;
}

static esp_err_t collect_refs(ref_list_t *refs)
{
    char dir_path[360];
    if (snprintf(dir_path, sizeof(dir_path), "%s/observations", s_root) >=
        (int)sizeof(dir_path))
        return ESP_ERR_INVALID_ARG;
    DIR *d = opendir(dir_path);
    if (!d) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    esp_err_t ret = ESP_OK;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len <= 5 || strcmp(ent->d_name + len - 5, ".json") != 0) continue;
        char path[420];
        if (snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name) >=
            (int)sizeof(path)) { ret = ESP_ERR_INVALID_ARG; break; }
        uint8_t *text = NULL;
        size_t text_len = 0;
        esp_err_t r = read_file(path, &text, &text_len);
        if (r != ESP_OK) { ret = r; break; }
        cJSON *j = cJSON_Parse((char *)text);
        free(text);
        const char *id = j ? cJSON_GetStringValue(
                                cJSON_GetObjectItem(j, "object")) : NULL;
        if (!id || !id_valid(id) || !ref_add(refs, id)) {
            cJSON_Delete(j);
            ret = ESP_ERR_INVALID_CRC;
            break;
        }
        cJSON_Delete(j);
    }
    closedir(d);
    return ret;
}

esp_err_t source_store_prune_orphans(int *removed)
{
    if (!s_root[0] || !removed) return ESP_ERR_INVALID_ARG;
    *removed = 0;
    ref_list_t refs = {0};
    esp_err_t r = collect_refs(&refs);
    if (r != ESP_OK) { free(refs.ids); return r; }

    char dir_path[360];
    if (snprintf(dir_path, sizeof(dir_path), "%s/objects", s_root) >=
        (int)sizeof(dir_path)) { free(refs.ids); return ESP_ERR_INVALID_ARG; }
    DIR *d = opendir(dir_path);
    if (!d) { free(refs.ids); return errno == ENOENT ? ESP_OK : ESP_FAIL; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        char path[420];
        if (snprintf(path, sizeof(path), "%s/%s", dir_path, name) >=
            (int)sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        bool tmp = strstr(name, ".tmp") != NULL;
        bool orphan = id_valid(name) && !ref_has(&refs, name);
        if (tmp || orphan) {
            if (unlink(path) == 0) (*removed)++;
        }
    }
    closedir(d);
    free(refs.ids);
    return ESP_OK;
}

esp_err_t source_store_count_objects(int *count)
{
    if (!s_root[0] || !count) return ESP_ERR_INVALID_ARG;
    *count = 0;
    char dir_path[360];
    if (snprintf(dir_path, sizeof(dir_path), "%s/objects", s_root) >=
        (int)sizeof(dir_path)) return ESP_ERR_INVALID_ARG;
    DIR *d = opendir(dir_path);
    if (!d) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!id_valid(ent->d_name)) continue;
        char path[420];
        if (snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name) >=
            (int)sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) (*count)++;
    }
    closedir(d);
    return ESP_OK;
}
