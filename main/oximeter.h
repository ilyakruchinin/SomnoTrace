/*
 * SomnoTrace - Oximeter BLE sync — public API (multi-driver dispatcher)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
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
 *
 * Clean-room implementation of the OxyII BLE protocol for Wellue O2 Ring S
 * and SleepHQ O2 Ring Pro, and the Legacy BLE protocol for Wellue O2 Ring
 * (Gen1) / ViaTom rings. Protocol studied from published documentation;
 * no third-party source code copied. See spec/0003-o2ring-ble-sync.md.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

/* Driver type — determines which BLE protocol to use for the paired ring. */
typedef enum {
    OX_DRIVER_OXYII  = 0,  /* Gen2: O2 Ring S / SHQO2Pro (OxyII protocol) */
    OX_DRIVER_LEGACY = 1,  /* Gen1: O2 Ring / ViaTom (Legacy protocol) */
    OX_DRIVER_AUTO   = 2,  /* Auto-detect: try OxyII first, fall back to Legacy */
} ox_driver_t;

/* Oximeter state machine states (returned by oximeter_get_status). */
#define OX_STATUS_IDLE       "idle"
#define OX_STATUS_SCANNING   "scanning"
#define OX_STATUS_CONNECTING "connecting"
#define OX_STATUS_PULLING    "pulling"
#define OX_STATUS_PAIRED     "paired"
#define OX_STATUS_ERROR      "error"
#define OX_STATUS_MONITORING "monitoring"  /* persistent connection held, polling LIVE_B */

/* Probe mode determines how the watch task monitors a worn ring.
 *
 * OX_PROBE_LEGACY:     Reconnect + full AUTH/SETUP/GET_INFO handshake every
 *                      ~60 s.  Most intrusive; suspected to cause HR artifacts
 *                      on some ring firmware revisions.
 * OX_PROBE_PERSISTENT: Hold one GATT connection, poll unauthenticated LIVE_B
 *                      (cmd=0x04) every 30 s.  AUTH/SETUP/GET_INFO and file
 *                      transfer are deferred until off-finger is detected.
 *                      Recommended mode (see .ai/OXIMETRY2.md). */
typedef enum {
    OX_PROBE_LEGACY     = 0,
    OX_PROBE_PERSISTENT = 1,
} ox_probe_mode_t;

/* Initialise the oximeter module.  Must be called after as11_ble_init()
 * (shares the NimBLE host) and sd_storage_init().  Loads paired serial
 * from NVS and starts the background watch task. */
esp_err_t oximeter_init(void);

/* Start a BLE scan for oximeter rings (both OxyII and Legacy protocols).
 * Blocks until scan completes.  AS11 connection stays up.
 * Results retrieved via oximeter_get_scan_results(). */
esp_err_t oximeter_scan(int timeout_sec);

/* Return a cJSON array of discovered rings.
 * Each element: {"addr":"AA:BB:...","name":"...","rssi":-65,"type":"oxyii"|"legacy"}
 * Caller must cJSON_Delete(). */
cJSON *oximeter_get_scan_results(void);

/* Pair with the ring at the given BLE address.  Connects, runs GET_INFO,
 * stores the serial + driver type in NVS + paired.json, disconnects.
 * Replaces any previously paired ring.  Non-blocking — runs in a background task.
 * driver selects the BLE protocol to use (OxyII for Gen2, Legacy for Gen1). */
esp_err_t oximeter_pair(const char *addr_str, ox_driver_t driver);

/* Forget the paired ring: erase NVS + paired.json.  Keeps files/. */
esp_err_t oximeter_forget(void);

/* Return the current state string (one of OX_STATUS_*). */
const char *oximeter_get_status(void);

/* Return the last error message (valid when status is "error"). */
const char *oximeter_get_error(void);

/* Return true if a ring serial is stored in NVS. */
bool oximeter_is_paired(void);

/* Return a cJSON object with paired ring info: serial, firmware,
 * name_prefix, last_addr, driver.  NULL if not paired.  Caller cJSON_Delete(). */
cJSON *oximeter_get_paired_info(void);

/* Return the driver type of the currently paired ring.
 * Returns OX_DRIVER_OXYII if not paired (default). */
ox_driver_t oximeter_get_driver(void);

/* Get/set the probe mode (see ox_probe_mode_t).  Persists to NVS.
 * The watch task picks up the new mode on its next iteration. */
ox_probe_mode_t oximeter_get_probe_mode(void);
esp_err_t oximeter_set_probe_mode(ox_probe_mode_t mode);
