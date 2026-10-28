/*
 * SomnoTrace - EDF → SNT import engine (historical ResMed card imports)
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
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────
 *  Import inbox (/somnotrace/IMPORT/)
 *
 *  Users drop ResMed card content into IMPORT/ — a full card layout
 *  (Identification.json|tgt + STR.edf + SETTINGS/ + DATALOG/<day>/EDFs),
 *  bare day folders (IMPORT/<YYYYMMDD>/EDFs), or flat session EDFs.
 *  SomnoBridge (Phase 2) stages downloads + <day>/_bridge_drift.json the
 *  same way.
 *
 *  The engine converts each session cluster into native .snt streams +
 *  events.snt + ident/settings/session manifests under
 *  .somnotrace/sessions/streams/<day>/, writes summaries/<day>.spool +
 *  <day>.settings.json from STR.edf (or synthesises them from imported
 *  sessions), and only then deletes the consumed source files.
 * ──────────────────────────────────────────────────────────────────── */

/* One processed "unit" — either the root-files pass (STR/ident/settings),
 * or one day-group of session EDFs.  The worker rebuilds + reuploads the
 * day's export after each day unit. */
typedef struct {
    char day_label[9];      /* primary streams noon-day "YYYYMMDD", '\0' = root */
    char day2_label[9];     /* second streams day touched, if any, else "" */
    bool root_pass;         /* this unit was the root-files pass */
    int  sessions_imported;
    int  sessions_skipped;  /* already present / duplicates */
    int  sessions_failed;
    int  files_consumed;    /* source files deleted after durable commit */
    int  files_rejected;    /* corrupt files moved aside to *.bad */
    int  spools_written;
    bool deferred;          /* nothing processed: the next unit's files were
                             * modified within the quiescence window (a copy
                             * may still be in flight) — call again later */
} edf_import_unit_t;

/* Opaque scan context: keeps the resolved identification/settings trees and
 * the list of days touched by this run (for gap spool synthesis). */
typedef struct edf_import_ctx edf_import_ctx_t;

/* True when IMPORT/ currently holds any importable content (cheap check
 * used by the worker trigger). */
bool edf_import_pending(void);

/* Remove stale *.part files and any incomplete <id>_* import artifacts
 * (where <id>_session.json.importing exists without <id>_session.json).
 * Safe to call at boot before session_writer_recover(). */
void edf_import_cleanup_residue(void);

edf_import_ctx_t *edf_import_ctx_create(void);
void              edf_import_ctx_destroy(edf_import_ctx_t *ctx);

/* Quiescence window: a unit whose source files (or drift sidecar) were
 * modified less than `seconds` ago is deferred (edf_import_unit_t.deferred)
 * instead of imported — the user may still be copying the card.  Default is
 * 15 s on target and 0 (off) under EDF_IMPORT_HOST.  An explicit "import
 * now" trigger can pass 0.  Files dated in the future (unset clocks) never
 * count as recent. */
void edf_import_ctx_set_quiesce(edf_import_ctx_t *ctx, int seconds);

/* Process exactly one pending unit.
 *   ESP_OK            — a unit was processed (or, with out->deferred set,
 *                       held back for quiescence); *out describes it.
 *   ESP_ERR_NOT_FOUND — the inbox is empty and all follow-up work is done.
 *   other errors      — partial progress was still made (see *out); safe
 *                       to call again. */
esp_err_t edf_import_process_next(edf_import_ctx_t *ctx, edf_import_unit_t *out);

#ifndef EDF_IMPORT_HOST
/* Spawn the background import worker: waits for SD + settles FTP/quiescence
 * windows, drains the inbox unit-by-unit under the EXPORT lease, and kicks
 * edf_gen_rebuild_day() + uploader_on_export_complete() per imported day.
 * Idempotent — a second call is a no-op. */
esp_err_t edf_import_start_worker(void);

/* Re-run the inbox scan now (used after SomnoBridge stages a day, and by the
 * Phase-3 "Import now" button).  Safe to call while the worker is running:
 * it is a no-op then, and restarts a worker that has already exited. */
void edf_import_kick_now(void);
#endif

#ifdef __cplusplus
}
#endif
