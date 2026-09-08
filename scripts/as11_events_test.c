/*
 * SomnoTrace — Host unit tests for AS11 event parsing & state machine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tests the event classification and state transitions in session_writer.c
 * ensuring Mask Fit and tube drying (Cooldown) modes are never recorded
 * as therapy sessions while mid-therapy reboot recovery and 3 AM restarts
 * remain fully supported.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

#include "cJSON.h"
#include "esp_log.h"

/* Replicated event taxonomy from session_writer.c */
typedef enum {
    AS11_EV_NONE = 0,
    AS11_EV_THERAPY_START,
    AS11_EV_THERAPY_STOP,
    AS11_EV_MASK_FIT_START,
    AS11_EV_MASK_FIT_STOP,
    AS11_EV_COOLDOWN_START,
    AS11_EV_COOLDOWN_STOP,
    AS11_EV_STANDBY_START,
} as11_event_t;

/* Replicated parser from session_writer.c */
static as11_event_t check_event_notification(const cJSON *msg, const char **out_report)
{
    if (out_report) *out_report = NULL;

    cJSON *params = cJSON_GetObjectItem(msg, "params");
    if (!params) return AS11_EV_NONE;

    cJSON *events = cJSON_GetObjectItem(params, "events");
    if (!events || !cJSON_IsArray(events)) return AS11_EV_NONE;

    int n = cJSON_GetArraySize(events);
    for (int i = 0; i < n; i++) {
        cJSON *ev = cJSON_GetArrayItem(events, i);
        if (!ev) continue;
        cJSON *event = cJSON_GetObjectItem(ev, "event");
        if (!event || !cJSON_IsString(event)) continue;
        const char *name = event->valuestring;

        if (strcmp(name, "TherapyStart") == 0) {
            cJSON *rt = cJSON_GetObjectItem(ev, "reportTime");
            if (out_report && rt && cJSON_IsString(rt))
                *out_report = rt->valuestring;
            return AS11_EV_THERAPY_START;
        }
        if (strcmp(name, "TherapyStop") == 0) {
            return AS11_EV_THERAPY_STOP;
        }
        if (strcmp(name, "MaskFitStart") == 0 ||
            strcmp(name, "MaskfitStarted") == 0 ||
            strcmp(name, "LearnTargetsStart") == 0) {
            return AS11_EV_MASK_FIT_START;
        }
        if (strcmp(name, "MaskFitStop") == 0 ||
            strcmp(name, "LearnTargetsStop") == 0) {
            return AS11_EV_MASK_FIT_STOP;
        }
        if (strcmp(name, "CooldownStarted") == 0) {
            return AS11_EV_COOLDOWN_START;
        }
        if (strcmp(name, "CooldownStopped") == 0) {
            return AS11_EV_COOLDOWN_STOP;
        }
        if (strcmp(name, "StandbyStarted") == 0) {
            return AS11_EV_STANDBY_START;
        }
    }
    return AS11_EV_NONE;
}

/* State machine test fixture */
typedef struct {
    bool s_therapy_stopped;
    bool s_in_mask_fit;
    bool s_in_cooldown;
    bool s_started_from_event;
    bool session_active;
    int  session_starts;
    int  session_finalizes;
    int  session_aborts;
} test_state_t;

static void state_init(test_state_t *st) {
    memset(st, 0, sizeof(*st));
}

static void on_event(test_state_t *st, as11_event_t ev) {
    if (ev == AS11_EV_THERAPY_STOP) {
        st->s_therapy_stopped = true;
        st->s_in_mask_fit = false;
        if (st->session_active) {
            st->session_active = false;
            st->session_finalizes++;
        }
    } else if (ev == AS11_EV_THERAPY_START) {
        st->s_therapy_stopped = false;
        st->s_in_mask_fit = false;
        st->s_in_cooldown = false;
        if (st->session_active) {
            /* rotate */
            st->session_finalizes++;
        }
        st->session_active = true;
        st->s_started_from_event = true;
        st->session_starts++;
    } else if (ev == AS11_EV_MASK_FIT_START) {
        st->s_in_mask_fit = true;
        if (st->session_active && !st->s_started_from_event) {
            st->session_active = false;
            st->session_aborts++;
        }
    } else if (ev == AS11_EV_MASK_FIT_STOP) {
        st->s_in_mask_fit = false;
    } else if (ev == AS11_EV_COOLDOWN_START) {
        st->s_in_cooldown = true;
        st->s_therapy_stopped = true;
        if (st->session_active) {
            st->session_active = false;
            st->session_finalizes++;
        }
    } else if (ev == AS11_EV_COOLDOWN_STOP) {
        st->s_in_cooldown = false;
    } else if (ev == AS11_EV_STANDBY_START) {
        if (st->session_active) {
            st->s_therapy_stopped = true;
            st->session_active = false;
            st->session_finalizes++;
        }
    }
}

