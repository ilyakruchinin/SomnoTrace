/*
 * SomnoTrace - Therapy detection and standby watchdog state machine
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
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
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
#include <stdio.h>

#include "snt_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pressure thresholds in 0.01 cmH2O units (parse_scaled_array scaling). */
#ifndef SW_AUTOSTART_PRESS_THRESH
#define SW_AUTOSTART_PRESS_THRESH  300     /* 3.00 cmH2O (min CPAP Rx is 4.0) */
#endif
#ifndef SW_STANDBY_PRESS_THRESH
#define SW_STANDBY_PRESS_THRESH    150     /* 1.50 cmH2O (standby ~0.1, dry ~0.5) */
#endif
#ifndef SW_THERAPY_PRESS_FLOOR
#define SW_THERAPY_PRESS_FLOOR     200     /* 2.00 cmH2O counts as real therapy */
#endif

/* Peak-to-peak flow span in 0.01 L/s units across the auto-start window.
 * Rejects flat standby baselines such as AS11 constant 0.63 L/s. */
#ifndef SW_AUTOSTART_FLOW_SPAN
#define SW_AUTOSTART_FLOW_SPAN     5       /* 0.05 L/s = 3 L/min */
#endif

/* Sustained duration required for mid-therapy auto-start (ms). */
#ifndef SW_AUTOSTART_SUSTAIN_MS
#define SW_AUTOSTART_SUSTAIN_MS    5000    /* 5 s */
#endif

/* Continuous standby duration before closing an active session (ms). */
#ifndef SW_HEURISTIC_STANDBY_MS
#define SW_HEURISTIC_STANDBY_MS    30000   /* 30 s for unconfirmed heuristic */
#endif
#ifndef SW_STANDBY_TIMEOUT_MS
#define SW_STANDBY_TIMEOUT_MS      180000  /* 3 min if TherapyStop missed */
#endif

/* Minimum cumulative therapeutic pressure before a timed-out session is kept
 * as real therapy rather than discarded as an aborted false trigger (ms). */
#ifndef SW_MIN_REAL_THERAPY_MS
#define SW_MIN_REAL_THERAPY_MS     60000   /* 60 s */
#endif

typedef struct {
    /* Lifecycle flags (RAM-only: therapy_stopped starts false on boot so a
     * mid-therapy reboot can recover without waiting for TherapyStart). */
    bool    therapy_stopped;
    bool    in_mask_fit;
    bool    in_cooldown;
    bool    started_from_event;
    bool    saw_zle_high;

    /* Sustained auto-start streak state */
    bool    streak_active;
    int64_t streak_start_ms;
    int16_t streak_flow_min;
    int16_t streak_flow_max;

    /* Active-session standby watchdog state */
    bool    standby_active;
    int64_t standby_start_ms;
    int64_t therapy_press_ms;
    int64_t last_sample_ms;
    bool    has_last_sample;
} sw_detect_t;

typedef enum {
    SW_WATCHDOG_OK = 0,
    SW_WATCHDOG_ABORT_NO_THERAPY,
    SW_WATCHDOG_CLOSE_TIMEOUT,
} sw_watchdog_action_t;

static inline void sw_detect_reset_streak(sw_detect_t *d)
{
    d->streak_active = false;
    d->streak_start_ms = 0;
    d->streak_flow_min = 0;
    d->streak_flow_max = 0;
}

static inline void sw_detect_on_session_open(sw_detect_t *d, bool from_event,
                                             int64_t now_ms)
{
    d->started_from_event = from_event;
    d->saw_zle_high = false;
    d->standby_active = false;
    d->standby_start_ms = 0;
    d->therapy_press_ms = 0;
    d->last_sample_ms = now_ms;
    d->has_last_sample = true;
    sw_detect_reset_streak(d);
}

static inline void sw_detect_on_therapy_start(sw_detect_t *d)
{
    d->therapy_stopped = false;
    d->in_mask_fit = false;
    d->in_cooldown = false;
    sw_detect_reset_streak(d);
}

static inline void sw_detect_on_therapy_stop(sw_detect_t *d)
{
    d->therapy_stopped = true;
    d->in_mask_fit = false;
    sw_detect_reset_streak(d);
}

static inline void sw_detect_on_mask_fit_start(sw_detect_t *d)
{
    d->in_mask_fit = true;
    sw_detect_reset_streak(d);
}

static inline void sw_detect_on_mask_fit_stop(sw_detect_t *d)
{
    d->in_mask_fit = false;
    d->therapy_stopped = true;
    sw_detect_reset_streak(d);
}

static inline void sw_detect_on_cooldown_start(sw_detect_t *d)
{
    d->in_cooldown = true;
    d->therapy_stopped = true;
    sw_detect_reset_streak(d);
}

static inline void sw_detect_on_cooldown_stop(sw_detect_t *d)
{
    d->in_cooldown = false;
    d->therapy_stopped = true;
    sw_detect_reset_streak(d);
}

