/*
 * SomnoTrace - SD card paths used by the uploader
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

/* These MUST mirror main/sd_storage.h.  The uploader is a component and the
 * app depends on it, not the other way round, so the paths cannot be included
 * from there.  Previously each backend defined its own copy; they live here so
 * there is exactly one place to change.
 *
 * The ResMed-compatible export tree is what gets uploaded:
 *
 *   SDCARD/DATALOG/<YYYYMMDD>/<prefix>_{BRP,PLD,SA2,EVE,CSL}.edf   session data
 *   SDCARD/STR.edf, Identification.json/.crc, SETTINGS files       root bundle
 */

/* Overridable so the scan logic can be exercised against a copy of a real
 * card on the host (see .ai test harness). */
#ifndef SD_MOUNT_POINT
#define SD_MOUNT_POINT       "/somnotrace"
#endif
#define SD_SDCARD_DIR        SD_MOUNT_POINT "/SDCARD"
#define SD_SDCARD_DATALOG    SD_SDCARD_DIR "/DATALOG"
#define SD_SDCARD_SETTINGS   SD_SDCARD_DIR "/SETTINGS"
