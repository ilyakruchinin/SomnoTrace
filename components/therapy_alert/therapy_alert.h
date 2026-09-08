/*
 * SomnoTrace - Therapy-stop alert system with ntfy push and buzzer escalation
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
#include <stdint.h>
#include "esp_err.h"

/* ── Configuration ──────────────────────────────────────────────────── */

typedef struct {
    bool     enabled;          /* master enable (opt-in)             */
    uint16_t win_start;        /* minutes from midnight (e.g. 1380 = 23:00) */
    uint16_t win_end;          /* minutes from midnight (e.g. 360 = 06:00); end < start = wraps */
    uint16_t delay1;           /* minutes after TherapyStop before push/buzzer */
    bool     push_en;          /* enable ntfy push notification      */
    char     ntfy_srv[64];     /* server base URL (e.g. "https://ntfy.sh") */
    char     ntfy_topic[64];   /* topic name                          */
    uint8_t  ntfy_prio;        /* ntfy priority (1-5, 5 = max)       */
    uint16_t delay2;           /* minutes after push before buzzer   */
    bool     buzz_en;          /* enable buzzer escalation           */
} therapy_alert_config_t;

#define ALERT_DEFAULTS { \
    .enabled = false, \
    .win_start = 1380, \
    .win_end = 360, \
    .delay1 = 5, \
    .push_en = true, \
    .ntfy_srv = "https://ntfy.sh", \
    .ntfy_topic = "", \
    .ntfy_prio = 5, \
    .delay2 = 5, \
    .buzz_en = true, \
}

/* ── Alert state (for /api/status and LCD) ──────────────────────────── */

typedef enum {
    ALERT_DISARMED = 0,
    ALERT_ARMED,
    ALERT_PENDING,
    ALERT_PUSH_SENT,
    ALERT_BUZZING,
    ALERT_ACKED,
} alert_state_t;

/* ── Injection (app → component) ────────────────────────────────────── */

/* Beep function signature (matches bsp_audio_beep). */
typedef esp_err_t (*alert_beep_fn_t)(int freq_hz, int duration_ms, uint8_t volume);

/* NVS executor function signature (matches nvs_writer_run). */
typedef esp_err_t (*alert_nvs_exec_fn_t)(esp_err_t (*fn)(void *), void *arg);

/* Therapy-active checker (matches bsp_display_is_therapy_active). */
typedef bool (*alert_therapy_active_fn_t)(void);

/* Inject the buzzer function (called from main.c at init). */
void therapy_alert_set_beep_fn(alert_beep_fn_t fn);

/* Inject the NVS executor (called from main.c at init, like uploader). */
void therapy_alert_set_nvs_executor(alert_nvs_exec_fn_t fn);

/* Inject the therapy-active checker (called from main.c at init). */
void therapy_alert_set_therapy_active_fn(alert_therapy_active_fn_t fn);

/* ── Lifecycle ──────────────────────────────────────────────────────── */

/* Initialise the alert subsystem: loads config from NVS, creates the
 * alert task.  Must be called after NVS is initialised. */
esp_err_t therapy_alert_init(void);

/* ── Event hooks (called from session_writer.c) ─────────────────────── */

/* Called when TherapyStart is detected.  Re-arms if inside the window. */
void therapy_alert_on_therapy_start(void);

/* Called when TherapyStop is detected.  Triggers the alert routine if armed. */
void therapy_alert_on_therapy_stop(void);

/* Called when the BLE link drops (disconnect).  Disarms without alerting. */
void therapy_alert_on_ble_disconnect(void);

/* Called when the PLUS button is single-clicked.  Acknowledges and silences. */
void therapy_alert_acknowledge(void);

/* ── Config API (for web UI / net_provision.c) ──────────────────────── */

/* Load config from NVS into cfg.  Returns ESP_OK on success. */
esp_err_t therapy_alert_load_config(therapy_alert_config_t *cfg);

/* Save config from JSON string to NVS.  Returns ESP_OK on success. */
esp_err_t therapy_alert_save_config_json(const char *json_str);

/* Get config as a newly-allocated JSON string.  Caller must free. */
esp_err_t therapy_alert_get_config_json(char **out_json);

/* Get current alert state. */
alert_state_t therapy_alert_get_state(void);

/* Get alert state as a string for /api/status. */
const char *therapy_alert_state_str(alert_state_t st);

/* Send a test push notification (for the "Send test push" button).
 * If json_override is non-NULL, parse push_en/ntfy_srv/ntfy_topic/ntfy_prio
 * from it; otherwise use the saved config. */
esp_err_t therapy_alert_send_test_push(const char *json_override);
