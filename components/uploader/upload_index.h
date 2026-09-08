/*
 * SomnoTrace - Upload tracking index (per-group, per-backend)
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

/* ────────────────────────────────────────────────────────────────────
 *  Upload tracking index
 *
 *  Tracks what has been uploaded at *group* granularity, independently
 *  per backend.
 *
 *  A "group" is the set of EDF files sharing one filename prefix, e.g.
 *  20260807_134405_{BRP,PLD,SA2}.edf.  Note that one therapy session
 *  normally produces TWO groups, because edf_gen names EVE/CSL from
 *  TherapyStart and BRP/PLD/SA2 from the _ZLE/MaskOn edge a few seconds
 *  later:
 *
 *      20260807_134400_{EVE,CSL}.edf     <- TherapyStart prefix
 *      20260807_134405_{BRP,PLD,SA2}.edf <- MaskOn prefix
 *
 *  The group is used as the tracking unit rather than the therapy session
 *  because it is derivable purely from the SDCARD/ tree that actually gets
 *  uploaded — no cross-referencing into the raw session store — which makes
 *  the self-healing scan a pure directory listing.  The two groups of a
 *  session upload back to back, so the practical granularity is the same.
 *
 *  Persistence: one small JSON file per day under UPLOAD_STATE_DIR, rewritten
 *  atomically (temp -> fsync -> rename) when that day changes.  JSON was
 *  chosen over a binary layout for debuggability; at a few hundred bytes per
 *  day the size difference does not matter, and a whole-file rewrite is
 *  cheaper than an append plus the replay/compaction machinery it would need.
 *  Pruning is a file unlink.
 *
 *  NOTE: UPLOAD_STATE_DIR must mirror SD_APP_DIR/upload_state in
 *  main/sd_storage.h (this component does not depend on main).
 * ──────────────────────────────────────────────────────────────────── */

#define UPLOAD_STATE_DIR          "/somnotrace/.somnotrace/upload_state"
#define UPLOAD_BUNDLE_STATE_PATH  UPLOAD_STATE_DIR "/bundle.json"

#define UPLOAD_MAX_BACKENDS        4
#define UPLOAD_BACKEND_ID_LEN      12
#define UPLOAD_MAX_GROUPS_PER_DAY  128
#define UPLOAD_MAX_DAYS_CAP        366   /* hard ceiling for max_days     */
#define UPLOAD_DEFAULT_MAX_DAYS    30

/* Per-group, per-backend upload status. */
typedef enum {
    UG_PENDING = 0,   /* never uploaded, or invalidated                   */
    UG_OK,            /* every file in the group transferred successfully */
    UG_FAILED,        /* attempted and failed; retried via the ladder     */
} upload_unit_status_t;

/* Which EDF kinds a group contains.  Kept for change detection and to make
 * the state files self-describing. */
#define UGK_BRP  (1u << 0)
#define UGK_PLD  (1u << 1)
#define UGK_SA2  (1u << 2)
#define UGK_EVE  (1u << 3)
#define UGK_CSL  (1u << 4)

typedef struct {
    uint8_t  status;       /* upload_unit_status_t                        */
    uint8_t  attempts;
    uint32_t last_try_s;   /* epoch seconds of last attempt, 0 = never    */
} upload_unit_t;

typedef struct {
    uint32_t      prefix_sec;   /* HHMMSS of the prefix, as seconds        */
    uint8_t       kinds;        /* UGK_* bitmask                           */
    uint8_t       n_files;
    upload_unit_t be[UPLOAD_MAX_BACKENDS];
} upload_group_t;

typedef struct {
    uint32_t       day;         /* 20260807                                */
    int            n_groups;
    bool           dirty;       /* needs persisting                        */
    upload_group_t groups[UPLOAD_MAX_GROUPS_PER_DAY];
} upload_day_t;

/* ── Lifecycle ────────────────────────────────────────────────────── */

/* Create the state directory and allocate the index.  Idempotent. */
esp_err_t upload_index_init(void);

/* Load every day file present, keeping the newest max_days.  Older files are
 * pruned from disk.  Safe to call more than once. */
esp_err_t upload_index_load(int max_days);

/* Drop all state, on disk and in memory (the "reset upload state" action). */
esp_err_t upload_index_clear(void);

/* ── Backend identity ─────────────────────────────────────────────────
 * Backends are addressed by slot.  Slots are assigned on first use and
 * persisted by name, so adding a backend later cannot renumber existing
 * state files. */
int  upload_index_backend_slot(const char *backend_id);
const char *upload_index_backend_name(int slot);
int  upload_index_backend_count(void);

/* ── Day / group access ───────────────────────────────────────────── */

/* Find a day, optionally creating it.  Returns NULL if absent (and !create)
 * or if the index is full. */
upload_day_t *upload_index_day(uint32_t day, bool create);

/* Find a group within a day by prefix, optionally creating it. */
upload_group_t *upload_index_group(upload_day_t *d, uint32_t prefix_sec,
                                   bool create);

/* Remove a group from a day (the file disappeared from the card). */
void upload_index_drop_group(upload_day_t *d, uint32_t prefix_sec);

/* Iteration, ordered newest day first. */
int           upload_index_day_count(void);
upload_day_t *upload_index_day_at(int i);

/* Forget one day entirely: deletes its state file and drops it from the
 * index, so every group in it is re-uploaded.  Used when a day's export is
 * regenerated (rebuild-day) and the files on the card have been replaced. */
esp_err_t upload_index_forget_day(uint32_t day);

/* ── Persistence ──────────────────────────────────────────────────── */

/* Persist one day if marked dirty.  Clears the dirty flag on success. */
esp_err_t upload_index_save_day(upload_day_t *d);

/* Persist every dirty day. */
esp_err_t upload_index_save_all(void);

/* ── Root bundle ──────────────────────────────────────────────────────
 * STR.edf, Identification.json/.crc and the SETTINGS files are rewritten
 * export (STR.edf is cumulative across all days), so unlike session EDFs
 * they cannot be tracked by name alone.  They are tracked as one unit,
 * fingerprinted by the size+mtime of its members, per backend. */

/* Returns the fingerprint last successfully uploaded to this backend, or 0. */
uint64_t upload_index_bundle_ok_fp(int slot);

/* Record that the bundle at fingerprint fp uploaded successfully. */
void upload_index_set_bundle_ok(int slot, uint64_t fp);

/* ── Aggregates for the progress API ────────────────────────────────── */

/* Count days that have at least one group, and days where every group is OK
 * for this backend, within the newest max_days. */
void upload_index_backend_progress(int slot, int max_days,
                                   int *out_days_done, int *out_days_total);

/* Total number of groups still pending or failed for this backend. */
int upload_index_backend_pending(int slot, int max_days);

/* Dump one day's state as JSON (debug endpoint).  Caller frees. */
esp_err_t upload_index_day_to_json(uint32_t day, char **out_json);
