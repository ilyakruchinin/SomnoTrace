/*
 * SomnoTrace - SD card storage initialisation and FATFS mount
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
#include "esp_err.h"

#define SD_MOUNT_POINT   "/somnotrace"

/* All SomnoTrace-managed data lives under a single dotted app-root folder on
 * the card (the FATFS mount point itself is /somnotrace). The subfolders are
 * undotted. Layout on the card:
 *   .somnotrace/sessions/      ESP-native session data (streams, summaries)
 *   .somnotrace/logs/          rotated device logs
 *   .somnotrace/upload_state/  uploader persistence (was LittleFS)
 * The ResMed-compatible SDCARD/ export stays at the card root. */
#define SD_APP_DIR           SD_MOUNT_POINT "/.somnotrace"

/* ESP-native session data (raw streams, spool files, internal state) */
#define SD_SESSIONS_DIR      SD_APP_DIR "/sessions"
#define SD_STREAMS_DIR       SD_SESSIONS_DIR "/streams"
#define SD_SUMMARIES_DIR     SD_SESSIONS_DIR "/summaries"

/* Rotated device logs */
#define SD_LOG_DIR           SD_APP_DIR "/logs"

/* Uploader persistent state (migrated off the LittleFS flash partition) */
#define SD_UPLOAD_STATE_DIR  SD_APP_DIR "/upload_state"

/* Oximetry raw files from O2 Ring (Format A blobs, not EDF) */
#define SD_OXYMETRY_DIR       SD_APP_DIR "/oximetry"

/* ResMed-compatible export folder (self-contained, OSCAR-ready) */
#define SD_SDCARD_DIR        SD_MOUNT_POINT "/SDCARD"
#define SD_SDCARD_DATALOG    SD_SDCARD_DIR "/DATALOG"
#define SD_SDCARD_SETTINGS   SD_SDCARD_DIR "/SETTINGS"

/* Initialise SDMMC 4-bit mode and mount FATFS at /somnotrace.
 * Creates the .somnotrace/ and SDCARD/ directory trees if they don't exist.
 * Returns ESP_OK on success. Non-fatal — caller may continue without SD. */
esp_err_t sd_storage_init(void);

/* Returns true if the SD card is mounted and ready. */
bool sd_storage_is_ready(void);

/* ── Free space ────────────────────────────────────────────────────────
 * Queries FATFS directly (f_getfree).  Deliberately uncached: the status
 * endpoint caches its copy for 10 minutes, which is far too stale to gate
 * a therapy recording on. */
esp_err_t sd_storage_get_free(uint64_t *free_bytes, uint64_t *total_bytes);

/* Space policy for raw capture.  Raw session data is the source of truth;
 * SDCARD/ is derived and regenerable, so derived output is reclaimed before
 * recording is ever refused.
 *
 * Returns true if a new session may start.  Warns (on screen) when free
 * space is low, and only returns false below the hard floor. */
bool sd_storage_reserve_for_recording(void);

/* ── Storage arbitration ───────────────────────────────────────────────
 * The card is shared by the raw writer, EDF generation, the uploader, FTP,
 * HTTP downloads and the destructive maintenance actions.  Raw capture
 * outranks everything else, so bulk and destructive work must declare
 * itself and can be refused while a session is recording.
 *
 * Roles:
 *   EXPORT      — EDF generation / day rebuild.  Mutually exclusive with
 *                 other EXPORT work and with DESTRUCTIVE work.  Allowed
 *                 during recording (a mask-off break starts a new session
 *                 while the previous one still needs exporting).
 *   DESTRUCTIVE — recreate/delete/reset/format.  Refused while recording
 *                 or while an export or upload is in progress.
 *   UPLOAD      — reads a day folder.  Excluded from concurrent EXPORT so
 *                 it can never read a day that is being replaced.
 */
typedef enum {
    SD_LEASE_EXPORT = 0,
    SD_LEASE_DESTRUCTIVE,
    SD_LEASE_UPLOAD,
} sd_lease_t;

/* Mark a therapy recording as active/inactive (called by the session
 * writer).  Destructive operations are refused while this is non-zero. */
void sd_storage_recording_begin(void);
void sd_storage_recording_end(void);
bool sd_storage_recording_active(void);

/* Acquire/release a storage lease.  timeout_ms may be 0 to fail fast.
 * Returns false if the lease could not be acquired (busy, or refused
 * because a recording is in progress). */
bool sd_storage_lease_acquire(sd_lease_t role, uint32_t timeout_ms);
void sd_storage_lease_release(sd_lease_t role);

/* Format the SD card filesystem (FAT32).  All data on the card is lost.
 * Unmounts, formats, remounts, and recreates the directory tree.
 * Must NOT be called from the HTTP handler task — run via a background task.
 * Returns ESP_OK on success. */
esp_err_t sd_storage_format(void);

/* Flush and unmount FATFS.  Issues f_unmount (which syncs the FAT and the
 * card's internal write buffer) and releases the card handle.  Intended to be
 * called just before a deliberate reboot so a hard reset never lands on top of
 * unflushed filesystem state.  Safe to call when nothing is mounted. */
void sd_storage_deinit(void);
