/*
 * SomnoTrace - Canonical oximetry upload discovery and state
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3, or (at your option) any later version.
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

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "upload_index.h"

#define UPLOAD_OX_MAX_UNITS 64
#define UPLOAD_OX_MAX_FILES 8
#define UPLOAD_OX_ID_LEN 64
#define UPLOAD_OX_PATH_LEN 512
#define UPLOAD_OX_REL_LEN 96

typedef struct {
    char day[12];
    char recording_id[UPLOAD_OX_ID_LEN];
    char source_name[UPLOAD_OX_REL_LEN];
    uint32_t generation;
    char root_path[UPLOAD_OX_PATH_LEN];
    int n_files;
    char local_paths[UPLOAD_OX_MAX_FILES][UPLOAD_OX_PATH_LEN];
    char relative_paths[UPLOAD_OX_MAX_FILES][UPLOAD_OX_REL_LEN];
    uint64_t fingerprint;
} upload_ox_ref_t;

esp_err_t upload_ox_init(void);
int upload_ox_scan(upload_ox_ref_t *out, int max_out);
int upload_ox_reconcile(upload_ox_ref_t *out, int max_out, int max_days);
int upload_ox_status(const upload_ox_ref_t *ref, int backend_slot);
void upload_ox_mark(const upload_ox_ref_t *ref, int backend_slot,
                    upload_unit_status_t status, const char *remote_id);
esp_err_t upload_ox_save(void);
int upload_ox_pending(const upload_ox_ref_t *refs, int n_refs, int backend_slot);
int upload_ox_cached_pending(int backend_slot);
char *upload_ox_status_json(void);
