/*
 * SomnoTrace - Battery power latch and power button control
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

/* Latch the battery power rail on.
 *
 * The board has a soft power circuit: pressing KEY_PWR (IO5) momentarily
 * connects the battery, but BAT_EN (IO2) must be driven HIGH to keep the
 * power MOSFET latched once the button is released. Call this as the very
 * first thing at boot, before any delays, or the device powers off on
 * USB-C disconnect / button release. */
void bsp_power_hold(void);

/* Immediately release the battery latch (powers off when on battery only;
 * stays on if USB-C is supplying power). */
void bsp_power_off(void);

/* Start a background task that powers the device off when KEY_PWR (IO5) is
 * held for hold_ms milliseconds. */
void bsp_power_start_button_monitor(int hold_ms);

/* Start a background task that sets the flag at softap_flag when BOOT (IO0)
 * is held for hold_ms milliseconds. softap_flag must remain valid for the
 * lifetime of the task. */
void bsp_power_start_boot_monitor(volatile bool *softap_flag, int hold_ms);

/* Immutable snapshot of the battery state, published by the monitor task. */
typedef struct {
    int      percent;      /* 0-100 (slew-limited for display), -1 if unknown */
    int      millivolts;   /* filtered VBAT in mV, -1 if unknown */
    bool     charging;     /* true while CHG_STAT is low */
    bool     valid;        /* false until the first successful sample burst */
    uint32_t age_s;        /* seconds since the last successful sample burst */
} bsp_battery_t;

/* Start the background battery monitor.  Call once at boot, after
 * bsp_power_hold().  The monitor task is the only owner of the ADC:
 * it samples on a slow cadence and publishes a snapshot that all
 * consumers (LCD, /api/status, ...) read for free.  Safe to call twice. */
esp_err_t bsp_power_battery_monitor_start(void);

/* Read the latest published battery snapshot.  Non-blocking, never touches
 * the ADC, safe from any task.  Before the first burst completes, *out has
 * valid=false and percent/millivolts = -1. */
void bsp_power_battery_get(bsp_battery_t *out);

/* Battery charge level as a percentage (0-100), or -1 if not yet known.
 * Convenience wrapper over bsp_power_battery_get(). */
int bsp_power_battery_percent(void);

/* Returns true if the battery is currently charging (CHG_STAT pin low).
 * Reads the GPIO directly — cheap and always current. */
bool bsp_power_is_charging(void);

/* Start a background task that monitors the PLUS button (IO4) for
 * double-clicks. On a double-click, toggles AS11 therapy: sends
 * EnterStandby RPC if therapy is active, or EnterTherapy RPC if not. */
void bsp_power_start_plus_monitor(void);
