/*
 * SomnoTrace - SomnoBridge root resource name canonicalisation
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

#include <string.h>
#include <strings.h>

static const char *const k_root_names[SB_ROOT_NAME_COUNT] = {
    "STR.edf", "Identification.json", "Identification.tgt", "CurrentSettings.json"
};

int sb_root_name_index(const char *name)
{
    if (!name || !name[0]) return -1;
    for (int i = 0; i < SB_ROOT_NAME_COUNT; i++)
        if (strcasecmp(name, k_root_names[i]) == 0) return i;
    return -1;
}

const char *sb_root_canonical_name(const char *name)
{
    int idx = sb_root_name_index(name);
    return idx >= 0 ? k_root_names[idx] : NULL;
}

const char *sb_root_name_at(int index)
{
    return index >= 0 && index < SB_ROOT_NAME_COUNT ? k_root_names[index] : NULL;
}

bool sb_root_name_has_path_separator(const char *name)
{
    return name && (strchr(name, '/') || strchr(name, '\\'));
}

sb_root_cache_action_t sb_root_cache_action(bool tuple_matches, bool committed,
                                            int64_t now_ms, int64_t *last_check_ms)
{
    if (!tuple_matches || !committed || !last_check_ms) return SB_ROOT_RESTORE;
    if (*last_check_ms <= 0 || now_ms < *last_check_ms) {
        *last_check_ms = now_ms > 0 ? now_ms : 1;
        return SB_ROOT_SKIP_COMMITTED;
    }
    return now_ms - *last_check_ms >= SB_ROOT_RECHECK_MS
           ? SB_ROOT_RECHECK : SB_ROOT_SKIP_COMMITTED;
}
