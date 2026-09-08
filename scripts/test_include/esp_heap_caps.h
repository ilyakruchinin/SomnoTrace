/*
 * SomnoTrace - Minimal esp_heap_caps test shim for host unit tests
 * Copyright (C) 2026 Plantucha <https://github.com/Plantucha>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
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

#pragma once

#include <stdlib.h>

/* PSRAM vs internal placement is irrelevant on the host: plain malloc. */
#define MALLOC_CAP_SPIRAM    0
#define MALLOC_CAP_INTERNAL  0
#define MALLOC_CAP_8BIT      0
#define MALLOC_CAP_DEFAULT   0

#define heap_caps_malloc(size, caps)        malloc(size)
#define heap_caps_calloc(n, size, caps)     calloc((n), (size))
#define heap_caps_realloc(p, size, caps)    realloc((p), (size))
#define heap_caps_free(p)                   free(p)
#define heap_caps_get_free_size(caps)           ((size_t)(64u << 20))
#define heap_caps_get_largest_free_block(caps)  ((size_t)(64u << 20))
