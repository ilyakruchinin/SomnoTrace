/*
 * SomnoTrace - Host unit tests for therapy detection & standby watchdog
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

#include <stdio.h>
#include <string.h>

#include "session_detect.h"

static int g_failures;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));          \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

static void feed_packets(sw_detect_t *d, int64_t *t_ms, int count,
                         int16_t press, const int16_t *flow5, bool *triggered)
{
    int16_t p[5] = {press, press, press, press, press};
    for (int i = 0; i < count; i++) {
        *t_ms += 200;
        if (sw_detect_should_autostart(d, false, flow5, 5, p, 5, *t_ms)) {
            *triggered = true;
        }
    }
}

static void test_cooldown_reboot_ghost_blocked(void)
{
    sw_detect_t d = {0};
    int16_t standby_flow[5] = {63, 63, 63, 63, 63};
    int16_t breath_flow[5] = {-20, 0, 25, 40, 10};
    int16_t spike_p[5] = {450, 450, 450, 450, 450};
    int16_t dry_p[5] = {50, 50, 50, 50, 50};
    int64_t t = 1000;

    /* Boot during cooldown: _ZLE=0 arrives on subscribe */
    sw_detect_on_zle(&d, 0, false);
    CHECK(d.therapy_stopped, "idle _ZLE=0 disarms auto-start");

    /* Cooldown airflow (0.5 cmH2O, constant 0.63 flow) */
    for (int i = 0; i < 10; i++) {
        t += 200;
        CHECK(!sw_detect_should_autostart(&d, false, standby_flow, 5, dry_p, 5, t),
              "cooldown packets never start session");
    }

    /* CooldownStopped followed by a transient pressure spike */
    sw_detect_on_cooldown_stop(&d);
    CHECK(d.therapy_stopped, "CooldownStopped keeps auto-start disarmed");
    t += 200;
    CHECK(!sw_detect_should_autostart(&d, false, standby_flow, 5, spike_p, 5, t),
          "post-cooldown pressure spike blocked");
    for (int i = 0; i < 30; i++) {
        t += 200;
        CHECK(!sw_detect_should_autostart(&d, false, breath_flow, 5, spike_p, 5, t),
              "sustained pressure after CooldownStopped stays blocked without TherapyStart/_ZLE=1");
    }
}

static void test_lifecycle_events_disarm_autostart(void)
{
    sw_detect_t d = {0};
    sw_detect_on_cooldown_stop(&d);
    CHECK(d.therapy_stopped, "CooldownStopped alone disarms after mid-cooldown reboot");

    memset(&d, 0, sizeof(d));
    sw_detect_on_standby_start(&d);
    CHECK(d.therapy_stopped, "idle StandbyStarted disarms auto-start");

    memset(&d, 0, sizeof(d));
    sw_detect_on_mask_fit_start(&d);
    CHECK(d.in_mask_fit, "MaskFitStart sets in_mask_fit");
    sw_detect_on_mask_fit_stop(&d);
    CHECK(!d.in_mask_fit && d.therapy_stopped,
          "MaskFitStop clears mask_fit and leaves auto-start disarmed");
}

static void test_flat_standby_flow_rejected(void)
{
    sw_detect_t d = {0};
    int16_t flat_flow[5] = {63, 63, 63, 63, 63};
    int64_t t = 0;
    bool triggered = false;

    /* Armed on boot, 4.0 cmH2O pressure, but constant 0.63 standby flow for 10s */
    feed_packets(&d, &t, 50, 400, flat_flow, &triggered);
    CHECK(!triggered, "flat 0.63 L/s standby baseline never triggers auto-start");
}

static void test_transient_blip_resets_streak(void)
{
    sw_detect_t d = {0};
    int16_t breath_flow[5] = {-10, 5, 20, 35, 10};
    int16_t low_p[5] = {50, 50, 50, 50, 50};
    int64_t t = 0;
    bool triggered = false;

    /* 4.8 s of therapy pressure (< 5 s sustain) */
    feed_packets(&d, &t, 24, 400, breath_flow, &triggered);
    CHECK(!triggered, "4.8 s does not reach 5 s sustain threshold");

    /* Brief drop resets streak */
    t += 200;
    CHECK(!sw_detect_should_autostart(&d, false, breath_flow, 5, low_p, 5, t),
          "pressure drop resets streak");
    CHECK(!d.streak_active, "streak cleared on low pressure");

    /* Another 4.8 s still does not trigger */
    feed_packets(&d, &t, 24, 400, breath_flow, &triggered);
    CHECK(!triggered, "new streak must earn full 5 s");
}

static void test_legitimate_mid_therapy_reboot_recovers(void)
{
    sw_detect_t d = {0};
    int16_t breath_flow[5] = {60, 62, 65, 68, 70}; /* span = 10 >= 5 */
    int16_t high_p[5] = {800, 800, 800, 800, 800};
    int64_t t = 0;

    /* First packet starts streak at t = 200; 25 more packets = t = 5200 (elapsed 5000ms) */
    for (int i = 0; i < 25; i++) {
        t += 200;
        CHECK(!sw_detect_should_autostart(&d, false, breath_flow, 5, high_p, 5, t),
              "should wait full 5000 ms before starting");
    }
    t += 200; /* 5200 - 200 == 5000 ms */
    CHECK(sw_detect_should_autostart(&d, false, breath_flow, 5, high_p, 5, t),
          "sustained therapy flow+pressure recovers mid-therapy session");
}

