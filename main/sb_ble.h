/*
 * SomnoTrace - SomnoBridge BLE client (Phase 2 of IMPORT-EDFs.md)
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

/* SomnoBridge is the alternative CPAP BLE source to the AirSense 11 (slot 1):
 * the bridge owns the EZShare card, SomnoTrace pulls its DATALOG files over
 * BLE and stages them into /somnotrace/IMPORT/, where the Phase-1 importer
 * converts them exactly like an FTP drop.
 *
 * Wire protocol: somnopal/docs/implementation-spec.md §10 (16-byte frame
 * header, JSON ops on Control, byte streams on Data, binary time exchange).
 * SomnoTrace is the sole persistent sync authority (§5.4): the bridge's
 * catalog is volatile, our bridge_ledger.json decides what to re-pull. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#define SB_STATUS_IDLE          "idle"
#define SB_STATUS_CONNECTING    "connecting"
#define SB_STATUS_WAIT_PASSKEY  "waiting_passkey"
#define SB_STATUS_PAIRED        "paired"
#define SB_STATUS_ERROR         "error"

/* Start the client on the shared NimBLE host (call after as11_ble_init()).
 * Idempotent; spawns the status/sync task and auto-reconnects when bonded. */
esp_err_t sb_ble_init(void);

bool        sb_ble_is_available(void);      /* NimBLE host ready            */
bool        sb_ble_is_paired(void);         /* bonded peer known            */
const char *sb_ble_get_status(void);        /* SB_STATUS_*                  */
const char *sb_ble_get_error(void);

/* Pairing: connect, request MTU/PHY, initiate LE Secure Connections, then
 * wait for the 6-digit PIN the bridge shows on its LCD. */
esp_err_t sb_ble_start_pair(const char *addr_str);
/* Same, with the address type the peer advertised (random-static bridges
 * must not be dialled as BLE_ADDR_PUBLIC). */
esp_err_t sb_ble_start_pair_ex(const char *addr_str, uint8_t addr_type);
esp_err_t sb_ble_confirm_pair(const char *pin6);
esp_err_t sb_ble_forget(void);
esp_err_t sb_ble_disconnect(void);

/* Telemetry for /api/status and the portal: state, peer, therapy, catalog,
 * last sync, ledger counters.  Caller frees. */
cJSON    *sb_ble_status_json(void);

/* "Sync Now" (days=14) / "Full Sync" (days=365).  Returns immediately; the
 * sync task does the work under its own lock. */
esp_err_t sb_ble_sync_now(bool full);

/* True while therapy is active according to the bridge (used to keep the
 * AS11-side session logic from treating a therapy night as "no data"). */
bool sb_ble_therapy_active(void);

/* Injected by main.c: run the Phase-1 importer over freshly staged files. */
typedef void (*sb_import_kick_fn_t)(void);
void sb_ble_set_import_kick_fn(sb_import_kick_fn_t fn);
