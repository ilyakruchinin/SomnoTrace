/*
 * SomnoTrace - Oximeter driver interface (internal)
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

#include "oximeter.h"

/* This header defines the interface that each oximeter BLE driver must
 * implement.  The dispatcher (oximeter.c) routes public API calls to the
 * active driver based on the paired device type stored in NVS.
 *
 * Each driver is a self-contained .c file that implements all functions
 * below as static, then registers them via this struct.  The dispatcher
 * holds a pointer to the active driver's vtable. */

typedef struct {
    /* Called once at init (after semaphores and NVS are loaded). */
    void (*init)(void);

    /* Start a BLE scan.  The driver should populate its internal scan
     * results and return ESP_OK.  Caller holds s_ops_mtx. */
    esp_err_t (*scan)(int timeout_sec);

    /* Return a cJSON array of scan results.  Each element includes
     * "addr", "name", "rssi", and "type" fields. */
    cJSON *(*get_scan_results)(void);

    /* Pair with the device at addr_str.  Non-blocking — runs in a
     * background task.  Stores serial + metadata via ox_store_save_paired. */
    esp_err_t (*pair)(const char *addr_str);

    /* Forget the paired device.  Clears driver-internal state. */
    void (*forget)(void);

    /* Return current status string (one of OX_STATUS_*). */
    const char *(*get_status)(void);

    /* Return last error message. */
    const char *(*get_error)(void);

    /* Return true if a device is paired. */
    bool (*is_paired)(void);

    /* Return cJSON object with paired device info. */
    cJSON *(*get_paired_info)(void);

    /* Get/set probe mode. */
    ox_probe_mode_t (*get_probe_mode)(void);
    esp_err_t (*set_probe_mode)(ox_probe_mode_t mode);
} ox_driver_ops_t;

/* Driver registrations — each driver .c file provides a const pointer. */
extern const ox_driver_ops_t oxyii_driver_ops;
extern const ox_driver_ops_t legacy_driver_ops;
