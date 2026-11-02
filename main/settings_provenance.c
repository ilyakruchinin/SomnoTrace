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

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static cJSON *read_manifest(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len <= 0 || len > 32768 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *json = n == (size_t)len ? cJSON_Parse(buf) : NULL;
    free(buf);
    return json;
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

bool settings_provenance_session_matches(const char *day, const char *session,
                                        const char *serial)
{
    if (!day || strlen(day) != 8 || !session || !session[0] ||
        strchr(session, '/') || !serial || !serial[0]) return false;
    char path[420];
    int n = snprintf(path, sizeof(path), "%s/%s/%s_session.json",
                     SD_STREAMS_DIR, day, session);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;
    cJSON *manifest = read_manifest(path);
    if (!cJSON_IsObject(manifest)) { cJSON_Delete(manifest); return false; }
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(manifest, "source_serial");
    cJSON *ident = NULL;
    if (!cJSON_IsString(source) || !source->valuestring || !source->valuestring[0]) {
        n = snprintf(path, sizeof(path), "%s/%s/%s_ident.json",
                     SD_STREAMS_DIR, day, session);
        if (n >= 0 && (size_t)n < sizeof(path)) ident = read_manifest(path);
        source = cJSON_GetObjectItemCaseSensitive(ident, "SerialNumber");
    }
    bool match = cJSON_IsString(source) && source->valuestring &&
                 strcmp(source->valuestring, serial) == 0;
    cJSON_Delete(ident);
    cJSON_Delete(manifest);
    return match;
}

static cJSON *local_session_ids(const char *day, const char *serial)
{
    if (!day || strlen(day) != 8 || !serial || !serial[0]) return NULL;
    for (int i = 0; i < 8; i++)
        if (day[i] < '0' || day[i] > '9') return NULL;
    char dir_path[300];
    int written = snprintf(dir_path, sizeof(dir_path), "%s/%s", SD_STREAMS_DIR, day);
    if (written < 0 || (size_t)written >= sizeof(dir_path)) return NULL;
    cJSON *ids = cJSON_CreateArray();
    if (!ids) return NULL;
    DIR *dir = opendir(dir_path);
    if (!dir) {
        if (errno == ENOENT) return ids;
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
            if (errno != 0) ok = false;
            break;
        }
        size_t len = strlen(entry->d_name), tail = sizeof(suffix) - 1;
        if (len <= tail || strcmp(entry->d_name + len - tail, suffix) != 0) continue;
        size_t sid_len = len - tail;
        if (sid_len >= 100) { ok = false; break; }
        char path[420], sid[100];
        size_t dir_len = strlen(dir_path);
        if (dir_len + 1 + len >= sizeof(path)) { ok = false; break; }
        memcpy(path, dir_path, dir_len);
        path[dir_len] = '/';
        memcpy(path + dir_len + 1, entry->d_name, len + 1);
        cJSON *manifest = read_manifest(path);
        if (!cJSON_IsObject(manifest)) { cJSON_Delete(manifest); ok = false; break; }
        memcpy(sid, entry->d_name, sid_len);
        sid[sid_len] = '\0';
        const cJSON *imported = cJSON_GetObjectItemCaseSensitive(manifest, "import_source");
        if (imported && (!cJSON_IsString(imported) || !imported->valuestring ||
                   (strcmp(imported->valuestring, "file_import") != 0 &&
                    strcmp(imported->valuestring, "somnobridge") != 0))) {
            ok = false;
        } else {
            const cJSON *source = cJSON_GetObjectItemCaseSensitive(manifest, "source_serial");
            cJSON *ident = NULL;
            if (!cJSON_IsString(source) || !source->valuestring || !source->valuestring[0]) {
                int n = snprintf(path, sizeof(path), "%s/%s_ident.json", dir_path, sid);
                if (n >= 0 && (size_t)n < sizeof(path)) ident = read_manifest(path);
                source = cJSON_GetObjectItemCaseSensitive(ident, "SerialNumber");
            }
            ok = cJSON_IsString(source) && source->valuestring &&
                 strcmp(source->valuestring, serial) == 0;
            if (ok && !imported) ok = add_sorted_id(ids, sid);
            cJSON_Delete(ident);
        }
        cJSON_Delete(manifest);
        if (!ok) break;
    }
    closedir(dir);
    if (!ok) { cJSON_Delete(ids); return NULL; }
    return ids;
}

bool settings_provenance_record_sessions(cJSON *root, const char *day)
{
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(root, SETTINGS_PROVENANCE_KEY);
    if (!cJSON_IsObject(meta)) return false;
    cJSON *ids = local_session_ids(day, settings_provenance_serial(root));
    if (!ids) return false;
    cJSON_DeleteItemFromObjectCaseSensitive(meta, "local_sessions");
    if (!cJSON_AddItemToObject(meta, "local_sessions", ids)) {
        cJSON_Delete(ids);
        return false;
    }
    return true;
}

bool settings_provenance_covers_sessions(const cJSON *root, const char *day)
{
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(root, SETTINGS_PROVENANCE_KEY);
    const cJSON *recorded = cJSON_GetObjectItemCaseSensitive(meta, "local_sessions");
    if (!cJSON_IsArray(recorded)) return false;
    cJSON *current = local_session_ids(day, settings_provenance_serial(root));
    if (!current) return false;
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
    return covered;
}
