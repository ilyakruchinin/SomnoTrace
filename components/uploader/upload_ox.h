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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "upload_index.h"

/* Bounds the ref array returned by upload_ox_scan()/reconcile() — one ref is
 * ~1.3 KB (paths), allocs are transient PSRAM.  This is NOT a state cap:
 * upload state is a grow-on-demand list persisted per recording, so exceeding
 * this only hides the oldest recordings from one pass (a warning is logged). */
#define UPLOAD_OX_MAX_UNITS 768
#define UPLOAD_OX_MAX_FILES 4
#define UPLOAD_OX_ID_LEN 64
#define UPLOAD_OX_PATH_LEN 192
#define UPLOAD_OX_REL_LEN 48

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
/* Same park rule as EDF groups (upload_park.h): a unit failed
 * UPLOAD_MAX_GROUP_ATTEMPTS times is skipped by dispatch and excluded from
 * pending until the parking goes stale. */
bool upload_ox_parked(const upload_ox_ref_t *ref, int backend_slot);
int upload_ox_pending(const upload_ox_ref_t *refs, int n_refs, int backend_slot);
int upload_ox_cached_pending(int backend_slot);
void upload_ox_backend_progress(int backend_slot, int max_days,
                                int *out_done, int *out_total);
/* "Reset Upload State" hook (#317): clears the per-recording state files
 * under upload_state/ox AND the in-memory list, so the next reconcile
 * re-offers every recording inside the window.  upload_index_clear()
 * alone cannot reach this subdirectory. */
esp_err_t upload_ox_clear(void);
char *upload_ox_status_json(void);
