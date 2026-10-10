/*
 * SomnoTrace - SomnoStage ML sleep staging component
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

#ifndef SOMNO_ML_H
#define SOMNO_ML_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOMNO_ML_N_FEATURES 95
#define SOMNO_ML_N_STAGES   4   /* 0 Wake, 1 Light, 2 Deep, 3 REM */

/* .sst sidecar (SST1): fixed 64-byte little-endian header then u8 stage[] +
 * u8 confidence[] per epoch. Written once, atomically (tmp+rename). */
#define SOMNO_SST_MAGIC   0x31545353u /* "SST1" */
#define SOMNO_SST_VERSION 1

#define SOMNO_SST_SOURCE_RING 0
#define SOMNO_SST_SOURCE_AS11 1

#define SOMNO_SST_FLAG_PARTIAL      0x01  /* source data truncated/incomplete */
#define SOMNO_SST_FLAG_MOTION_USED  0x02  /* motion channel fed to scorer    */
#define SOMNO_SST_FLAG_INSUFFICIENT 0x04  /* below oximeter-presence guard   */

/* Packed wire layout — do not rely on compiler padding. */
typedef struct __attribute__((packed)) {
    uint32_t magic;            /* @0  SOMNO_SST_MAGIC */
    uint16_t version;          /* @4  SOMNO_SST_VERSION */
    uint8_t  decode_mode;      /* @6  0=argmax 1=viterbi 2=forward_backward */
    uint8_t  flags;            /* @7  SOMNO_SST_FLAG_* */
    uint8_t  model_semver[24]; /* @8  NUL-padded */
    uint8_t  source_type;      /* @32 SOMNO_SST_SOURCE_* */
    uint8_t  reserved0[3];     /* @33 */
    uint32_t n_epochs;         /* @36 */
    int64_t  start_ms;         /* @40 epoch 0 start, unix ms */
    uint32_t epoch_sec;        /* @48 nominal epoch duration (30) */
    uint32_t header_crc;       /* @52 reserved, 0 */
    uint8_t  reserved1[8];     /* @56 */
} somno_sst_header_t;          /* exactly 64 bytes */

_Static_assert(sizeof(somno_sst_header_t) == 64, "SST1 header must be 64 bytes");

typedef struct {
    int      stage;            /* 0..3, or -1 when unavailable */
    float    confidence;       /* 0..1 posterior max */
    int64_t  epoch_start_ms;
} somno_ml_epoch_t;

/* Scoring job descriptor — one completed recording. The worker resolves
 * source/sidecar paths itself from the canonical layout. */
typedef enum {
    SOMNO_ML_SRC_RING_VITALS = 0,  /* canonical vitals.snt, SNT3; id=recording_id */
    SOMNO_ML_SRC_AS11_SA2    = 1,  /* AS11 session; id=file prefix, dir=day dir */
} somno_ml_source_t;

typedef struct {
    somno_ml_source_t type;
    char     id[64];           /* recording_id or session file prefix */
    char     dir[192];         /* AS11: streams day dir; ring: unused */
} somno_ml_job_t;

/* ------- public API ------- */

/* True when a real model is compiled in (false for stub/fork builds). */
bool somno_ml_available(void);

/* Decrypt + parse the embedded model. Call early in app_main, before any
 * flash-writing background tasks exist (flash-cache constraint). */
int somno_ml_init(void);

/* Model semver string ("-" when unavailable). */
const char *somno_ml_model_semver(void);

/* Enqueue a scoring job; returns 0 on success. Safe to call from any task. */
int somno_ml_enqueue(const somno_ml_job_t *job);

/* Convenience enqueue helpers for the two source types. */
int somno_ml_enqueue_ring(const char *recording_id);
int somno_ml_enqueue_as11(const char *day_dir, const char *file_prefix);

/* Synchronous scoring (used by worker internally; exposed for tests).
 * Reads the source file, runs the causal pipeline, writes <sst_path> via
 * tmp+rename. Returns 0 on success. */
int somno_ml_score_file(const somno_ml_job_t *job);

/* Boot-time reconcile: scan canonical oximetry recordings + AS11 session
 * dirs for missing/stale .sst sidecars and enqueue them. */
int somno_ml_reconcile(void);

/* Same, but only scans day dirs newer than `days` ago.  Used by the
 * periodic self-heal pass, where drops are always recent. */
int somno_ml_reconcile_recent(int days);

/* Current rough state for the MQTT/API feed (RAM only, never persisted). */
typedef struct {
    bool     active;
    int      stage;          /* -1 when inactive */
    float    confidence;
    int64_t  updated_ms;
    bool     provisional;    /* always true — never a final hypnogram */
} somno_ml_live_state_t;
void somno_ml_live_state(somno_ml_live_state_t *out);

/* Validate an existing .sst file (magic/version). Returns the header
 * flags (>=0) on success, -1 on failure; optionally copies semver out. */
int somno_ml_sst_validate(const char *path, char *semver_out, size_t semver_len);

#ifdef __cplusplus
}
#endif

#endif /* SOMNO_ML_H */
