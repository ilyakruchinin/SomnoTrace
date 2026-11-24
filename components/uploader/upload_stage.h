/*
 * SomnoTrace - SomnoStage CSV upload discovery and state
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

/* Tracks the consumer-facing Zeo CSV files that somno_ml exports into
 * .somnotrace/stages/<YYYYMMDD>/<unit_id>.csv — one unit per scored
 * recording (ring) or session (AS11).  A stage unit exists only after its
 * source was scored, so stages arrive later than CPAP/oximetry data and are
 * tracked independently: rescoring changes the CSV fingerprint, which
 * resets every backend's unit to pending without touching CPAP or ox state.
 *
 * Modelled on upload_ox: grow-on-demand state list, one JSON file per unit
 * under upload_state/stage/, backends keyed by name.  Backends that cannot
 * consume stages (supports_stage == false) simply leave their units
 * pending — enabling the capability later backfills automatically. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "upload_index.h"

/* Bounds the ref array returned per pass — same trade-off as oximetry:
 * exceeding this only hides the oldest units from one pass, the persisted
 * state itself is unbounded. */
#define UPLOAD_STAGE_MAX_UNITS 768
#define UPLOAD_STAGE_ID_LEN    64
#define UPLOAD_STAGE_PATH_LEN  192

/* Stage representation consumed by a backend:
 *   UPLOAD_STAGE_FMT_CSV — Zeo-compatible CSV in stages/<day>/<id>.csv (SMB)
 *   UPLOAD_STAGE_FMT_SST — Native SST1 binary sidecar (Aerivue, BreathingRoom, …) */
typedef enum {
    UPLOAD_STAGE_FMT_CSV = 0,
    UPLOAD_STAGE_FMT_SST = 1,
} upload_stage_fmt_t;

/* One stage unit as it exists on the card, ready to hand to a backend.
 * csv_path is the Zeo-compatible export (tracked via `fingerprint`);
 * sst_path locates the internal SST1 sidecar (tracked via `sst_fingerprint`).
 * Each backend slot is bound to the format it consumes so CSV and SST
 * destinations are tracked and invalidated independently. */
typedef struct {
    char     day[12];                         /* YYYYMMDD folder          */
    char     unit_id[UPLOAD_STAGE_ID_LEN];    /* <rec_id> | as11_<prefix> */
    char     csv_path[UPLOAD_STAGE_PATH_LEN];
    char     sst_path[UPLOAD_STAGE_PATH_LEN];
    uint64_t fingerprint;                     /* CSV content fingerprint  */
    uint64_t sst_fingerprint;                 /* SST content fingerprint  */
} upload_stage_ref_t;

esp_err_t upload_stage_init(void);
void      upload_stage_set_backend_fmt(int backend_slot, upload_stage_fmt_t fmt);
upload_stage_fmt_t upload_stage_get_backend_fmt(int backend_slot);
bool      upload_stage_ref_has_fmt(const upload_stage_ref_t *ref,
                                   upload_stage_fmt_t fmt);
void      upload_stage_sst_filename(const upload_stage_ref_t *ref,
                                    char *out, size_t n);
/* True when a backend that wants an explicit time zone should send one: the
 * unit id carries no local time, or epoch 0 of the SST sits more than five
 * minutes from the id's local stamp (BreathingRoom derives the offset from
 * that stamp, snapped to 15 minutes).  Pure — host-testable. */
bool      upload_stage_needs_timezone(const upload_stage_ref_t *ref);
int       upload_stage_scan(upload_stage_ref_t *out, int max_out);
int       upload_stage_reconcile(upload_stage_ref_t *out, int max_out,
                                 int max_days);
int       upload_stage_status(const upload_stage_ref_t *ref, int backend_slot);
void      upload_stage_mark(const upload_stage_ref_t *ref, int backend_slot,
                            upload_unit_status_t status, const char *remote_id);
/* Same park rule as EDF groups (upload_park.h): a unit failed
 * UPLOAD_MAX_GROUP_ATTEMPTS times is skipped by dispatch and excluded from
 * pending until the parking goes stale. */
bool      upload_stage_parked(const upload_stage_ref_t *ref, int backend_slot);
int       upload_stage_pending(const upload_stage_ref_t *refs, int n_refs,
                               int backend_slot);
int       upload_stage_cached_pending(int backend_slot);
void      upload_stage_backend_progress(int backend_slot, int max_days,
                                        int *out_done, int *out_total);
/* "Reset Upload State" hook: clears on-disk unit files AND the in-memory
 * list so every stage CSV inside the window is re-offered (#317 parity). */
esp_err_t upload_stage_clear(void);
