/*
 * SomnoTrace - Coverage of imported daily settings snapshots
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
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

#include "settings_provenance.h"
#include "sd_storage.h"
#include "esp_heap_caps.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static cJSON *read_manifest_checked(const char *path, esp_err_t *error)
{
    FILE *f = fopen(path, "rb");
    if (!f) { *error = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL; return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); *error = ESP_FAIL; return NULL; }
    long len = ftell(f);
    if (len <= 0 || len > 32768 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        *error = ESP_FAIL;
        return NULL;
    }
    char *buf = heap_caps_malloc((size_t)len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf && len <= 1024) buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); *error = ESP_ERR_NO_MEM; return NULL; }
    size_t n = fread(buf, 1, (size_t)len, f);
    bool ok = n == (size_t)len && !ferror(f);
    if (fclose(f) != 0) ok = false;
    buf[n] = '\0';
    errno = 0;
    cJSON *json = ok ? cJSON_Parse(buf) : NULL;
    int parse_errno = errno;
    free(buf);
    *error = json ? ESP_OK : (parse_errno == ENOMEM ? ESP_ERR_NO_MEM : ESP_FAIL);
    return json;
}

static cJSON *read_manifest(const char *path)
{
    esp_err_t error;
    return read_manifest_checked(path, &error);
}

/* Sorted IDs keep an unchanged repeated import byte-stable. */
static bool add_sorted_id(cJSON *arr, const char *id)
{
    int pos = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (cJSON_IsString(item) && item->valuestring &&
            strcmp(id, item->valuestring) < 0) break;
        pos++;
    }
    cJSON *value = cJSON_CreateString(id);
    if (!value) return false;
    if (!cJSON_InsertItemInArray(arr, pos, value)) {
        cJSON_Delete(value);
        return false;
    }
    return true;
}

static bool manifest_matches(const cJSON *manifest, const char *day,
                             const char *session, const char *serial,
                             int family, bool *imported, esp_err_t *error)
{
    if (!cJSON_IsObject(manifest)) return false;
    const cJSON *origin = cJSON_GetObjectItemCaseSensitive(manifest, "import_source");
    *imported = origin != NULL;
    if (origin && (!cJSON_IsString(origin) || !origin->valuestring ||
        (strcmp(origin->valuestring, "file_import") != 0 &&
         strcmp(origin->valuestring, "somnobridge") != 0))) return false;

    const cJSON *source = NULL;
    cJSON *ident = NULL;
    if (*imported) {
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(manifest,
                                                          "source_identity_verified")))
            return false;
        const cJSON *device_family = cJSON_GetObjectItemCaseSensitive(manifest,
                                                                       "source_device_family");
        const char *expected = settings_provenance_family_name(family);
        if (!expected || !cJSON_IsString(device_family) || !device_family->valuestring ||
            strcmp(expected, device_family->valuestring)) return false;
        source = cJSON_GetObjectItemCaseSensitive(manifest, "source_serial");
    } else {
        char path[420];
        int n = snprintf(path, sizeof(path), "%s/%s/%s_ident.json",
                         SD_STREAMS_DIR, day, session);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            *error = ESP_ERR_INVALID_SIZE;
            return false;
        }
        esp_err_t read_error;
        ident = read_manifest_checked(path, &read_error);
        if (read_error != ESP_OK && read_error != ESP_ERR_NOT_FOUND) {
            *error = read_error;
            return false;
        }
        if (settings_provenance_is(ident, SETTINGS_SOURCE_LIVE_IDENT, day, session) &&
            family != SETTINGS_FAMILY_UNKNOWN && settings_provenance_family(ident) == family)
            source = cJSON_GetObjectItemCaseSensitive(ident, "SerialNumber");
    }
    bool match = cJSON_IsString(source) && source->valuestring &&
                 strcmp(source->valuestring, serial) == 0;
    cJSON_Delete(ident);
    return match;
}

/* A bridge-imported session whose manifest predates verified identity.  It
 * carries no serial or family that could contradict the card, so the card's
 * own native record for the day is the only evidence it can rely on. */
