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
bool bsp_display_set_therapy_active(bool active);
/* Two-phase therapy-safe restart gate. Reserve before acquiring the SD lease;
 * then commit only after the lease is held. A concurrent therapy start waits
 * for cancellation and forces commit to fail, so it cannot lose its recording
 * claim to a restart that subsequently defers. No state lock spans SD calls. */
bool bsp_display_try_reserve_therapy_safe_restart(void);
bool bsp_display_try_commit_therapy_safe_restart(void);
void bsp_display_cancel_therapy_safe_restart(void);
/* Hold this short-lived claim around a local command that can start therapy.
 * It participates in the same restart reservation and must always be released. */
bool bsp_display_reserve_therapy_start(void);
void bsp_display_release_therapy_start(void);
/* Account for raw AirSense notifications from enqueue through dispatch.  A
 * queued TherapyStart must be visible to the restart gate before its JSON is
 * decrypted and classified by the worker. */
void bsp_display_note_as11_notification_queued(void);
void bsp_display_note_as11_notification_processed(void);
/* Cancellable maintenance gate used by OTA. Unlike the final restart gate,
 * it never blocks therapy publication: an independent start makes
 * should_abort true so recording wins immediately. */
bool bsp_display_try_begin_therapy_safe_maintenance(void);
bool bsp_display_therapy_safe_maintenance_should_abort(void);
/* Short atomic OTA boot-selection reservation; cancel after SDK commit returns. */
bool bsp_display_try_reserve_maintenance_commit(void);
void bsp_display_end_therapy_safe_maintenance(void);
void bsp_display_push_flow(float flow_lpm);
/* Preserve absent 25 Hz positions without synthesizing physiological values. */
void bsp_display_push_flow_gap(uint32_t samples);
void bsp_display_push_leak(float leak_lpm);
/* Live two-second metrics. Pass NAN for an unavailable value. */
void bsp_display_push_metrics(float pressure_cmh2o, float respiratory_rate,
                              float flow_limitation);
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

void bsp_display_push_flow_gap(uint32_t samples);
void bsp_display_set_critical_notice(const char *text);

/* Storage publishes readiness; the compact screen has no separate SD badge. */
void bsp_display_set_sd_ready(bool ready);
