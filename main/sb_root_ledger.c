/*
 * SomnoTrace - SomnoBridge root ledger helpers
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

#include "sb_root.h"

#include <strings.h>

#include "cJSON.h"

cJSON *sb_root_ledger_latest(cJSON *root, const char *key, size_t *count)
{
    if (count) *count = 0;
    if (!cJSON_IsObject(root) || !key || !key[0]) return NULL;
    cJSON *latest = NULL;
    for (cJSON *it = root->child; it; it = it->next) {
        if (it->string && !strcasecmp(it->string, key)) {
            latest = it;
            if (count) (*count)++;
        }
    }
    return latest;
}

bool sb_root_ledger_replace(cJSON *root, const char *key, const char *size,
                            const char *ts, bool ts_null)
{
    if (!cJSON_IsObject(root) || !key || !key[0] || !size || !size[0] ||
        (!ts_null && (!ts || !ts[0]))) return false;
    cJSON *entry = cJSON_CreateObject();
    if (!entry) return false;
    if (!cJSON_AddStringToObject(entry, "size", size) ||
        !(ts_null ? cJSON_AddNullToObject(entry, "ts")
                  : cJSON_AddStringToObject(entry, "ts", ts)) ||
        !cJSON_AddItemToObject(root, key, entry)) {
        cJSON_Delete(entry);
        return false;
    }
    for (cJSON *it = root->child; it;) {
        cJSON *next = it->next;
        if (it != entry && it->string && !strcasecmp(it->string, key))
            cJSON_Delete(cJSON_DetachItemViaPointer(root, it));
        it = next;
    }
    return true;
}
