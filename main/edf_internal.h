/*
 * SomnoTrace - Internal definitions and cross-module interfaces for EDF generation
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "snt_format.h"
#include "edf_data_dict.h"
#include "as11_time.h"
#include "sd_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── CRC Operations ────────────────────────────────────────────────── */

uint16_t edf_crc16_ccitt(const uint8_t *data, size_t len);
uint32_t edf_crc32_ieee(const uint8_t *data, size_t len);

/* ── File Operations & Utilities ────────────────────────────────────── */

void edf_write_field(char *buf, int width, const char *text);
bool edf_write_all(FILE *f, const void *data, size_t len);

FILE *edf_open_atomic_file(const char *path, char *tmp_path, size_t tmp_path_len);
esp_err_t edf_finalize_atomic_file(FILE *f, const char *tmp_path, const char *path);
void edf_discard_atomic_file(FILE *f, const char *tmp_path);

cJSON *edf_read_json_file(const char *path);
esp_err_t edf_write_json_file(const char *path, const cJSON *json);
uint8_t *edf_read_bin_file(const char *path, size_t *out_len);

/* ── Subsystem Module Interfaces ───────────────────────────────────── */

/* Header Generator (edf_header.c) */
int edf_write_header(FILE *f, const char *patient_id, const char *recording_id,
                     const char *start_date, const char *start_time,
                     int num_records, const char *record_duration,
                     const char *reserved_field,
                     const edf_signal_def_t *signals, int n_signals);

esp_err_t edf_generate_identification_files(const char *edf_dir,
                                            const char *ident_json_path);

/* Waveform Exporter (edf_waveform.c) */
esp_err_t edf_convert_snt_to_edf(const char *snt_path, const char *edf_path,
                                 const char *patient_id, const char *recording_id,
                                 const char *start_date, const char *start_time,
                                 const edf_signal_def_t *signals, int n_signals,
                                 const char *record_duration,
                                 const int *channel_map,
                                 int skip_records, int max_records,
                                 const char *second_snt_path);

/* Annotation Exporter (edf_annotations.c) */
esp_err_t edf_generate_eve_edf(const char *out_path, const char *events_snt_path,
                               int64_t session_start_ms, int64_t clock_drift_ms,
                               const char *patient_id, const char *recording_id,
                               const char *start_date, const char *start_time);

esp_err_t edf_generate_csl_edf(const char *out_path, const char *events_snt_path,
                               int64_t session_start_ms, int64_t clock_drift_ms,
                               const char *patient_id, const char *recording_id,
                               const char *start_date, const char *start_time);

/* Summary Exporter (edf_summary.c) */
esp_err_t edf_generate_str_edf(const char *sdcard_dir,
                               const char *patient_id, const char *recording_id,
                               const char *start_date,
                               const cJSON *settings_json,
                               const char *session_dir,
                               const char *session_id,
                               int64_t start_epoch_ms, int64_t end_epoch_ms,
                               int64_t clock_drift_ms);

#ifdef __cplusplus
}
#endif
