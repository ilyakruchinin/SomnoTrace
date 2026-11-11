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

/* Opt-in fault injection: a test that DEFINES this flag and sets it non-zero
 * makes heap_caps_malloc() return NULL, as an exhausted PSRAM pool does on the
 * device, so the caller's plain-malloc fallback actually runs.  Weak, so every
 * test that does not define it links unchanged and gets plain malloc. */
extern int host_heap_caps_malloc_fail __attribute__((weak));
extern int host_heap_caps_calloc_fail __attribute__((weak));
extern int host_heap_caps_realloc_fail __attribute__((weak));
#define heap_caps_malloc(size, caps)                                         \
    ((&host_heap_caps_malloc_fail && host_heap_caps_malloc_fail)             \
         ? NULL : malloc(size))
#define heap_caps_calloc(n, size, caps)                                      \
    ((&host_heap_caps_calloc_fail && host_heap_caps_calloc_fail)             \
         ? NULL : calloc((n), (size)))
#define heap_caps_realloc(p, size, caps)                                    \
    ((&host_heap_caps_realloc_fail && host_heap_caps_realloc_fail)           \
         ? NULL : realloc((p), (size)))
#define heap_caps_free(p)                   free(p)
#define heap_caps_get_free_size(caps)           ((size_t)(64u << 20))
#define heap_caps_get_largest_free_block(caps)  ((size_t)(64u << 20))
