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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The root resources the importer consumes.  Bridge listings may expose a
 * different case (FAT short names), so classification is case-insensitive
 * while the exact exposed alias is retained for GET_FILE. */
#define SB_ROOT_NAME_COUNT 4

const char *sb_root_canonical_name(const char *name);
const char *sb_root_name_at(int index);
int sb_root_name_index(const char *name);
bool sb_root_name_has_path_separator(const char *name);

#define SB_ROOT_RECHECK_MS (6LL * 60 * 60 * 1000)
typedef enum {
    SB_ROOT_RESTORE,
    SB_ROOT_SKIP_COMMITTED,
    SB_ROOT_RECHECK,
} sb_root_cache_action_t;
sb_root_cache_action_t sb_root_cache_action(bool tuple_matches, bool committed,
                                            int64_t now_ms, int64_t *last_check_ms);
