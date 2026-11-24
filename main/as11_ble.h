/*
 * SomnoTrace - BLE transport for ResMed AirSense 11 pairing
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
#include "esp_err.h"
#include "cJSON.h"

/* Pairing state machine states (returned by as11_ble_get_status). */
#define AS11_STATUS_IDLE          "idle"
#define AS11_STATUS_SCANNING      "scanning"
#define AS11_STATUS_CONNECTING    "connecting"
#define AS11_STATUS_WAIT_PASSKEY  "waiting_passkey"
#define AS11_STATUS_CONFIRMING    "confirming"
#define AS11_STATUS_PAIRED        "paired"
#define AS11_STATUS_ERROR         "error"

/* Initialise the NimBLE host and start the BLE host task.
 * Call once at boot, after netprov_init(). */
esp_err_t as11_ble_init(void);

/* Return true if the NimBLE host has synced and is ready for operations.
 * Shared with the oximeter module which reuses the same NimBLE host. */
bool as11_ble_is_host_ready(void);

/* Return the own address type for ble_gap_disc/ble_gap_connect calls.
 * Shared with the oximeter module. */
uint8_t as11_ble_get_own_addr_type(void);

/* Start an active BLE scan for AirSense 11 devices.
 * Results are collected internally; retrieve with as11_ble_get_scan_results().
 * timeout_sec: scan duration in seconds (typically 5–10). */
esp_err_t as11_ble_scan(int timeout_sec);

/* Return a cJSON array of discovered AS11 devices.
 * Each element: {"addr":"AA:BB:CC:DD:EE:FF","name":"ResMed ...","rssi":-65}
 * Caller must call cJSON_Delete() on the returned object. */
cJSON *as11_ble_get_scan_results(void);

/* Address type of a scan hit (BLE_ADDR_PUBLIC / _RANDOM), so a client can
 * connect with the type the peer actually advertised. */
bool as11_ble_scan_lookup(const char *addr_str, uint8_t *addr_type);
/* Same, plus whether that hit is a SomnoBridge (the pair router trusts this
 * over whatever the browser sends, so a stale cached page cannot send a
 * bridge to the AS11 path). */
bool as11_ble_scan_lookup_ex(const char *addr_str, uint8_t *addr_type,
                             bool *is_bridge);

/* Begin pairing with the device at the given BLE address (AA:BB:CC:DD:EE:FF).
 * This connects, discovers GATT services, and sends StartKeyExchange.
 * When the AS11 shows a 4-digit passkey on its LCD, the state transitions
 * to AS11_STATUS_WAIT_PASSKEY. Call as11_ble_confirm_pair() next. */
esp_err_t as11_ble_start_pair(const char *addr_str);

/* Complete pairing with the 4-digit passkey shown on the AS11 screen.
 * Computes SRP proof, sends ConfirmKeyExchange, and saves credentials
 * (clientId + masterPairKey) to NVS on success. */
esp_err_t as11_ble_confirm_pair(const char *passkey);

/* Return the current state string (one of AS11_STATUS_* constants). */
const char *as11_ble_get_status(void);

/* Return the last error message (valid when status is "error"). */
const char *as11_ble_get_error(void);

/* Return true if credentials are stored in NVS (device was previously paired). */
bool as11_ble_is_paired(void);

/* Return a cJSON object with paired device info from the in-RAM cache.
 * Keys: "addr", "name", "clientId". NULL if not paired.
 * Caller must call cJSON_Delete(). */
cJSON *as11_ble_get_paired_info(void);

/* Erase stored pairing credentials from NVS. */
esp_err_t as11_ble_forget(void);

/* Disconnect the active BLE connection if any.
 * Frees the radio for WiFi-only operations (e.g. SoftAP scanning). */
esp_err_t as11_ble_disconnect(void);

/* Stop the AS11 data stream. Currently a no-op — the stream continues
 * between sessions and is restarted on next reconnect. Stopping via
 * StartStream with empty dataIds or CCCD write would block RPC responses
 * (which arrive as notifications on the same characteristic).
 * Call before as11_ble_get_datetime() to free BLE buffers (future use). */
esp_err_t as11_ble_stop_stream(void);

/* Compute clock drift from AS11 clock captured before stream start.
 * drift = NTP_time - AS11_time (positive = AS11 clock is behind).
 * Returns ESP_OK and stores drift in *out_drift_ms. */
esp_err_t as11_ble_get_clock_drift(int64_t *out_drift_ms);

/* Query the AS11 device clock via GetDateTime RPC.
 * Returns ESP_OK and stores epoch milliseconds in *out_epoch_ms.
 * Requires an active encrypted BLE session with available ACL buffers. */
esp_err_t as11_ble_get_datetime(int64_t *out_epoch_ms);

/* Extended GetDateTime variant.  In addition to the AS11 clock it returns
 * the NTP wall-clock midpoint of the RPC round trip — the instant the
 * returned AS11 reading best corresponds to.  This matters on the split
 * path: the session's back-dated end_epoch_ms is correct as a session
 * boundary but must NOT be used to measure clock drift, because the AS11
 * clock read happens several seconds after that boundary
 * (see .ai/RECONNECT/PLAN.md Phase B.1). */
esp_err_t as11_ble_get_datetime_ex(int64_t *out_epoch_ms, int64_t *out_meas_ms);

/* Query AS11 firmware identity via GetVersion and write a compact
 * one-line summary into buf ("FlowGenerator=... BluetoothModule=...").
 * Requires an active encrypted BLE session. */