static bool manifest_bridge_unverified(const cJSON *manifest, const char *serial, int family)
{
    if (!cJSON_IsObject(manifest)) return false;
    const cJSON *origin = cJSON_GetObjectItemCaseSensitive(manifest, "import_source");
    if (!cJSON_IsString(origin) || !origin->valuestring ||
        strcmp(origin->valuestring, "somnobridge") != 0) return false;
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(manifest, "source_identity_verified")))
        return false;
    const cJSON *sn = cJSON_GetObjectItemCaseSensitive(manifest, "source_serial");
    if (cJSON_IsString(sn) && sn->valuestring && sn->valuestring[0] &&
        (!serial || strcmp(sn->valuestring, serial) != 0)) return false;
    const cJSON *fam = cJSON_GetObjectItemCaseSensitive(manifest, "source_device_family");
    const char *want = settings_provenance_family_name(family);
    if (cJSON_IsString(fam) && fam->valuestring && fam->valuestring[0] &&
        (!want || strcmp(fam->valuestring, want) != 0)) return false;
    return true;
}

bool settings_provenance_session_is_bridge_unverified(const char *day, const char *session,
                                                      const char *serial, int family)
{
    if (!day || strlen(day) != 8 || !session || !session[0] || strchr(session, '/')) return false;
    char path[420];
    int n = snprintf(path, sizeof(path), "%s/%s/%s_session.json",
                     SD_STREAMS_DIR, day, session);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;
    cJSON *manifest = read_manifest(path);
    bool result = manifest_bridge_unverified(manifest, serial, family);
    cJSON_Delete(manifest);
    return result;
}

bool settings_provenance_session_matches(const char *day, const char *session,
                                        const char *serial, int family)
{
    if (!day || strlen(day) != 8 || !session || !session[0] ||
        strchr(session, '/') || !serial || !serial[0]) return false;
    char path[420];
    int n = snprintf(path, sizeof(path), "%s/%s/%s_session.json",
                     SD_STREAMS_DIR, day, session);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;
    cJSON *manifest = read_manifest(path);
    bool imported = false;
    esp_err_t error = ESP_OK;
    bool match = manifest_matches(manifest, day, session, serial, family, &imported, &error);
    cJSON_Delete(manifest);
    return match;
}

esp_err_t settings_provenance_session_checked(const char *day, const char *session,
                                              const char *serial, int family,
                                              bool *match, bool *bridge_unverified)
{
    if (!match || !bridge_unverified || !day || strlen(day) != 8 ||
        !session || !session[0] || strchr(session, '/'))
        return ESP_ERR_INVALID_ARG;
    *match = false;
    *bridge_unverified = false;
    char path[420];
    int n = snprintf(path, sizeof(path), "%s/%s/%s_session.json",
                     SD_STREAMS_DIR, day, session);
    if (n < 0 || (size_t)n >= sizeof(path)) return ESP_ERR_INVALID_SIZE;
    esp_err_t error = ESP_OK;
    cJSON *manifest = read_manifest_checked(path, &error);
    if (!manifest) return error;
    bool imported = false;
    if (serial && serial[0])
        *match = manifest_matches(manifest, day, session, serial, family, &imported, &error);
    if (error == ESP_OK && !*match)
        *bridge_unverified = manifest_bridge_unverified(manifest, serial, family);
    cJSON_Delete(manifest);
    return error;
}

