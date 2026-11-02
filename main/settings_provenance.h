/*
 * SomnoTrace - Provenance for daily therapy settings snapshots
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

#pragma once

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

#define SETTINGS_PROVENANCE_KEY "_somnotrace_provenance"
#define SETTINGS_SOURCE_LIVE "live"
#define SETTINGS_SOURCE_CACHED "cached"
#define SETTINGS_SOURCE_IMPORTED_STR "imported_str"
#define SETTINGS_FAMILY_UNKNOWN 0
#define SETTINGS_FAMILY_AS10 10
#define SETTINGS_FAMILY_AS11 11

/* Imported settings must retain the machine identity that supplied them. */
static inline bool settings_provenance_set_serial(cJSON *root, const char *serial)
{
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(root, SETTINGS_PROVENANCE_KEY);
    if (!cJSON_IsObject(meta) || !serial || !serial[0]) return false;
    cJSON_DeleteItemFromObjectCaseSensitive(meta, "serial");
    return cJSON_AddStringToObject(meta, "serial", serial) != NULL;
}

static inline const char *settings_provenance_serial(const cJSON *root)
{
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(root, SETTINGS_PROVENANCE_KEY);
    const cJSON *serial = cJSON_GetObjectItemCaseSensitive(meta, "serial");
    return cJSON_IsString(serial) && serial->valuestring && serial->valuestring[0]
           ? serial->valuestring : NULL;
}

static inline int settings_provenance_family(const cJSON *ident)
{
    const cJSON *mid = cJSON_GetObjectItemCaseSensitive(ident, "PlatformIdentifier");
    long value;
    if (cJSON_IsNumber(mid)) {
        value = mid->valueint;
    } else if (cJSON_IsString(mid) && mid->valuestring && mid->valuestring[0]) {
        char *end = NULL;
        value = strtol(mid->valuestring, &end, 10);
        if (!end || *end) return SETTINGS_FAMILY_UNKNOWN;
    } else {
        return SETTINGS_FAMILY_UNKNOWN;
    }
    if (value <= 0 || value > 9999) return SETTINGS_FAMILY_UNKNOWN;
    return value >= 40 ? SETTINGS_FAMILY_AS11 : SETTINGS_FAMILY_AS10;
}

static inline const char *settings_provenance_family_name(int family)
{
    if (family == SETTINGS_FAMILY_AS10) return "AS10";
    if (family == SETTINGS_FAMILY_AS11) return "AS11";
    return NULL;
}

static inline bool settings_provenance_set_family(cJSON *root, int family)
{
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(root, SETTINGS_PROVENANCE_KEY);
    const char *name = settings_provenance_family_name(family);
    if (!cJSON_IsObject(meta) || !name) return false;
    cJSON_DeleteItemFromObjectCaseSensitive(meta, "family");
    return cJSON_AddStringToObject(meta, "family", name) != NULL;
}

static inline bool settings_provenance_is_family(const cJSON *root, int family)
{
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(root, SETTINGS_PROVENANCE_KEY);
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(meta, "family");
    const char *name = settings_provenance_family_name(family);
    return name && cJSON_IsString(value) && value->valuestring &&
           strcmp(value->valuestring, name) == 0;
}

/* Metadata travels in the same JSON file as the settings, so a partial write
 * cannot accidentally pair a new tree with an old trust marker. */
static inline bool settings_provenance_set(cJSON *root, const char *source,
                                           const char *day, const char *session)
{
    if (!cJSON_IsObject(root) || !source || !day || strlen(day) != 8) return false;
    cJSON *meta = cJSON_CreateObject();
    if (!meta) return false;
    if (!cJSON_AddNumberToObject(meta, "version", 1) ||
        !cJSON_AddStringToObject(meta, "source", source) ||
        !cJSON_AddStringToObject(meta, "day", day) ||
        (session && !cJSON_AddStringToObject(meta, "session", session))) {
        cJSON_Delete(meta);
        return false;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(root, SETTINGS_PROVENANCE_KEY);
    if (!cJSON_AddItemToObject(root, SETTINGS_PROVENANCE_KEY, meta)) {
        cJSON_Delete(meta);
        return false;
    }
    return true;
}

static inline bool settings_provenance_is(const cJSON *root, const char *source,
                                          const char *day, const char *session)
{
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(root, SETTINGS_PROVENANCE_KEY);
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(meta, "version");
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(meta, "source");
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(meta, "day");
    const cJSON *sid = cJSON_GetObjectItemCaseSensitive(meta, "session");
    return cJSON_IsObject(meta) && cJSON_IsNumber(version) && version->valuedouble == 1 &&
           cJSON_IsString(src) && src->valuestring && strcmp(src->valuestring, source) == 0 &&
           cJSON_IsString(d) && d->valuestring && strcmp(d->valuestring, day) == 0 &&
           (!session || (cJSON_IsString(sid) && sid->valuestring &&
                         strcmp(sid->valuestring, session) == 0));
}

/* An imported daily record can become stale if another local session is
 * recorded after the import. Track which local sessions existed at import. */
bool settings_provenance_record_sessions(cJSON *root, const char *day);
bool settings_provenance_covers_sessions(const cJSON *root, const char *day);
bool settings_provenance_session_matches(const char *day, const char *session,
                                        const char *serial);