esp_err_t as11_ble_get_version(char *buf, size_t buflen);

/* ── Spool RPC ────────────────────────────────────────────────────────
 * Post-therapy spool data collection.  The AS11 stores session summaries,
 * event logs, and other data in internal "spools" that are retrieved via
 * the StartSpool / PullSpoolFragments RPC cycle.  SpoolFragment
 * notifications arrive asynchronously on the same BLE characteristic as
 * StreamData and RPC responses.
 *
 * as11_ble_spool_pull() handles the full cycle: StartSpool →
 * PullSpoolFragments → collect SpoolFragment notifications → reassemble.
 * Multi-round pulls (SPOOL_COMPLETE_MORE_DATA_PENDING) are handled
 * automatically by looping with the nextSpoolAddress from the last
 * fragment.
 *
 * Returns ESP_OK on success.  *out_data is heap-allocated; caller frees.
 * Parameters:
 *   spool_type  - e.g. "Summary", "TherapyEvents-RespiratoryEvents"
 *   from_dt     - ISO-8601 start time, e.g. "2026-06-25T00:00:00.000Z"
 *   out_data    - receives malloc'd buffer with raw protobuf bytes
 *   out_len     - receives buffer length
 */
esp_err_t as11_ble_spool_pull(const char *spool_type, const char *from_dt,
                              uint8_t **out_data, size_t *out_len);

/* Same pull cycle as as11_ble_spool_pull(), but stops accumulation once
 * the total payload would exceed max_bytes and returns ESP_ERR_INVALID_SIZE
 * (with *out_data freed).  Used by the deferred diagnostic pull to bound
 * BLE airtime and RAM use (see .ai/RECONNECT/PLAN.md Phase B.3/C). */
esp_err_t as11_ble_spool_pull_bounded(const char *spool_type, const char *from_dt,
                                      uint8_t **out_data, size_t *out_len,
                                      size_t max_bytes);

/* Send a Get RPC for multiple variable names (encrypted).
 * Returns a cJSON result object (caller must cJSON_Delete) or NULL.
 * The result is the "result" object from the RPC response, mapping
 * variable names to their values. */
cJSON *as11_ble_get_values(const char *const *keys, int n_keys);

/* Monotonic counter incremented each time an encrypted BLE session with the
 * AS11 finishes setup and enters AS11_STATUS_PAIRED. */
uint32_t as11_ble_get_session_epoch(void);

/* Block until a new encrypted BLE session (with epoch != prev_epoch) is
 * established and ready, or until timeout_ms elapses.  Used by post-therapy
 * collection to passively wait out a 30 s ATT transaction timeout + auto-
 * reconnect before retrying a failed metadata RPC. */
bool as11_ble_wait_for_reconnect(uint32_t prev_epoch, uint32_t timeout_ms);

/* Send EnterStandby RPC to stop therapy on the AS11.
 * Requires an active encrypted BLE session.
 * Returns ESP_OK on success. */
esp_err_t as11_ble_stop_therapy(void);

/* Send EnterTherapy RPC to start therapy on the AS11.
 * Requires an active encrypted BLE session.
 * Returns ESP_OK on success. */
esp_err_t as11_ble_start_therapy(void);

/* Generic BLE JSON-RPC passthrough interface.
 * Transmits json_in over the encrypted BLE session, awaits the AS11 response,
 * and returns the decrypted response string in *json_out (malloc'd, caller frees).
 * Requires an active encrypted BLE session.
 * A PullSpoolFragments request also arms the fragment collector so the
 * asynchronous SpoolFragment notifications are returned in *json_out. */
esp_err_t as11_ble_passthrough_rpc(const char *json_in, char **json_out, uint32_t timeout_ms);

/* ── Incident latch ───────────────────────────────────────────────────
 * Set when a supervision-timeout disconnect (HCI 0x08) or an unfillable
 * long gap occurs.  Checked (and consumed) at the next post-therapy run
 * that finds therapy fully stopped — this is where the deferred
 * diagnostic spool pull happens, never on the split path itself
 * (see .ai/RECONNECT/PLAN.md Phase C).  RAM-only: a reboot intentionally
 * clears it so a stale flag can never trigger BLE work days later. */
void    as11_ble_incident_mark(const char *kind);
bool    as11_ble_incident_pending(void);
int64_t as11_ble_incident_time_ms(void);   /* 0 = unknown */
void    as11_ble_incident_note_attempt(void);
int     as11_ble_incident_attempts(void);
void    as11_ble_incident_clear(void);

/* ── Experimental mitigations (Phase D, opt-in, persisted in NVS) ─────
 * supv_timeout_ms: 0 = accept peer-negotiated supervision timeout
 *                  (default).  Non-zero = after the peer's own parameter
 *                  update settles, issue a central-initiated
 *                  LL_CONNECTION_UPDATE_IND raising the supervision
 *                  timeout to this value (100 ms units applied
 *                  internally).  Unproven against reason-520 drops.
 * coex_fix:        false = keep the historical (argument-inverted)
 *                  esp_coex_status_bit_set call for bug-compat;
 *                  true  = emit the corrected BLE status bit. */
uint16_t  as11_ble_exp_supv_timeout_ms(void);
esp_err_t as11_ble_exp_supv_timeout_set(uint16_t ms);
bool      as11_ble_exp_coex_fix(void);
esp_err_t as11_ble_exp_coex_fix_set(bool on);

