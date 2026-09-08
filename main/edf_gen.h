/*
 * SomnoTrace - EDF file generation from session data warehouse
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

#include "esp_err.h"

/* ── EDF file generation ──────────────────────────────────────────────
 *
 * This module generates EDF (European Data Format) files from session
 * data, producing output compatible with OSCAR and SleepHQ.
 *
 * Input data lives under the SomnoTrace app-root (ESP-native):
 *   /somnotrace/.somnotrace/sessions/streams/YYYYMMDD/
 *     <prefix>_brp.snt, _sa2.snt, _pld.snt, _brp_mm.snt  ← stream data
 *     <prefix>_events.snt                                 ← live events
 *     <prefix>_resp_events.bin                             ← TherapyEvents spool
 *     <prefix>_ident.json                                  ← device identity
 *     <prefix>_settings.json                               ← current settings
 *     <prefix>_session.json                                ← session metadata
 *   /somnotrace/.somnotrace/sessions/summaries/
 *     YYYYMMDD.spool                                       ← per-day Summary spool
 *
 * Output goes to SDCARD/ (ResMed-compatible SD card image, OSCAR-ready):
 *   /somnotrace/SDCARD/
 *     STR.edf                ← Multi-record daily summary (one record per day
 *                              from .somnotrace/sessions/summaries/ spool files, sorted chronologically)
 *     Identification.json    ← Device identity (nested AS11 format)
 *     Identification.crc     ← CRC-32 of Identification.json
 *     SETTINGS/
 *       CurrentSettings.json ← Latest settings snapshot
 *       CurrentSettings.crc  ← CRC-32 of CurrentSettings.json
 *     DATALOG/
 *       YYYYMMDD/            ← Noon-based day folder
 *         <prefix>_BRP.edf   ← Breath waveform 25 Hz (from brp.snt)
 *         <prefix>_PLD.edf   ← Per-breath stats 0.5 Hz (from pld.snt)
 *         <prefix>_SA2.edf   ← SpO2/pulse 1 Hz (from sa2.snt)
 *         <prefix>_EVE.edf   ← Respiratory event annotations
 *         <prefix>_CSL.edf   ← CSR event annotations
 *
 * SDCARD/ is fully derived from .somnotrace/sessions/ and can be deleted and regenerated
 * at any time without BLE access.
 *
 * This function is blocking and should be called from a task with adequate
 * stack (8KB+).  It must be called AFTER post_therapy_collect() has completed,
 * so that all spool and RPC data is available.
 *
 * Parameters:
 *   session_dir    - path to the noon-day stream folder (.somnotrace/sessions/streams/YYYYMMDD/)
 *   session_id     - session prefix (e.g. "20260627_224219" or "20260627_224219_2")
 *   start_epoch_ms - session start time in epoch ms (NTP-corrected)
 *   end_epoch_ms   - session end time in epoch ms (0 if unknown)
 *   clock_drift_ms - NTP time - AS11 device time (positive = AS11 is behind).
 *                    Applied to spool-sourced timestamps (Summary, events)
 *                    to correct for AS11 clock skew.  Stream .snt data is
 *                    already NTP-timestamped and needs no adjustment.
 */
/* Build a per-noon-day JSON summary (AHI/indices, usage, leak/pressure/EPAP/
 * resp-rate percentiles, session count) from that day's Summary spool, in
 * physical units matching STR.edf/OSCAR. On success returns ESP_OK and sets
 * *out_json to a malloc'd string (caller frees). Returns ESP_ERR_NOT_FOUND if
 * the day has no spool. noon_day is "YYYYMMDD". */
esp_err_t edf_gen_summary_json(const char *noon_day, char **out_json);

esp_err_t edf_gen_generate(const char *session_dir, const char *session_id,
                           int64_t start_epoch_ms, int64_t end_epoch_ms,
                           int64_t clock_drift_ms);

/* ── Split generation passes ───────────────────────────────────────────
 * A generation call writes two kinds of artifact:
 *
 *   PER_SESSION  BRP/PLD/SA2/EVE/CSL under <out_root>/DATALOG/<noon-day>/
 *   SHARED       <out_root>/STR.edf, Identification.json/.crc and
 *                SETTINGS/CurrentSettings.json/.crc
 *
 * The shared artifacts are NOT session-local: STR.edf is a multi-day
 * cumulative file, and when no Summary spool covers the day its record is
 * synthesised from the single session passed in.  Rebuilding a day by
 * calling the generator once per session would therefore leave STR
 * reflecting whichever session happened to be last.  Splitting the passes
 * makes the aggregation explicit: run PER_SESSION for every session, then
 * SHARED exactly once. */
#define EDF_GEN_PER_SESSION   (1u << 0)
#define EDF_GEN_SHARED        (1u << 1)
#define EDF_GEN_ALL           (EDF_GEN_PER_SESSION | EDF_GEN_SHARED)

/* As edf_gen_generate(), but writes into out_root and honours the pass
 * flags.  out_root must be an existing-or-creatable directory; pass
 * SD_SDCARD_DIR for the live export. */
esp_err_t edf_gen_generate_ex(const char *out_root,
                              const char *session_dir, const char *session_id,
                              int64_t start_epoch_ms, int64_t end_epoch_ms,
                              int64_t clock_drift_ms, uint32_t flags);

/* Rebuild the export for exactly one noon-day, as a transaction.
 *
 * Generates every session of that day into a staging directory, and only
 * swaps it into place once all of them succeeded — so a partial failure
 * leaves the previous good export untouched.  Then runs the shared pass
 * once.  Takes the storage export lease for the whole operation, so it
 * cannot run concurrently with another export, an upload of the same day,
 * or a destructive action.
 *
 * day_folder is "YYYYMMDD" (noon-based).  Returns ESP_OK only if the day
 * was fully rebuilt and published — the caller may then queue it for
 * upload. */
esp_err_t edf_gen_rebuild_day(const char *day_folder);

/* Boot check: was a day rebuild interrupted while publishing?
 *
 * Publication deletes the live day and moves the staged files into place, so
 * a reset in that window leaves the day incomplete.  Returns true and fills
 * out_day (8 chars + NUL) when such a day is found, consuming the marker so
 * the caller owns the retry.  Call once at boot, before anything reads the
 * export tree. */
bool edf_gen_take_interrupted_rebuild(char *out_day, size_t out_len);
