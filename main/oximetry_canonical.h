/*
 * SomnoTrace - Canonical oximetry recording-package storage and conversion
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

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The canonical tree is rooted below SD_OXYMETRY_DIR.  It intentionally does
 * not overlap the legacy oximeter_store.c files/ tree. */
#define OXIMETRY_CANONICAL_MAX_COMPONENT 64
#define OXIMETRY_CANONICAL_MAX_PATH      512
#define OXIMETRY_CANONICAL_MAX_RECORDINGS 64
#define OXIMETRY_CANONICAL_MAX_SOURCE_BYTES (64ULL * 1024ULL * 1024ULL)
#define OXIMETRY_CANONICAL_MAX_JSON_BYTES   (16U * 1024U)

#define OXIMETRY_CANONICAL_SNT_MAGIC       0x33544e53u /* little-endian "SNT3" */
#define OXIMETRY_CANONICAL_SNT_VERSION     3u
#define OXIMETRY_CANONICAL_SNT_HEADER_LEN  64u
#define OXIMETRY_CANONICAL_SNT_MISSING     INT16_MIN
#define OXIMETRY_CANONICAL_VITALS_CHANNELS 5u

/* Interleaved INT16 channels in vitals.snt.  The canonical status is a bit
 * mask: bit 0 means missing SpO2, bit 1 means missing pulse. */
#define OXIMETRY_CANONICAL_VITALS_SPO2          0u
#define OXIMETRY_CANONICAL_VITALS_PULSE         1u
#define OXIMETRY_CANONICAL_VITALS_MOTION_FLAGS  2u
#define OXIMETRY_CANONICAL_VITALS_STATUS        3u
#define OXIMETRY_CANONICAL_VITALS_SOURCE_STATUS 4u

/* Create .somnotrace/oximetry and its canonical children:
 * devices, inbox, staging, recordings, quarantine, and state. */
esp_err_t oximetry_canonical_ensure_dirs(void);

/* Convert one completed O2 Ring S Format A file into a durable recording
 * package.  start_utc_ms is the UTC timestamp of the first 1 Hz sample.
 * The source is copied byte-for-byte into the package; it is never rewritten.
 * device_id and recording_id are single safe path components. */
esp_err_t oximetry_canonical_convert_format_a(const char *device_id,
                                               const char *recording_id,
                                               const char *source_path,
                                               int64_t start_utc_ms);

/* Convert one completed O2 Ring (Gen1) VLD3 file into a durable recording
 * package.  Vendor-layout metadata provides cadence; header and filename
 * timestamps are validated independently with provenance retained.  The source
 * is copied byte-for-byte. device_id and recording_id are safe path components. */
esp_err_t oximetry_canonical_convert_vld3(const char *device_id,
                                           const char *recording_id,
                                           const char *source_path);

/* Reconcile interrupted staging/package work at boot.  Invalid or incomplete
 * entries are moved to quarantine.  Ready recordings are those with a valid
 * root recording.json pointer and a valid generation manifest/track. */
esp_err_t oximetry_canonical_reconcile(void);

/* Import valid files from the current files/<serial>/ tree without deleting
 * or moving the legacy source. Idempotent and safe to call at boot. */
esp_err_t oximetry_canonical_migrate_legacy(const char *device_id);
esp_err_t oximetry_canonical_migrate_all_legacy(void);

/* Resolve only canonical, ready paths.  The output is NUL terminated.  A
 * caller must not concatenate untrusted path text around these results. */
esp_err_t oximetry_canonical_resolve_recording(const char *recording_id,
                                               char *out_path,
                                               size_t out_path_size);
esp_err_t oximetry_canonical_resolve_track(const char *recording_id,
                                           const char *track_id,
                                           char *out_path,
                                           size_t out_path_size);

/* JSON APIs return owned cJSON values; the caller must cJSON_Delete(*out).
 * list_ready returns an array of ready recording summaries. */
esp_err_t oximetry_canonical_list_ready(cJSON **out);
esp_err_t oximetry_canonical_list_ready_for_day(const char *day, cJSON **out);
esp_err_t oximetry_canonical_list_days(cJSON **out);
esp_err_t oximetry_canonical_get_manifest(const char *recording_id,
                                          cJSON **out);

/* Convenience JSON serialization.  The returned buffer is allocated by
 * cJSON and must be released with cJSON_free().  NULL means failure. */
char *oximetry_canonical_list_json(void);
char *oximetry_canonical_manifest_json(const char *recording_id);

#ifdef __cplusplus
}
#endif
