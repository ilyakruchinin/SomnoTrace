/*
 * SomnoTrace - Upload scanner (derives upload units from the SD card tree)
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
#include "upload_index.h"

/* ────────────────────────────────────────────────────────────────────
 *  Scanner
 *
 *  Derives upload units from the SDCARD/ tree and reconciles them against
 *  the index.  This is the self-healing half of the design: whatever the
 *  index believes, the card is the truth about which files exist.
 *
 *  Session EDFs are immutable once renamed into place, so a group is
 *  identified by prefix and validated by (kinds, file count) only — no
 *  hashing, no stat.  Replacement of an existing export is an explicit
 *  event (rebuild-day / recreate-edfs), which calls
 *  upload_index_forget_day() rather than relying on the scan to notice.
 *
 *  The root bundle is different: STR.edf and friends are rewritten by every
 *  export, so they are fingerprinted by size+mtime.
 * ──────────────────────────────────────────────────────────────────── */

#define UPLOAD_GROUP_MAX_FILES  6
#define UPLOAD_FILENAME_LEN     40
#define UPLOAD_BUNDLE_MAX_FILES 8

/* A group as it exists on the card, ready to hand to a backend. */
typedef struct {
    char     day[12];                  /* "20260807"                      */
    char     prefix[20];               /* "20260807_134405"               */
    uint32_t prefix_sec;
    uint8_t  kinds;                    /* UGK_* bitmask                   */
    int      n_files;
    char     files[UPLOAD_GROUP_MAX_FILES][UPLOAD_FILENAME_LEN];
} upload_group_ref_t;

/* The root bundle as it exists on the card. */
typedef struct {
    int      n_files;
    char     paths[UPLOAD_BUNDLE_MAX_FILES][80];   /* absolute local paths */
    char     names[UPLOAD_BUNDLE_MAX_FILES][40];   /* remote basename      */
    bool     in_settings[UPLOAD_BUNDLE_MAX_FILES]; /* lives in SETTINGS/   */
    uint64_t fp;                                   /* size+mtime digest    */
} upload_bundle_ref_t;

/* Enumerate DATALOG day folders, newest first.  Returns the count. */
int upload_scan_days(uint32_t *out_days, int max_out);

/* Enumerate the groups of one day.  Returns the count. */
int upload_scan_day_groups(const char *day, upload_group_ref_t *out, int max_out);

/* Collect the root bundle and its fingerprint.  Returns false if STR.edf is
 * missing, in which case there is nothing meaningful to upload yet. */
bool upload_scan_bundle(upload_bundle_ref_t *out);

/* Reconcile one day against the index: add groups that are new, refresh
 * groups whose file set changed (which resets them to pending), and drop
 * groups whose files have gone.  Returns the number of groups in the day. */
int upload_scan_reconcile_day(uint32_t day);

/* Reconcile the newest max_days days.  Returns the total number of groups
 * that are not yet OK for at least one configured backend. */
int upload_scan_reconcile_all(int max_days, const int *slots, int n_slots);