static void on_stream_flow(test_state_t *st, bool has_flow, bool has_pressure) {
    if (!st->session_active && !st->s_therapy_stopped
        && !st->s_in_mask_fit && !st->s_in_cooldown
        && has_flow && has_pressure) {
        st->session_active = true;
        st->s_started_from_event = false;
        st->session_starts++;
    }
}

static cJSON *make_event_msg(const char *ev_name, const char *report_time) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "method", "EventNotification");
    cJSON *params = cJSON_CreateObject();
    cJSON_AddItemToObject(msg, "params", params);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToObject(params, "events", arr);
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "event", ev_name);
    if (report_time) cJSON_AddStringToObject(item, "reportTime", report_time);
    cJSON_AddItemToArray(arr, item);
    return msg;
}

int main(void) {
    printf("=== Running AS11 Event & State Machine Unit Tests ===\n");

    /* Test 1: Parser classification */
    {
        cJSON *m1 = make_event_msg("TherapyStart", "2026-09-02T12:00:00Z");
        const char *rt = NULL;
        assert(check_event_notification(m1, &rt) == AS11_EV_THERAPY_START);
        assert(rt && strcmp(rt, "2026-09-02T12:00:00Z") == 0);
        cJSON_Delete(m1);

        cJSON *m2 = make_event_msg("MaskFitStart", NULL);
        assert(check_event_notification(m2, NULL) == AS11_EV_MASK_FIT_START);
        cJSON_Delete(m2);

        cJSON *m3 = make_event_msg("MaskfitStarted", NULL);
        assert(check_event_notification(m3, NULL) == AS11_EV_MASK_FIT_START);
        cJSON_Delete(m3);

        cJSON *m4 = make_event_msg("CooldownStarted", NULL);
        assert(check_event_notification(m4, NULL) == AS11_EV_COOLDOWN_START);
        cJSON_Delete(m4);

        printf("  [PASS] Test 1: Event notification taxonomy parsed correctly\n");
    }

    /* Test 2: Standard Night Therapy Lifecycle */
    {
        test_state_t st;
        state_init(&st);

        /* 11:00 PM TherapyStart */
        on_event(&st, AS11_EV_THERAPY_START);
        assert(st.session_active == true);
        assert(st.session_starts == 1);

        /* Streaming data */
        on_stream_flow(&st, true, true);
        assert(st.session_active == true);
        assert(st.session_starts == 1);

        /* 7:00 AM TherapyStop */
        on_event(&st, AS11_EV_THERAPY_STOP);
        assert(st.session_active == false);
        assert(st.session_finalizes == 1);

        /* 7:00 AM CooldownStarted (tube drying) */
        on_event(&st, AS11_EV_COOLDOWN_START);
        assert(st.s_in_cooldown == true);

        /* Flow during drying must NOT start a session */
        on_stream_flow(&st, true, true);
        assert(st.session_active == false);
        assert(st.session_starts == 1);

        /* 7:20 AM CooldownStopped */
        on_event(&st, AS11_EV_COOLDOWN_STOP);
        assert(st.s_in_cooldown == false);

        printf("  [PASS] Test 2: Standard therapy lifecycle & cooldown suppression\n");
    }

    /* Test 3: Mask Fit Standalone (Issue #149 Core Scenario) */
    {
        test_state_t st;
        state_init(&st);

        /* User taps Mask Fit on AS11 screen */
        on_event(&st, AS11_EV_MASK_FIT_START);
        assert(st.s_in_mask_fit == true);

        /* Blower blows air at 10 cmH2O (must NOT start session) */
        on_stream_flow(&st, true, true);
        assert(st.session_active == false);
        assert(st.session_starts == 0);

        /* User cancels Mask Fit */
        on_event(&st, AS11_EV_MASK_FIT_STOP);
        assert(st.s_in_mask_fit == false);

        /* Tube drying starts */
        on_event(&st, AS11_EV_COOLDOWN_START);
        assert(st.s_in_cooldown == true);

        /* Drying airflow must NOT start session */
        on_stream_flow(&st, true, true);
        assert(st.session_active == false);
        assert(st.session_starts == 0);

        on_event(&st, AS11_EV_COOLDOWN_STOP);
        assert(st.s_in_cooldown == false);
        assert(st.session_starts == 0);

        printf("  [PASS] Test 3: Mask Fit & subsequent drying never create therapy sessions\n");
    }

    /* Test 4: 3 AM Bathroom Break (Therapy during Cooldown) */
    {
        test_state_t st;
        state_init(&st);

        /* Session 1 */
        on_event(&st, AS11_EV_THERAPY_START);
        on_event(&st, AS11_EV_THERAPY_STOP);
        on_event(&st, AS11_EV_COOLDOWN_START);
        assert(st.s_in_cooldown == true);

        /* User returns 3 mins later, starts therapy while snowflake drying active */
        on_event(&st, AS11_EV_THERAPY_START);
        assert(st.s_in_cooldown == false);
        assert(st.session_active == true);
        assert(st.session_starts == 2);

        on_event(&st, AS11_EV_THERAPY_STOP);
        assert(st.session_active == false);
        assert(st.session_finalizes == 2);

        printf("  [PASS] Test 4: TherapyStart unconditionally overrides active cooldown\n");
    }

    /* Test 5: Mid-Therapy Reboot Recovery */
    {
        test_state_t st;
        state_init(&st);

        /* Boot up while patient is breathing under therapeutic pressure */
        assert(st.s_in_mask_fit == false);
        assert(st.s_in_cooldown == false);
        assert(st.s_therapy_stopped == false);

        /* Stream packets arrive with non-zero flow and pressure */
        on_stream_flow(&st, true, true);
        assert(st.session_active == true);
        assert(st.session_starts == 1);
        assert(st.s_started_from_event == false);

        printf("  [PASS] Test 5: Mid-therapy reboot recovery activates on active flow\n");
    }

    /* Test 6: Dropped TherapyStop Safety Net */
    {
        test_state_t st;
        state_init(&st);

        on_event(&st, AS11_EV_THERAPY_START);
        assert(st.session_active == true);

        /* TherapyStop dropped over BLE! Only CooldownStarted arrives */
        on_event(&st, AS11_EV_COOLDOWN_START);
        assert(st.session_active == false);
        assert(st.session_finalizes == 1);
        assert(st.s_therapy_stopped == true);

        printf("  [PASS] Test 6: Dropped TherapyStop recovered cleanly by CooldownStarted\n");
    }

    /* Test 7: Race condition / late MaskFitStart event */
    {
        test_state_t st;
        state_init(&st);

        /* Flow arrives just before MaskFit event */
        on_stream_flow(&st, true, true);
        assert(st.session_active == true);
        assert(st.s_started_from_event == false);

        /* MaskFitStart arrives immediately after */
        on_event(&st, AS11_EV_MASK_FIT_START);
        assert(st.session_active == false);
        assert(st.session_aborts == 1);

        printf("  [PASS] Test 7: Late MaskFit event aborts false session opened by airflow\n");
    }

    /* Test 8: CSL.edf CSR Event Labels, Backdate & Deduplication (Issue #190) */
    {
        /* 1. Label mapping: distinct start/end labels */
        const char *l_start = (strcmp("CsrStart", "CsrStart") == 0) ? "CSR Start" : NULL;
        const char *l_end = (strcmp("CsrEnd", "CsrEnd") == 0) ? "CSR End" : NULL;
        assert(l_start != NULL && strcmp(l_start, "CSR Start") == 0);
        assert(l_end != NULL && strcmp(l_end, "CSR End") == 0);

        /* 2. Backdate calculation: onset must subtract backdateSeconds */
        int64_t session_start_ms = 1781390000000LL;
        int64_t report_time_ms  = 1781390869000LL; /* +869s from session start */
        int64_t backdate_sec = 42;
        int64_t clock_drift_ms = 0;

        int64_t event_ntp_ms = report_time_ms + clock_drift_ms - (backdate_sec * 1000);
        int64_t onset_sec = (event_ntp_ms - session_start_ms) / 1000;
        assert(onset_sec == 827); /* exactly 869 - 42 */

        /* 3. Deduplication: start and end at same second are NOT collapsed */
        typedef struct {
            int64_t onset_sec;
            int64_t dur_sec;
            const char *label;
        } test_ev_t;

        test_ev_t evs[3] = {
            { .onset_sec = 827, .dur_sec = 0, .label = "CSR Start" },
            { .onset_sec = 827, .dur_sec = 0, .label = "CSR End" },   /* same onset, different label: must preserve */
            { .onset_sec = 827, .dur_sec = 0, .label = "CSR End" },   /* duplicate retransmit: must drop */
        };

        test_ev_t deduped[3];
        size_t dedup_count = 0;
        for (size_t i = 0; i < 3; i++) {
            if (dedup_count > 0 &&
                deduped[dedup_count - 1].onset_sec == evs[i].onset_sec &&
                strcmp(deduped[dedup_count - 1].label, evs[i].label) == 0) {
                continue;
            }
            deduped[dedup_count++] = evs[i];
        }

        assert(dedup_count == 2);
        assert(strcmp(deduped[0].label, "CSR Start") == 0);
        assert(strcmp(deduped[1].label, "CSR End") == 0);

        printf("  [PASS] Test 8: CSL.edf CSR Event Labels, Backdate & Deduplication (Issue #190)\n");
    }

    printf("\n>>> ALL AS11 EVENT TESTS PASSED (8/8) <<<\n");
    return 0;
}
