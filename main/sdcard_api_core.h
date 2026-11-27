/*
 * SomnoTrace - Pure path validation and directory walk for read-only SD API
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDCARD_API_MAX_RELPATH 192
#define SDCARD_API_MAX_DEPTH   4

/* Validate a relative path under SDCARD/ (no leading/trailing '/', no '\\',
 * no empty/dot/dot-dot segments, no hidden '.'-prefixed segments, no control
 * chars, length < SDCARD_API_MAX_RELPATH). */
bool sdcard_api_relpath_valid(const char *relpath);

/* Construct "<root>/<relpath>" into out, rejecting invalid relpath or
 * truncation. Trailing '/' on root is trimmed before joining. */
bool sdcard_api_build_path(const char *root, const char *relpath, char *out, size_t out_cap);

/* JSON-escape src into dst (without outer quotes). Returns bytes written
 * (excluding NUL) or -1 on invalid input or insufficient capacity. */
int sdcard_api_json_escape(const char *src, char *dst, size_t dst_cap);

typedef int (*sdcard_api_file_cb_t)(const char *relpath, uint64_t size, int64_t mtime, void *ctx);

/* Iteratively walk root (or root/sub_dir when sub_dir is non-empty), skipping
 * hidden entries and staying within SDCARD_API_MAX_DEPTH. Invokes cb for each
 * regular file; non-zero callback return aborts and propagates immediately.
 * Returns 0 on success or -errno on error. */
int sdcard_api_walk(const char *root, const char *sub_dir, sdcard_api_file_cb_t cb, void *ctx);
