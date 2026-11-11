/*
 * SomnoTrace - Immutable source object and observation store
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
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "cJSON.h"

#define SOURCE_STORE_SCHEMA 1
#define SOURCE_STORE_ID_HEX 64

typedef struct {
    char     id[SOURCE_STORE_ID_HEX + 1];
    uint64_t length;
    bool     existed;
} source_store_put_result_t;

/* Serialize compound store operations shared by the importer and bridge. */
bool source_store_lock(void);
void source_store_unlock(void);

/* Initialise the store under `root`; NULL selects
 * SD_SESSIONS_DIR "/sources".  Creates objects/ and observations/. */
esp_err_t source_store_init(const char *root);

/* Publish immutable bytes.  The object name is the content digest.  An
 * existing object is reused only when its length and digest verify. */
esp_err_t source_store_put(const char *kind, const uint8_t *data, size_t len,
                           source_store_put_result_t *out);

/* Publish a file's bytes without loading them in the caller. */
esp_err_t source_store_put_path(const char *kind, const char *path,
                                source_store_put_result_t *out);

/* Read and verify an object.  Returns ESP_ERR_NOT_FOUND when absent and
 * ESP_ERR_INVALID_CRC when the bytes no longer match their name. */
esp_err_t source_store_get(const char *id, uint8_t *buf, size_t cap,
                           size_t *len_out);

/* Verify length and digest without returning the bytes. */
esp_err_t source_store_verify(const char *id, uint64_t expected_len);

/* Attach one immutable observation envelope to an object.  `observation_id`
 * must be a safe filename component; the envelope is stored as
 * observations/<object>-<observation_id>.json.  Repeating the same envelope
 * is idempotent; a different envelope under the same id is rejected. */
esp_err_t source_store_observe(const char *id, const char *observation_id,
                               const char *kind, const cJSON *observation);

/* Caller holds the store lock. */
bool source_store_has_observation(const char *id, const char *observation_id,
                                  const char *kind);

/* Find the newest observation for a source tuple.  The observation JSON must
 * carry `key`, `size`, `ts` (string or null) and ideally `observed_at_ms`.
 * Returns ESP_ERR_NOT_FOUND when no committed observation matches and
 * ESP_ERR_INVALID_STATE when equally-new observations disagree. */
esp_err_t source_store_find(const char *kind, const char *key,
                            const char *size, const char *ts, bool ts_null,
                            char *id_out, size_t id_cap,
                            char *obs_out, size_t obs_cap);

/* Copy a verified object to a staging path atomically. */
esp_err_t source_store_copy_to(const char *id, const char *dest);

/* Delete objects that no observation references, plus stale .tmp files.
 * Returns the number of files removed.  Never deletes referenced evidence. */
esp_err_t source_store_prune_orphans(int *removed);

/* Byte budget for immutable objects.  A new object that would exceed the
 * budget is refused with ESP_ERR_NO_MEM; reusing an existing object is
 * always allowed.  Default: SOURCE_STORE_DEFAULT_BUDGET. */
#define SOURCE_STORE_DEFAULT_BUDGET (32ULL * 1024 * 1024)
esp_err_t source_store_set_budget(uint64_t bytes);
esp_err_t source_store_total_bytes(uint64_t *total);

/* Number of immutable objects currently present (diagnostics/tests). */
esp_err_t source_store_count_objects(int *count);
