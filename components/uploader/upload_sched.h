/*
 * SomnoTrace - Upload scheduler (triggers, per-backend state machine)
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
#include <stdint.h>
#include "esp_err.h"
#include "uploader.h"

/* ────────────────────────────────────────────────────────────────────
 *  Scheduler
 *
 *  Owns the upload task and decides what runs when.  Three triggers:
 *
 *   1. export complete   — the exporter reports a day; that day is rescanned
 *                          and, if anything is pending, uploaded now.
 *   2. periodic scan     — every SCAN_INTERVAL, reconciles the card against
 *                          the index.  This is what recovers after a crash or
 *                          a lost state file.  Nothing pending means nothing
 *                          is connected to.
 *   3. manual            — reset (clear state and re-upload, bounded by
 *                          max_days) or an explicit rescan from the Web UI.
 *
 *  Backends are independent: each has its own cursor, its own cooldown ladder
 *  and its own error.  A backend that fails twice in a run is put on cooldown
 *  and the next backend runs immediately, so an unreachable SMB share cannot
 *  hold up SleepHQ.  Nothing is ever abandoned permanently — attempts only
 *  drive the ladder, and the periodic scan keeps re-offering the work.
 * ──────────────────────────────────────────────────────────────────── */

/* ── Provided by uploader.c ───────────────────────────────────────────
 * The scheduler needs configuration and the backend registry, both of which
 * uploader.c owns.  Declared here so there is one declaration rather than
 * extern statements scattered across translation units. */
int  uploader_max_days(void);
int  uploader_enabled_backends(const upload_backend_t **out, int max_out);
bool uploader_lease_take(uint32_t timeout_ms);
void uploader_lease_give(void);

/* Fan out a backend state transition to the app's injected notifier (if any)
 * so the web UI can be updated immediately.  Cheap and non-blocking. */
void uploader_notify_progress_changed(void);

/* Create the task and queue.  Backends must already be registered. */
esp_err_t upload_sched_init(void);

/* Trigger 1: an export finished for this day (numeric YYYYMMDD). */
void upload_sched_notify_export(uint32_t day);

/* The day's exported files were replaced (rebuild / recreate), so whatever
 * was uploaded before is stale.  Drops the day's state and re-uploads. */
void upload_sched_notify_invalidate(uint32_t day);

/* Trigger 3: rescan now, or clear all state and re-upload from scratch. */
void upload_sched_request_scan(void);
void upload_sched_request_reset(void);

/* Optional hook: return true while the SD card is busy with something that
 * should take priority (a live therapy recording).  Periodic scans are
 * deferred while it returns true; event-driven uploads still run. */
typedef bool (*upload_sched_busy_fn_t)(void);
void upload_sched_set_busy_fn(upload_sched_busy_fn_t fn);

/* True while any backend is inside a run (connected and transferring), so a
 * "Test connection" probe does not open a second transport alongside it. */
bool upload_sched_uploading(void);

/* Compact progress for the Web UI.  Caller frees. */
esp_err_t upload_sched_progress_json(char **out_json);

/* One-line summary for /api/status: number of units not yet uploaded across
 * configured backends, and the worst backend state as a short string. */
void upload_sched_summary(int *out_pending, const char **out_worst);