static inline void sw_detect_on_standby_start(sw_detect_t *d)
{
    d->therapy_stopped = true;
    sw_detect_reset_streak(d);
}

static inline void sw_detect_on_zle(sw_detect_t *d, int zle_val,
                                      bool session_active)
{
    if (zle_val == 0) {
        if (!session_active) {
            d->therapy_stopped = true;
            sw_detect_reset_streak(d);
        }
    } else if (zle_val == 1) {
        if (session_active) {
            d->saw_zle_high = true;
        } else if (!d->in_mask_fit && !d->in_cooldown) {
            d->therapy_stopped = false;
        }
    }
}

static inline bool sw_detect_should_autostart(sw_detect_t *d,
                                              bool session_active,
                                              const int16_t *flow_vals, int flow_n,
                                              const int16_t *press_vals, int press_n,
                                              int64_t now_ms)
{
    if (session_active || d->therapy_stopped || d->in_mask_fit || d->in_cooldown) {
        sw_detect_reset_streak(d);
        return false;
    }

    bool press_ok = false;
    int16_t press_max = 0;
    for (int j = 0; j < press_n; j++) {
        if (press_vals[j] == SNT_MISSING) continue;
        if (!press_ok || press_vals[j] > press_max) {
            press_max = press_vals[j];
            press_ok = true;
        }
    }

    bool flow_ok = false;
    int16_t pkt_fmin = 0, pkt_fmax = 0;
    for (int j = 0; j < flow_n; j++) {
        if (flow_vals[j] == SNT_MISSING) continue;
        if (!flow_ok) {
            pkt_fmin = flow_vals[j];
            pkt_fmax = flow_vals[j];
            flow_ok = true;
        } else {
            if (flow_vals[j] < pkt_fmin) pkt_fmin = flow_vals[j];
            if (flow_vals[j] > pkt_fmax) pkt_fmax = flow_vals[j];
        }
    }

    if (!press_ok || !flow_ok || press_max < SW_AUTOSTART_PRESS_THRESH) {
        sw_detect_reset_streak(d);
        return false;
    }

    if (!d->streak_active) {
        d->streak_active = true;
        d->streak_start_ms = now_ms;
        d->streak_flow_min = pkt_fmin;
        d->streak_flow_max = pkt_fmax;
        return false;
    }

    if (pkt_fmin < d->streak_flow_min) d->streak_flow_min = pkt_fmin;
    if (pkt_fmax > d->streak_flow_max) d->streak_flow_max = pkt_fmax;

    int32_t span = (int32_t)d->streak_flow_max - (int32_t)d->streak_flow_min;
    if ((now_ms - d->streak_start_ms) >= SW_AUTOSTART_SUSTAIN_MS &&
        span >= SW_AUTOSTART_FLOW_SPAN) {
        sw_detect_reset_streak(d);
        return true;
    }
    return false;
}

static inline sw_watchdog_action_t sw_detect_check_active_stream(
    sw_detect_t *d, const int16_t *press_vals, int press_n, int64_t now_ms)
{
    bool press_ok = false;
    int16_t press_max = 0;
    for (int j = 0; j < press_n; j++) {
        if (press_vals[j] == SNT_MISSING) continue;
        if (!press_ok || press_vals[j] > press_max) {
            press_max = press_vals[j];
            press_ok = true;
        }
    }
    if (!press_ok) return SW_WATCHDOG_OK;

    int64_t dt_ms = d->has_last_sample ? (now_ms - d->last_sample_ms) : 0;
    if (dt_ms < 0) dt_ms = 0;
    if (dt_ms > 1000) dt_ms = 1000;
    d->last_sample_ms = now_ms;
    d->has_last_sample = true;

    if (press_max >= SW_THERAPY_PRESS_FLOOR) {
        d->therapy_press_ms += dt_ms;
    }

    if (press_max >= SW_STANDBY_PRESS_THRESH) {
        d->standby_active = false;
        d->standby_start_ms = 0;
        return SW_WATCHDOG_OK;
    }

    if (!d->standby_active) {
        d->standby_active = true;
        d->standby_start_ms = now_ms;
        return SW_WATCHDOG_OK;
    }

    int64_t limit_ms = (!d->started_from_event && !d->saw_zle_high)
                       ? SW_HEURISTIC_STANDBY_MS
                       : SW_STANDBY_TIMEOUT_MS;
    if ((now_ms - d->standby_start_ms) >= limit_ms) {
        d->standby_active = false;
        d->therapy_stopped = true;
        return (d->saw_zle_high || d->therapy_press_ms >= SW_MIN_REAL_THERAPY_MS)
               ? SW_WATCHDOG_CLOSE_TIMEOUT
               : SW_WATCHDOG_ABORT_NO_THERAPY;
    }

    return SW_WATCHDOG_OK;
}

#ifdef __cplusplus
}
#endif