static cJSON *local_session_ids_mode(const char *day, const char *serial, int family,
                                     bool tolerant, esp_err_t *error)
{
    *error = ESP_OK;
    if (!day || strlen(day) != 8 || !serial || !serial[0] ||
        family == SETTINGS_FAMILY_UNKNOWN) return NULL;
    for (int i = 0; i < 8; i++)
        if (day[i] < '0' || day[i] > '9') return NULL;
    char dir_path[300];
    int written = snprintf(dir_path, sizeof(dir_path), "%s/%s", SD_STREAMS_DIR, day);
    if (written < 0 || (size_t)written >= sizeof(dir_path)) {
        *error = ESP_ERR_INVALID_SIZE;
        return NULL;
    }
    cJSON *ids = cJSON_CreateArray();
    if (!ids) { *error = ESP_ERR_NO_MEM; return NULL; }
    DIR *dir = opendir(dir_path);
    if (!dir) {
        if (errno == ENOENT) return ids;
        *error = ESP_FAIL;
        cJSON_Delete(ids);
        return NULL;
    }
    bool ok = true;
    const char suffix[] = "_session.json";
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno != 0) { ok = false; *error = ESP_FAIL; }
            break;
        }
        size_t len = strlen(entry->d_name), tail = sizeof(suffix) - 1;
        if (len <= tail || strcmp(entry->d_name + len - tail, suffix) != 0) continue;
        size_t sid_len = len - tail;
        if (sid_len >= 100) { ok = false; *error = ESP_ERR_INVALID_SIZE; break; }
        char path[420], sid[100];
        size_t dir_len = strlen(dir_path);
        if (dir_len + 1 + len >= sizeof(path)) {
            ok = false; *error = ESP_ERR_INVALID_SIZE; break;
        }
        memcpy(path, dir_path, dir_len);
        path[dir_len] = '/';
        memcpy(path + dir_len + 1, entry->d_name, len + 1);
        cJSON *manifest = read_manifest_checked(path, error);
        if (!cJSON_IsObject(manifest)) {
            cJSON_Delete(manifest);
            if (*error == ESP_OK) *error = ESP_FAIL;
            ok = false; break;
        }
        memcpy(sid, entry->d_name, sid_len);
        sid[sid_len] = '\0';
        bool imported = false;
        ok = manifest_matches(manifest, day, sid, serial, family, &imported, error);
        if (!ok && *error == ESP_OK && tolerant &&
            manifest_bridge_unverified(manifest, serial, family)) {
            ok = true;
            imported = true;
            /* Counted as covered by the native record, not as local evidence. */
        }
        if (ok && !imported && !add_sorted_id(ids, sid)) {
            ok = false;
            *error = ESP_ERR_NO_MEM;
        }
        cJSON_Delete(manifest);
        if (!ok) break;
    }
    closedir(dir);
    if (!ok) { cJSON_Delete(ids); return NULL; }
    return ids;
}

/* Missing provenance is not a conflict, but an explicit foreign identity is. */
esp_err_t settings_provenance_day_conflicts_checked(const char *day, const char *serial,
                                                    int family, bool *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = true;
    if (!day || strlen(day) != 8 || !serial || !serial[0] ||
        !settings_provenance_family_name(family)) return ESP_OK;
    char dir_path[300];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", SD_STREAMS_DIR, day);
    DIR *dir = opendir(dir_path);
    if (!dir) {
        if (errno != ENOENT) return ESP_FAIL;
        *out = false;
        return ESP_OK;
    }
    bool conflict = false;
    esp_err_t error = ESP_OK;
    struct dirent *ent;
    const char suffix[] = "_session.json";
    for (;;) {
        errno = 0;
        ent = readdir(dir);
        if (!ent) {
            if (errno != 0) error = ESP_FAIL;
            break;
        }
        size_t len = strlen(ent->d_name), tail = sizeof(suffix) - 1;
        if (len <= tail || strcmp(ent->d_name + len - tail, suffix)) continue;
        char path[420];
        int n = snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);
        cJSON *manifest = NULL;
        if (n < 0 || (size_t)n >= sizeof(path)) error = ESP_ERR_INVALID_SIZE;
        else manifest = read_manifest_checked(path, &error);
        if (!cJSON_IsObject(manifest)) {
            cJSON_Delete(manifest);
            if (error == ESP_OK) error = ESP_FAIL;
            break;
        }
        const cJSON *sn = cJSON_GetObjectItemCaseSensitive(manifest, "source_serial");
        const cJSON *fam = cJSON_GetObjectItemCaseSensitive(manifest, "source_device_family");
        if ((sn && !cJSON_IsNull(sn) && (!cJSON_IsString(sn) || !sn->valuestring ||
                    (sn->valuestring[0] && strcmp(sn->valuestring, serial)))) ||
            (fam && !cJSON_IsNull(fam) && (!cJSON_IsString(fam) || !fam->valuestring ||
                     (fam->valuestring[0] &&
                      strcmp(fam->valuestring, settings_provenance_family_name(family)))))) {
            conflict = true;
        }
        cJSON_Delete(manifest);
        if (conflict) break;
        size_t sid_len = len - tail;
        if (sid_len >= 100) { error = ESP_ERR_INVALID_SIZE; break; }
        char sid[100];
        memcpy(sid, ent->d_name, sid_len);
        sid[sid_len] = '\0';
        n = snprintf(path, sizeof(path), "%s/%s_ident.json", dir_path, sid);
        cJSON *ident = NULL;
        if (n < 0 || (size_t)n >= sizeof(path)) error = ESP_ERR_INVALID_SIZE;
        else ident = read_manifest_checked(path, &error);
        if (error == ESP_ERR_NOT_FOUND) error = ESP_OK;
        if (error != ESP_OK) { cJSON_Delete(ident); break; }
        if (settings_provenance_is(ident, SETTINGS_SOURCE_LIVE_IDENT, day, sid)) {
            const cJSON *is = cJSON_GetObjectItemCaseSensitive(ident, "SerialNumber");
            if (!cJSON_IsString(is) || !is->valuestring || strcmp(is->valuestring, serial) ||
                settings_provenance_family(ident) != family) conflict = true;
        }
        cJSON_Delete(ident);
        if (conflict) break;
    }
    closedir(dir);
    if (error != ESP_OK) return error;
    *out = conflict;
    return ESP_OK;
}

