/*
 * SomnoTrace - Crash diagnostics (reset reason + core dump analysis)
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

#include <stdint.h>

/**
 * Check the reset reason and, if a crash occurred, report what is known:
 * the RTC breadcrumb written before the crash, and the core dump summary
 * (task name, exception PC, backtrace) when one was successfully written.
 *
 * Should be called once, early in app_main() — after log_stream_init()
 * so the output is captured in the ring buffer, but before any other
 * subsystem that might itself crash.
 *
 * A core dump is erased only after its summary has been logged
 * successfully.  If the summary cannot be read (allocation failure, parse
 * failure) the image is left on flash so it can still be pulled off the
 * device for offline decoding, and retried on a later boot.
 */
void crash_diag_check(void);

/* ── Crash breadcrumb (RTC memory, survives panic/WDT resets) ────────────
 *
 * A core dump cannot be captured when the crashing task's stack is
 * unusable: the panic handler faults while spilling register windows onto
 * that stack, re-enters, and writes nothing.  That is exactly what happened
 * on 2026-08-09, leaving a reset reason and no context.
 *
 * These breadcrumbs are written during normal operation into RTC memory,
 * which is preserved across software/panic/watchdog resets and only
 * randomised on power-on.  They need no allocation, no locks and no
 * filesystem, so they remain valid however the firmware dies, and are
 * reported by crash_diag_check() on the next boot. */

/* Record the session currently being recorded ("" when idle). */
void crash_diag_note_session(const char *session_id);

/* Record the latest therapy/alert transition or operation tag.
 * Keep tags short and stable — they are truncated to 23 characters. */
void crash_diag_note_activity(const char *tag);
