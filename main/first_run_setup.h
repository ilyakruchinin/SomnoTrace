/*
 * SomnoTrace - Persistent first-run setup state
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

#ifdef __cplusplus
extern "C" {
#endif

#define FIRST_RUN_SETUP_SCHEMA_VERSION 1U

/* Stable bit positions. Append new steps; never reorder existing values. */
typedef enum {
    FIRST_RUN_SETUP_STEP_WIFI = 0,
    FIRST_RUN_SETUP_STEP_TIME,
    FIRST_RUN_SETUP_STEP_AIRSENSE,
    FIRST_RUN_SETUP_STEP_CARD,
    FIRST_RUN_SETUP_STEP_ALERTS,
    FIRST_RUN_SETUP_STEP_UPLOADS,
    FIRST_RUN_SETUP_STEP_COUNT,
    FIRST_RUN_SETUP_STEP_FINISHED = FIRST_RUN_SETUP_STEP_COUNT,
} first_run_setup_step_t;

typedef enum {
    /* Move the resumable setup cursor without changing a step outcome. */
    FIRST_RUN_SETUP_UPDATE_SELECT = 0,
    /* The step's requirement is currently satisfied. */
    FIRST_RUN_SETUP_UPDATE_COMPLETE,
    /* A deliberate step-specific skip. Card rejects this outcome. */
    FIRST_RUN_SETUP_UPDATE_SKIP,
    /* Card only: the user acknowledged that new nights will not be recorded. */
    FIRST_RUN_SETUP_UPDATE_CONTINUE_WITHOUT_RECORDING,
} first_run_setup_update_t;

typedef struct {
    uint16_t schema_version;
    first_run_setup_step_t current_step;
    uint8_t completed_mask;
    uint8_t skipped_mask;
    bool continue_without_recording;
} first_run_setup_state_t;

/* Facts are observed by the caller so this service remains independent of
 * Wi-Fi, BLE, storage, alert, upload, boot, and LVGL implementations.
 *
 * established_installation is an explicit migration decision. It must only be
 * true when the caller has evidence that this firmware is upgrading a device
 * which was already in use before the first-run state schema existed. On such
 * a device, and only when no setup record exists, absent optional/skippable
 * services are recorded as skipped and an absent card is recorded as an
 * explicit continue-without-recording choice. This prevents an upgrade from
 * unexpectedly trapping an existing deployment in setup.
 */
typedef struct {
    bool established_installation;
    bool wifi_configured;
    bool time_configured;
    bool airsense_paired;
    bool card_present;
    bool alerts_configured;
    bool uploads_configured;
} first_run_setup_observed_t;

typedef struct {
    first_run_setup_state_t state;
    bool persisted;
    bool schema_compatible;
    esp_err_t last_storage_result;
} first_run_setup_snapshot_t;

const char *first_run_setup_step_name(first_run_setup_step_t step);
bool first_run_setup_step_can_skip(first_run_setup_step_t step);
bool first_run_setup_step_is_resolved(const first_run_setup_state_t *state,
                                      first_run_setup_step_t step);
bool first_run_setup_is_finished(const first_run_setup_state_t *state);

/* Load the state from NVS. ESP_ERR_NVS_NOT_FOUND installs a fresh in-memory
 * state and is an expected result on a new device. Call this before update or
 * reconcile so an existing/future record can never be overwritten blindly.
 * All service I/O first initializes and then uses nvs_writer; it is safe for a
 * caller whose task stack lives in PSRAM. */
esp_err_t first_run_setup_load(void);

/* Copy the coherent in-memory state without performing I/O. */
void first_run_setup_snapshot(first_run_setup_snapshot_t *out);

/* Persist one explicit transition. There is deliberately no whole-setup skip:
 * only the five skippable named steps accept UPDATE_SKIP, and Card requires
 * UPDATE_CONTINUE_WITHOUT_RECORDING when it is absent. */
esp_err_t first_run_setup_update(first_run_setup_step_t step,
                                 first_run_setup_update_t update);

/* Merge caller-observed facts and persist the result. Existing persisted user
 * choices are never regressed by an absent observation. */
esp_err_t first_run_setup_reconcile(
    const first_run_setup_observed_t *observed);

/* Explicitly erase setup progress and restore the fresh first step. This also
 * permits deliberate recovery from a corrupt or unsupported stored schema. */
esp_err_t first_run_setup_reset(void);

#ifdef __cplusplus
}
#endif
