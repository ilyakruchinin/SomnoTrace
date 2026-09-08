/*
 * SomnoTrace - Device hardware settings (brightness, LCD therapy mode)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
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

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Screen to display during therapy */
typedef enum {
    THERAPY_SCREEN_INFO   = 0,  /* Info panel: leak rate + session runtime (default) */
    THERAPY_SCREEN_GRAPH  = 1,  /* Live flow graph */
    THERAPY_SCREEN_STATUS = 2,  /* Standby / main status screen */
} therapy_screen_t;

/* LCD backlight policy */
typedef enum {
    BACKLIGHT_MODE_ON         = 0,  /* Backlight always on (default) */
    BACKLIGHT_MODE_OFF_THRP   = 1,  /* Backlight off during therapy, on otherwise */
    BACKLIGHT_MODE_ALWAYS_OFF = 2,  /* Backlight always off (dark bedroom / battery) */
} backlight_mode_t;

/* LCD rotation in clockwise degrees (0 = default) */
typedef enum {
    LCD_ROTATION_0   = 0,
    LCD_ROTATION_90  = 90,
    LCD_ROTATION_180 = 180,
    LCD_ROTATION_270 = 270,
} lcd_rotation_t;

typedef struct {
    uint8_t brightness;        /* tenth-percent units: 1=0.1%, 200=20.0% */
    therapy_screen_t therapy_screen; /* screen shown during therapy */
    backlight_mode_t backlight_mode; /* backlight behavior */
    uint8_t alert_volume;      /* speaker volume for alerts: 0-100 */
    uint16_t lcd_rotation;     /* clockwise degrees: 0, 90, 180, or 270 */
    bool battery_enabled;      /* true to display battery indicator, false to hide */
    /* Temporary wake on touch */
    bool wake_on_touch;        /* true to wake screen on capacitive touch */
    uint8_t wake_timeout_sec;  /* duration in seconds (5, 10, 15, 30, 60; 0=disabled) */
} device_settings_t;

/* Load settings from NVS. Returns ESP_OK if loaded, ESP_ERR_NVS_NOT_FOUND
 * if no settings stored (defaults are filled in). */
esp_err_t device_settings_load(device_settings_t *cfg);

/* Save settings to NVS. */
esp_err_t device_settings_save(const device_settings_t *cfg);

/* Get current in-memory settings (loaded at boot). */
const device_settings_t *device_settings_get(void);

/* Check whether the battery indicator is enabled. */
bool device_settings_battery_enabled(void);

/* Set battery indicator enabled/disabled. Call device_settings_save() to persist. */
esp_err_t device_settings_set_battery_enabled(bool enabled);

/* Set brightness immediately (applies to hardware + updates in-memory copy).
 * Does NOT persist to NVS — call device_settings_save() for that. */
esp_err_t device_settings_set_brightness(uint8_t percent);

/* Set therapy screen (updates in-memory copy only).
 * Call device_settings_save() to persist. */
esp_err_t device_settings_set_therapy_screen(therapy_screen_t screen);

/* Set backlight policy (updates in-memory copy only).
 * Call device_settings_save() to persist. */
esp_err_t device_settings_set_backlight_mode(backlight_mode_t mode);

/* Set alert speaker volume (0-100). Updates in-memory copy and applies to
 * bsp_audio. Call device_settings_save() to persist. */
esp_err_t device_settings_set_alert_volume(uint8_t percent);

/* Set LCD rotation (0, 90, 180, or 270 degrees). Updates in-memory copy and
 * applies to hardware immediately. Call device_settings_save() to persist. */
esp_err_t device_settings_set_lcd_rotation(uint16_t degrees);

/* Get settings as JSON string for web UI. Caller must free(). */
esp_err_t device_settings_get_json(char **out_json);

/* Save settings from JSON string (from web UI POST body).
 * Also applies brightness immediately. */
esp_err_t device_settings_save_json(const char *json_str);
