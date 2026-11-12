/*
 * SomnoTrace - Synthetic imported summary spool marker
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
#include <stdio.h>
#include <string.h>

/* Unknown protobuf field 50000 (wire type 2), followed by a 19-byte tag. */
#define IMPORT_SPOOL_MARKER "\x82\xb5\x18\x13" "SOMNOTRACE_SYNTH_V1"
#define IMPORT_SPOOL_MARKER_LEN (sizeof(IMPORT_SPOOL_MARKER) - 1)

static inline bool import_spool_is_synthetic(const char *path)
{
    unsigned char tail[IMPORT_SPOOL_MARKER_LEN];
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool match = fseek(f, -(long)sizeof(tail), SEEK_END) == 0 &&
                 fread(tail, 1, sizeof(tail), f) == sizeof(tail) &&
                 memcmp(tail, IMPORT_SPOOL_MARKER, sizeof(tail)) == 0;
    fclose(f);
    return match;
}
