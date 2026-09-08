/*
 * SomnoTrace - ST7789 LCD driver
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

esp_err_t bsp_display_init(void);
void bsp_display_show_number(uint32_t value);
void bsp_display_show_lines(const char *title, const char *const *lines, int n_lines);

/* Persistent amber notice banner across the bottom of the status screen.
 * Survives any subsequent bsp_display_show_lines() call, so transient
 * messages cannot hide it. Pass NULL or "" to clear. */
void bsp_display_set_notice(const char *text);
void bsp_display_set_wifi_connected(bool connected);
void bsp_display_set_as11_paired(bool paired);
void bsp_display_set_battery(int percent, bool charging, bool valid);

/* Therapy graph mode */
void bsp_display_set_therapy_active(bool active);
void bsp_display_push_flow(float flow_lpm);
void bsp_display_push_leak(float leak_lpm);
void bsp_display_set_therapy_start_time(int64_t start_us);
bool bsp_display_is_therapy_active(void);

/* Backlight control (LEDC PWM on GPIO 46).
 * set_brightness: 1-200 (tenth-percent units: 1=0.1%, 200=20.0%), applied immediately.
 * set_backlight: hard on/off (used for therapy LCD-off mode).
 * toggle_backlight: flips the current hardware state and returns the new state.
 * get_brightness: returns last set brightness value. */
void bsp_display_set_brightness(uint8_t percent);
void bsp_display_set_backlight(bool on);
bool bsp_display_toggle_backlight(void);
uint8_t bsp_display_get_brightness(void);

/* Apply the current backlight policy based on backlight_mode and therapy
 * state.  Called after boot completes, when entering/leaving SoftAP, or
 * when settings change at runtime.
 *   force_on: if true, always turn backlight on (used for SoftAP mode). */
void bsp_display_apply_backlight_policy(bool force_on);

/* Temporarily wake the backlight for duration_sec seconds (e.g. on tap).
 * If already awake, extends the timeout. When duration_sec expires, returns
 * to the base backlight policy. */
void bsp_display_wake_temporary(uint32_t duration_sec);

/* Returns true if the backlight is currently on due to a temporary wake. */
bool bsp_display_is_temporarily_awake(void);

/* Cancel any active temporary wake and restore the base backlight policy immediately. */
void bsp_display_cancel_temporary_wake(void);

/* Set LCD rotation in degrees (0, 90, 180, 270). Applied by the display task
 * using the ST7789's reliable 0°/90° paths plus a software half-turn for
 * 180°/270°, and re-applied after panel reset. */
void bsp_display_set_rotation(uint16_t degrees);
