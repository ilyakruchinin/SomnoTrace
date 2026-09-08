/*
 * SomnoTrace - NTP time synchronisation with DHCP option 42 support
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
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Initialise SNTP after Wi-Fi STA connects.
 * Reads timezone from NVS (POSIX TZ string), configures timezone, starts SNTP
 * in poll mode (1-hour re-sync interval). Tries DHCP option 42 servers first,
 * falls back to public NTP pools. Defaults to UTC if no timezone is set. */
esp_err_t time_sync_init(void);

/* Apply the timezone stored in NVS to the C library (setenv TZ + tzset)
 * without touching SNTP.  Safe to call before the network is up, and called
 * again by time_sync_init().  Call this early in app_main() so that a session
 * started during a mid-therapy BLE reconnect is named in local time rather
 * than UTC. */
void time_sync_apply_saved_timezone(void);

/* Get/set timezone as POSIX TZ string and IANA name, stored in NVS.
 * tz_str: e.g. "AEST-10AEDT,M10.1.0,M4.1.0/3"
 * tz_name: e.g. "Australia/Melbourne" (for UI re-selection only) */
esp_err_t time_sync_set_timezone(const char *tz_str, const char *tz_name);
void time_sync_get_timezone(char *tz_str, size_t tz_str_len);
void time_sync_get_tz_name(char *tz_name, size_t tz_name_len);

/* Get/set custom NTP server hostname, stored in NVS.
 * Pass NULL or empty string to clear (revert to auto/DHCP mode).
 * When set, time_sync_init uses this server exclusively. */
esp_err_t time_sync_set_ntp_server(const char *server);
void time_sync_get_ntp_server(char *server, size_t server_len);

/* Returns true if system time has been synchronised via NTP. */
bool time_sync_is_synced(void);

/* ── Time-source provenance and degraded mode ──────────────────────── */

typedef enum {
    TIME_SRC_NONE = 0,       /* unusable — must not record            */
    TIME_SRC_AS11_DRIFT,     /* degraded — AS11 clock + stored drift  */
    TIME_SRC_NTP,            /* authoritative                         */
} time_source_t;

/* Returns the current time source. */
time_source_t time_source_get(void);

/* Returns true if the system clock is usable for recording (any source
 * except TIME_SRC_NONE).  Replaces time_sync_is_synced() as the gate
 * for therapy recording — NTP is no longer the only valid source. */
bool time_is_usable(void);

/* Returns the age of the drift sample in milliseconds (time since it was
 * measured), or -1 if no drift sample is available. */
int64_t time_source_drift_age_ms(void);

/* Returns true if a drift sample is available (in NVS or already loaded).
 * Can be called at boot before BLE connects to decide whether to enter
 * degraded mode or reboot. */
bool time_sync_has_drift(void);

/* Persisted-drift snapshot.
 *
 * Returns the value, when it was measured, and where it came from together,
 * so a consumer can judge whether an *estimate* is fit for its purpose
 * instead of silently treating it like a measurement.  Note that age must
 * be judged against the session or checkpoint being recovered: crash
 * recovery runs before wall time is usable, so "now - measured_at" is
 * meaningless there. */
typedef struct {
    bool        available;
    int64_t     drift_ms;         /* NTP_epoch_ms - AS11_epoch_ms          */
    int64_t     measured_at_ms;   /* NTP epoch ms when it was measured     */
    const char *source;           /* "nvs" | "sd" | "none"                 */
} time_drift_snapshot_t;

/* Fills *out with the best available persisted drift.  Loads from NVS (or
 * the SD upgrade fallback) if not already resident.  Returns out->available. */
bool time_sync_get_drift_snapshot(time_drift_snapshot_t *out);

/* Persist the most recent valid clock drift to NVS for use by the
 * degraded-mode fallback.  Called at session stop when clock_drift_valid.
 * drift_ms: NTP_epoch_ms - AS11_epoch_ms (positive = AS11 is behind).
 * measured_at_ms: NTP epoch ms when the drift was measured.
 * Safe to call from PSRAM-stack tasks (delegates to nvs_writer). */
void time_sync_save_drift(int64_t drift_ms, int64_t measured_at_ms);

/* Attempt to recover time without NTP by combining the AS11 BLE clock
 * with the most recently stored drift sample.
 *
 * 1. Load drift from NVS (or scan SD for newest _session.json with
 *     clock_drift_valid as an upgrade fallback).
 * 2. Call as11_ble_get_datetime() for the current AS11 wall clock.
 * 3. settimeofday(as11_ms + drift_ms) and set TIME_SRC_AS11_DRIFT.
 *
 * Returns ESP_OK on success, ESP_ERR_NOT_FOUND if no drift sample,
 * ESP_FAIL if AS11 clock query fails.  Must be called after the
 * encrypted BLE session is established. */
esp_err_t time_sync_recover_from_as11(void);

/* Block until the initial NTP sync succeeds or all attempts are exhausted.
 * Makes up to 3 attempts with 15-second timeouts. Returns true on success.
 * Must be called after time_sync_init(). Subsequent periodic re-syncs do
 * not trigger the failure path. */
bool time_sync_wait_initial(void);