static void test_missed_therapy_start_rearms_on_zle1(void)
{
    sw_detect_t d = {0};
    int16_t breath_flow[5] = {10, 20, 30, 40, 25};
    int64_t t = 0;
    bool triggered = false;

    sw_detect_on_zle(&d, 0, false);
    CHECK(d.therapy_stopped, "disarmed after _ZLE=0");

    /* _ZLE=1 during MaskFit or Cooldown must NOT re-arm */
    sw_detect_on_mask_fit_start(&d);
    sw_detect_on_zle(&d, 1, false);
    CHECK(d.therapy_stopped, "_ZLE=1 suppressed during MaskFit");
    sw_detect_on_mask_fit_stop(&d);

    sw_detect_on_cooldown_start(&d);
    sw_detect_on_zle(&d, 1, false);
    CHECK(d.therapy_stopped, "_ZLE=1 suppressed during Cooldown");
    sw_detect_on_cooldown_stop(&d);

    /* Normal idle _ZLE=1 re-arms and allows sustained start */
    sw_detect_on_zle(&d, 1, false);
    CHECK(!d.therapy_stopped, "idle _ZLE=1 re-arms missed TherapyStart");
    feed_packets(&d, &t, 26, 500, breath_flow, &triggered);
    CHECK(triggered, "sustained packets start session after idle _ZLE=1");
}

static void test_standby_watchdog_aborts_ghost_session(void)
{
    sw_detect_t d = {0};
    int16_t standby_p[5] = {10, 10, 10, 10, 10};
    int64_t t = 10000;

    sw_detect_on_session_open(&d, false, t);
    /* First standby sample marks standby_start_ms = 10200; 149 more packets = 40000 (29.8 s) */
    for (int i = 0; i < 150; i++) {
        t += 200;
        CHECK(sw_detect_check_active_stream(&d, standby_p, 5, t) == SW_WATCHDOG_OK,
              "under 30 s heuristic standby limit remains active");
    }
    t += 200; /* 40200 - 10200 == 30000 ms */
    CHECK(sw_detect_check_active_stream(&d, standby_p, 5, t) == SW_WATCHDOG_ABORT_NO_THERAPY,
          "30 s of flat standby aborts unconfirmed heuristic session");
    CHECK(d.therapy_stopped, "standby timeout disarms further auto-start");
}

static void test_standby_watchdog_keeps_real_therapy_and_resets(void)
{
    sw_detect_t d = {0};
    int16_t real_p[5] = {800, 800, 800, 800, 800};
    int16_t standby_p[5] = {10, 10, 10, 10, 10};
    int64_t t = 0;

    /* Event-started session gets 65 s real therapy, then standby dropout */
    sw_detect_on_session_open(&d, true, t);
    for (int i = 0; i < 325; i++) { /* 65 s */
        t += 200;
        CHECK(sw_detect_check_active_stream(&d, real_p, 5, t) == SW_WATCHDOG_OK,
              "active therapy never trips watchdog");
    }
    CHECK(d.therapy_press_ms >= 60000, "accumulated >= 60 s therapy pressure");

    /* 35 s standby does NOT trip 180 s event-started limit */
    for (int i = 0; i < 175; i++) {
        t += 200;
        CHECK(sw_detect_check_active_stream(&d, standby_p, 5, t) == SW_WATCHDOG_OK,
              "event-started session survives 35 s standby");
    }

    /* Therapy pressure resumes -> resets standby timer */
    t += 200;
    CHECK(sw_detect_check_active_stream(&d, real_p, 5, t) == SW_WATCHDOG_OK,
          "resuming pressure ok");
    CHECK(!d.standby_active, "resuming pressure resets standby timer");

    /* 180 s continuous standby closes with timed_out (not aborted) */
    sw_watchdog_action_t act = SW_WATCHDOG_OK;
    for (int i = 0; i < 902 && act == SW_WATCHDOG_OK; i++) {
        t += 200;
        act = sw_detect_check_active_stream(&d, standby_p, 5, t);
    }
    CHECK(act == SW_WATCHDOG_CLOSE_TIMEOUT,
          "missed TherapyStop closes real session as timed_out after 3 min standby");
}

int main(void)
{
    test_cooldown_reboot_ghost_blocked();
    test_lifecycle_events_disarm_autostart();
    test_flat_standby_flow_rejected();
    test_transient_blip_resets_streak();
    test_legitimate_mid_therapy_reboot_recovers();
    test_missed_therapy_start_rearms_on_zle1();
    test_standby_watchdog_aborts_ghost_session();
    test_standby_watchdog_keeps_real_therapy_and_resets();

    printf("session_detect_test: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
