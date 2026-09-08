/*
 * SomnoTrace - Post-therapy data collection from AS11 spools and RPC
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

#include "esp_err.h"
#include <stdbool.h>

/* ── Post-therapy data collection ─────────────────────────────────────
 *
 * After therapy stops, the AS11 device has internal spools containing
 * session summary data (for STR.edf) and respiratory event logs (for
 * EVE.edf).  Additionally, device identification fields (serial number,
 * model, variant) are needed for EDF headers and Identification.json.
 *
 * This module orchestrates the collection of all post-therapy data:
 *
 *   Summary spool (30-day lookback):
 *     → decoded per-day → .somnotrace/sessions/summaries/YYYYMMDD.spool
 *
 *   TherapyEvents-RespiratoryEvents spool:
 *     → .somnotrace/sessions/streams/YYYYMMDD/<prefix>_resp_events.bin
 *
 *   Device identification (Get RPC):
 *     → .somnotrace/sessions/streams/YYYYMMDD/<prefix>_ident.json
 *
 *   Current settings (Get RPC):
 *     → .somnotrace/sessions/streams/YYYYMMDD/<prefix>_settings.json
 *
 * post_therapy_collect() is blocking and should be called from a task
 * with adequate stack (8KB+).  It is invoked by stop_task after
 * session_writer_stop() has finalised the stream files.
 *
 * After collection, *spool_current indicates whether the current day's
 * Summary spool record is fresh (ClockB >= session end - 5s tolerance).
 * If stale, the caller should call post_therapy_wait_spool_current()
 * from a separate low-priority task before generating STR.edf.
 *
 * Parameters:
 *   session_dir    - path to the noon-day folder (e.g. ".somnotrace/sessions/streams/20260627")
 *   file_prefix    - session file prefix (e.g. "20260627_023000")
 *   start_epoch_ms - session start time in epoch ms (unused for lookback;
 *                    30-day fixed window is used instead)
 *   clock_drift_ms - NTP time - AS11 device time (positive = AS11 is behind).
 *                    Saved to manifest.json for use by EDF generation.
 *   end_epoch_ms   - session end time in epoch ms (NTP clock).  Used for
 *                    spool staleness detection (ClockB comparison).
 *   spool_current  - output: true if the current day's spool is fresh.
 */
esp_err_t post_therapy_collect(const char *session_dir, const char *file_prefix,
                               int64_t start_epoch_ms, int64_t clock_drift_ms,
                               int64_t end_epoch_ms, bool *spool_current);

/* Retry pulling the current day's Summary spool until fresh or timeout.
 *
 * Retries every 3 seconds for up to 2 minutes (40 attempts).  Each retry
 * pulls only the current noon-day's record (fromDateTime = noon today)
 * for speed.  Uses ClockB (field 40) with 5-second tolerance against
 * the AS11-equivalent session end time for staleness detection.
 *
 * Should be called from a low-priority task on core 0 so it doesn't
 * block EDF generation of other files or new therapy notifications.
 *
 * Parameters:
 *   end_epoch_ms   - session end time in epoch ms (NTP clock)
 *   clock_drift_ms - NTP time - AS11 device time
 *
 * Returns true if the spool became fresh, false if still stale after
 * timeout (caller should proceed with available data). */
bool post_therapy_wait_spool_current(int64_t end_epoch_ms, int64_t clock_drift_ms);