bool settings_provenance_day_conflicts(const char *day, const char *serial, int family)
{
    bool conflict = true;
    return settings_provenance_day_conflicts_checked(day, serial, family, &conflict) != ESP_OK ||
           conflict;
}

bool settings_provenance_record_sessions(cJSON *root, const char *day)
{
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(root, SETTINGS_PROVENANCE_KEY);
    if (!cJSON_IsObject(meta)) return false;
    esp_err_t error;
    cJSON *ids = local_session_ids_mode(day, settings_provenance_serial(root),
                                        settings_provenance_recorded_family(root), true,
                                        &error);
    if (!ids) return false;
    cJSON_DeleteItemFromObjectCaseSensitive(meta, "local_sessions");
    cJSON_DeleteItemFromObjectCaseSensitive(meta, "coverage_verified");
    if (!cJSON_AddItemToObject(meta, "local_sessions", ids)) {
        cJSON_Delete(ids);
        return false;
    }
    if (!cJSON_AddBoolToObject(meta, "coverage_verified", true)) {
        cJSON_DeleteItemFromObjectCaseSensitive(meta, "local_sessions");
        return false;
    }
    return true;
}

esp_err_t settings_provenance_covers_sessions_checked(const cJSON *root, const char *day,
                                                      bool *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = false;
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(root, SETTINGS_PROVENANCE_KEY);
    const cJSON *recorded = cJSON_GetObjectItemCaseSensitive(meta, "local_sessions");
    if (!cJSON_IsArray(recorded) ||
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(meta, "coverage_verified")))
        return ESP_OK;
    esp_err_t error;
    cJSON *current = local_session_ids_mode(day, settings_provenance_serial(root),
                                            settings_provenance_recorded_family(root), true,
                                            &error);
    if (!current) return error;
    /* Compare the current local-session set against the recorded set.  An
     * empty recorded list covers only an empty current list: a later local
     * session must stale the snapshot until a re-import records it. */
    bool covered = true;
    const cJSON *sid;
    cJSON_ArrayForEach(sid, current) {
        bool found = false;
        const cJSON *old;
        cJSON_ArrayForEach(old, recorded) {
            if (cJSON_IsString(old) && old->valuestring &&
                cJSON_IsString(sid) && sid->valuestring &&
                strcmp(old->valuestring, sid->valuestring) == 0) {
                found = true;
                break;
            }
        }
        if (!found) { covered = false; break; }
    }
    cJSON_Delete(current);
    *out = covered;
    return ESP_OK;
}

bool settings_provenance_covers_sessions(const cJSON *root, const char *day)
{
    bool covered = false;
    return settings_provenance_covers_sessions_checked(root, day, &covered) == ESP_OK &&
           covered;
}
