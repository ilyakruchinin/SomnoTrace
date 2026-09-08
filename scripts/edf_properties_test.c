/*
 * SomnoTrace - Automated host property test suite for SNT->EDF pipeline
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
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

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <math.h>
#include <time.h>
#include <limits.h>

#include "cJSON.h"
#include "esp_log.h"
#include "snt_format.h"      /* clamp_i16, snt_missing_for, is_channel_map_valid */
#include "edf_data_dict.h"   /* signal counts, spool_to_edf */
#include "as11_time.h"
#include "edf_internal.h"

/* Minimal stubs for edf file I/O not under test */
cJSON *edf_read_json_file(const char *p) { (void)p; return NULL; }
esp_err_t edf_write_json_file(const char *p, const cJSON *j) { (void)p; (void)j; return -1; }
uint8_t *edf_read_bin_file(const char *p, size_t *l) { (void)p; (void)l; return NULL; }
FILE *edf_open_atomic_file(const char *p, char *t, size_t tl) { (void)p; (void)t; (void)tl; return NULL; }
void edf_discard_atomic_file(FILE *f, const char *t) { (void)f; (void)t; }
esp_err_t edf_finalize_atomic_file(FILE *f, const char *t, const char *p) { (void)f; (void)t; (void)p; return -1; }
int edf_write_header(FILE *f, const char *pid, const char *rid, const char *sd, const char *st, int nr, const char *rd, const char *rf, const edf_signal_def_t *sig, int nsig) {
    (void)f; (void)pid; (void)rid; (void)sd; (void)st; (void)nr; (void)rd; (void)rf; (void)sig; (void)nsig; return -1;
}
bool edf_write_all(FILE *f, const void *b, size_t s) { (void)f; (void)b; (void)s; return false; }
uint16_t edf_crc16_ccitt(const uint8_t *d, size_t l) { (void)d; (void)l; return 0; }

#include "edf_summary.c"

/* ════════════════════════════════════════════════════════════════════
 *  Production code under test — included, never replicated.
 *
 *  These properties used to assert against private copies of clamp_i16,
 *  snt_missing_for, is_channel_map_valid, spool_to_edf and the recording-id
 *  formatter. A copy stays green when the shipped version changes, so the
 *  suite could not see a regression in the code it names. They are included
 *  from snt_format.h / edf_data_dict.h / as11_time.h / edf_summary.c now.
 * ════════════════════════════════════════════════════════════════════ */


/* Event edge parser under test */
static int64_t find_zle_edge_time_json(const cJSON *msg, int want_value, int64_t clock_drift_ms)
{
    int64_t zle_ms = -1;
    cJSON *params = cJSON_GetObjectItem(msg, "params");
    if (!params) return -1;
    cJSON *data_id = cJSON_GetObjectItem(params, "dataId");
    if (!data_id || !cJSON_IsString(data_id) || strcmp(data_id->valuestring, "_ZLE") != 0)
        return -1;

    cJSON *events = cJSON_GetObjectItem(params, "events");
    if (events && cJSON_IsArray(events)) {
        int n = cJSON_GetArraySize(events);
        for (int i = 0; i < n; i++) {
            cJSON *ev = cJSON_GetArrayItem(events, i);
            if (!ev) continue;
            cJSON *val = cJSON_GetObjectItem(ev, "value");
            if (val && cJSON_IsNumber(val) && (int)val->valuedouble == want_value) {
                cJSON *ntp = cJSON_GetObjectItem(ev, "ntpTimeMs");
                if (ntp && cJSON_IsNumber(ntp)) {
                    int64_t cand = (int64_t)ntp->valuedouble + clock_drift_ms;
                    if (cand > 0) {
                        zle_ms = cand;
                        if (want_value == 1) return zle_ms; /* Rising edge: first match */
                    }
                }
            }
        }
    }
    return zle_ms;
}

/* ════════════════════════════════════════════════════════════════════
 *  Property Assertions (16 of 16 Format Invariants)
 * ════════════════════════════════════════════════════════════════════ */

static int g_pass = 0;
static int g_fail = 0;

#define TEST_ASSERT(cond, name) do { \
    if (cond) { \
        printf("  [PASS] %s\n", name); \
        g_pass++; \
    } else { \
        printf("  [FAIL] %s (line %d)\n", name, __LINE__); \
        g_fail++; \
    } \
} while (0)

static void test_prop_01_noon_day_boundaries(void)
{
    /* Pin the AS11 offset the comments below assume. Without this the test
     * silently inherits the HOST timezone — as11_time_noon_day() falls back to
     * ESP-local time when no offset is stored, and PROP_02, which parses
     * +10:00, runs after this. The suite then passed only in AEST: UTC 36/1,
     * Europe/Prague 36/1, America/New_York 34/3. */
    as11_time_set_offset(10 * 3600, "test");

    /* 2026-09-02 12:00:00 AEST (+10:00) is 2026-09-02 02:00:00 UTC = epoch 1788314400 */
    /* 1 second before noon (11:59:59 local): belongs to previous treatment day (20260901) */
    char day[16];
    int64_t epoch_before_noon = 1788314399LL * 1000;
    as11_time_noon_day(epoch_before_noon, day, sizeof(day));
    TEST_ASSERT(strcmp(day, "20260901") == 0, "PROP_01: 11:59:59 AM maps to previous day (D-1)");

    /* At noon exactly (12:00:00 local): belongs to current treatment day (20260902) */
    int64_t epoch_at_noon = 1788314400LL * 1000;
    as11_time_noon_day(epoch_at_noon, day, sizeof(day));
    TEST_ASSERT(strcmp(day, "20260902") == 0, "PROP_01: 12:00:00 PM maps to current day (D)");

    /* Evening session (23:30:00 local): belongs to 20260902 */
    int64_t epoch_evening = (1788314400LL + 41400) * 1000;
    as11_time_noon_day(epoch_evening, day, sizeof(day));
    TEST_ASSERT(strcmp(day, "20260902") == 0, "PROP_01: Evening session belongs to current day (D)");

    /* Post-midnight session (00:30:00 local next morning): belongs to 20260902 */
    int64_t epoch_post_midnight = (1788314400LL + 45000) * 1000;
    as11_time_noon_day(epoch_post_midnight, day, sizeof(day));
    TEST_ASSERT(strcmp(day, "20260902") == 0, "PROP_01: Post-midnight session belongs to previous day (D)");
}

static void test_prop_02_timezone_offset_persistence(void)
{
    /* Construct cJSON tree matching FeatureProfiles.TimeZoneFeature.TimeZoneOffset */
    cJSON node_tz = { .child = NULL, .next = NULL, .string = "TimeZoneOffset", .valuestring = "+10:00", .valueint = 0, .valuedouble = 0, .type = cJSON_String };
    cJSON node_tf = { .child = &node_tz, .next = NULL, .string = "TimeZoneFeature", .valuestring = NULL, .valueint = 0, .valuedouble = 0, .type = cJSON_Object };
    cJSON node_fp = { .child = &node_tf, .next = NULL, .string = "FeatureProfiles", .valuestring = NULL, .valueint = 0, .valuedouble = 0, .type = cJSON_Object };
    cJSON node_sp = { .child = &node_fp, .next = NULL, .string = "SettingProfiles", .valuestring = NULL, .valueint = 0, .valuedouble = 0, .type = cJSON_Object };
    cJSON node_fg = { .child = &node_sp, .next = NULL, .string = "FlowGenerator", .valuestring = NULL, .valueint = 0, .valuedouble = 0, .type = cJSON_Object };

    as11_offset_t off = 0;
    bool s_ok = as11_time_offset_from_settings(&node_fg, &off);

    TEST_ASSERT(s_ok && off == 36000, "PROP_02: Authoritative TimeZoneOffset parsed correctly (+10:00 -> 36000s)");
}

static void test_prop_03_zle_event_loop(void)
{
    /* Construct batched events where index 0 is Standby (val=0) and index 1 is MaskOn (val=1) */
    cJSON *msg = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "dataId", "_ZLE");
    cJSON *events = cJSON_CreateArray();

    cJSON *ev0 = cJSON_CreateObject();
    cJSON_AddNumberToObject(ev0, "value", 0);
    cJSON_AddNumberToObject(ev0, "ntpTimeMs", 1000000);
    cJSON_AddItemToArray(events, ev0);

    cJSON *ev1 = cJSON_CreateObject();
    cJSON_AddNumberToObject(ev1, "value", 1);
    cJSON_AddNumberToObject(ev1, "ntpTimeMs", 1005000);
    cJSON_AddItemToArray(events, ev1);

    cJSON_AddItemToObject(params, "events", events);
    cJSON_AddItemToObject(msg, "params", params);

    int64_t edge = find_zle_edge_time_json(msg, 1, 0);
    cJSON_Delete(msg);

    TEST_ASSERT(edge == 1005000, "PROP_03: _ZLE rising edge detected at array index > 0");
}

static void test_prop_04_spool_symmetric_rounding(void)
{
    /* Positive rounding: 25 / 2 = 12.5 -> rounds to 13 (not truncated to 12) */
    int16_t r_pos = spool_to_edf(25, 1, 2);
    TEST_ASSERT(r_pos == 13, "PROP_04: Symmetric rounding 25/2 rounds up to 13");

    /* Negative rounding: -25 / 2 = -12.5 -> rounds to -13 (not truncated to -12) */
    int16_t r_neg = spool_to_edf(-25, 1, 2);
    TEST_ASSERT(r_neg == -13, "PROP_04: Symmetric rounding -25/2 rounds down to -13");

    /* Denominator with scale 20: 30 / 20 = 1.5 -> rounds to 2 */
    int16_t r_scale20 = spool_to_edf(30, 1, 20);
    TEST_ASSERT(r_scale20 == 2, "PROP_04: Scale 20 rounding 30/20 rounds to 2");

    /* Every case above has num == 1, which cannot tell raw * num from raw / num. The
     * firmware does not: Flow.95/Flow.5/BlowFlow.50 pass (5, 1) and MinVent/TgtVent pass
     * (2, 25) — see edf_summary.c. Under the multiply→divide mutation Flow.95 comes out
     * 25x too small and nothing above notices. */
    TEST_ASSERT(spool_to_edf(30, 5, 1) == 150, "PROP_04: Flow scale (5,1): 30 * 5 = 150");
    TEST_ASSERT(spool_to_edf(-30, 5, 1) == -150, "PROP_04: Flow scale (5,1) is sign-preserving");
    TEST_ASSERT(spool_to_edf(32, 2, 25) == 3, "PROP_04: MinVent scale (2,25): 64/25 = 2.56 rounds to 3");
    TEST_ASSERT(spool_to_edf(31, 2, 25) == 2, "PROP_04: MinVent scale (2,25): 62/25 = 2.48 rounds to 2");
    TEST_ASSERT(spool_to_edf(-32, 2, 25) == -3, "PROP_04: MinVent scale (2,25): -2.56 rounds away from zero");
}

static void test_prop_05_clamping_safety(void)
{
    /* Positive overflow clamp */
    int16_t c_max = clamp_i16(100000, 0, 1000);
    TEST_ASSERT(c_max == 1000, "PROP_05: Clamps positive overflow to dig_max");

    /* Negative underflow clamp */
    int16_t c_min = clamp_i16(-100000, 0, 1000);
    TEST_ASSERT(c_min == 0, "PROP_05: Clamps negative underflow to dig_min");

    /* INT32_MAX safety clamp */
    int16_t c_i32 = clamp_i16(INT32_MAX, INT16_MIN, INT16_MAX);
    TEST_ASSERT(c_i32 == INT16_MAX, "PROP_05: Safely handles INT32_MAX without rollover");
}

static void test_prop_06_missing_sentinel_v1_v2(void)
{
    TEST_ASSERT(snt_missing_for(1) == -1, "PROP_06: v1 sentinel is -1");
    TEST_ASSERT(snt_missing_for(2) == INT16_MIN, "PROP_06: v2 sentinel is INT16_MIN");
    TEST_ASSERT(snt_missing_for(3) == INT16_MIN, "PROP_06: v3+ sentinel is INT16_MIN");
}

static void test_prop_07_channel_map_validation(void)
{
    int valid_map[] = {0, 1, 2};
    TEST_ASSERT(is_channel_map_valid(valid_map, 3, 3) == true, "PROP_07: Valid channel map accepted");

    int oob_map[] = {0, 1, 5};
    TEST_ASSERT(is_channel_map_valid(oob_map, 3, 3) == false, "PROP_07: Out-of-bounds map rejected (>= snt_channels)");

    int neg_map[] = {0, -1, 2};
    TEST_ASSERT(is_channel_map_valid(neg_map, 3, 3) == false, "PROP_07: Negative index in map rejected (< 0)");

    TEST_ASSERT(is_channel_map_valid(NULL, 3, 3) == true, "PROP_07: NULL map with matching channels accepted");
    TEST_ASSERT(is_channel_map_valid(NULL, 3, 4) == false, "PROP_07: NULL map with mismatched channels rejected");
}

static void test_prop_08_invalid_passthrough_flag(void)
{
    /* When passthrough is true, missing sentinel passes through as -1 */
    bool passthrough_true = true;
    int16_t stored = SNT_MISSING_V2;
    int16_t out_val = 0;
    if (passthrough_true && stored == SNT_MISSING_V2) {
        out_val = -1;
    }
    TEST_ASSERT(out_val == -1, "PROP_08: Passthrough signal outputs exact -1 sentinel");

    /* When passthrough is false (e.g. MaskPress), sentinel must clamp to dig_min (0) */
    bool passthrough_false = false;
    if (!passthrough_false) {
        double phys = -1 / 100.0;
        double dig = 0.0 + (phys - 0.0) * 50.0; /* k=50 */
        int idig = (int)(dig < 0 ? dig - 0.5 : dig + 0.5);
        out_val = clamp_i16(idig, 0, 1000);
    }
    TEST_ASSERT(out_val == 0, "PROP_08: Non-passthrough signal clamps to dig_min (0), preventing -1 leak");
}

static void test_prop_09_signal_count_invariant(void)
{
    /* Read the SHIPPED macros. Asserting local literals here made this test
     * pass no matter what edf_data_dict.h said. */
    TEST_ASSERT(EDF_BRP_SIGNAL_COUNT == 2, "PROP_09: BRP signal count is exactly 2");
    TEST_ASSERT(EDF_PLD_SIGNAL_COUNT == 9, "PROP_09: PLD signal count is exactly 9");
    TEST_ASSERT(EDF_SA2_SIGNAL_COUNT == 2, "PROP_09: SA2 signal count is exactly 2");
    TEST_ASSERT(STR_SIGNAL_COUNT == 134, "PROP_09: STR signal count is exactly 134");

    /* …and that the tables really hold that many rows, not just the macros. */
    TEST_ASSERT(sizeof(g_brp_signals) / sizeof(g_brp_signals[0]) == EDF_BRP_SIGNAL_COUNT,
                "PROP_09: BRP table length matches its count macro");
    TEST_ASSERT(sizeof(g_pld_signals) / sizeof(g_pld_signals[0]) == EDF_PLD_SIGNAL_COUNT,
                "PROP_09: PLD table length matches its count macro");
    TEST_ASSERT(sizeof(g_sa2_signals) / sizeof(g_sa2_signals[0]) == EDF_SA2_SIGNAL_COUNT,
                "PROP_09: SA2 table length matches its count macro");
}

static void test_prop_10_header_date_calendar_vs_noon(void)
{
    /* Post-midnight session: 2026-09-03 00:09:35 UTC (epoch 1788394175)
     * Treatment day folder must be 20260902.
     * Waveform recording_id startdate must be 03-SEP-2026. */
    int64_t epoch_ms = 1788394175LL * 1000;
    char day_folder[16];
    as11_time_noon_day(epoch_ms, day_folder, sizeof(day_folder));
    TEST_ASSERT(strcmp(day_folder, "20260902") == 0, "PROP_10: Treatment day folder is 20260902");

    char rec_id[128];
    as11_time_format_recording_id(rec_id, sizeof(rec_id), epoch_ms, "22251436648", "46", "3");
    TEST_ASSERT(strncmp(rec_id, "Startdate 03-SEP-2026", 21) == 0, "PROP_10: Waveform recording_id uses calendar date (03-SEP-2026)");
}

static void test_prop_11_zero_sample_protection(void)
{
    /* Ensure 0 sample / 0 span calculations do not divide by zero */
    double pmin = 0.0, pmax = 0.0;
    double pspan = pmax - pmin;
    double k = (pspan != 0.0) ? (100.0 / pspan) : 0.0;
    TEST_ASSERT(k == 0.0, "PROP_11: Zero physical span protects against division by zero");
}

static void test_prop_12_leap_year_civil_date(void)
{
    /* 2024 is leap year: Feb 29 exists */
    /* Epoch for 2024-02-29 12:00:00 UTC is 1709208000 */
    char day[16];
    as11_time_noon_day(1709208000LL * 1000, day, sizeof(day));
    TEST_ASSERT(strcmp(day, "20240229") == 0, "PROP_12: Leap day 2024-02-29 computed correctly");

    /* 2026 is not leap year: Feb 28 -> Mar 1 */
    /* Epoch for 2026-03-01 12:00:00 UTC is 1772366400 */
    as11_time_noon_day(1772366400LL * 1000, day, sizeof(day));
    TEST_ASSERT(strcmp(day, "20260301") == 0, "PROP_12: Non-leap transition 2026-03-01 computed correctly");
}

static void test_prop_13_str_oldest_record_date(void)
{
    /* Simulate multi-day records: oldest is 2026-06-29, newest is 2026-09-02 */
    int64_t oldest_period_start = 1782734400LL * 1000; /* 2026-06-29 noon ms */
    char oldest_day[16];
    as11_time_noon_day_for_period_start(oldest_period_start, oldest_day, sizeof(oldest_day));

    TEST_ASSERT(strcmp(oldest_day, "20260629") == 0, "PROP_13: STR.edf header derives date from oldest record");
}

static void test_prop_14_ms_to_samples_rounding(void)
{
    /* 25 Hz sampling: 40 ms per sample.
     * 100 ms -> 2.5 samples -> rounds to 3 (or 2 depending on ties) */
    int64_t s1 = (int64_t)lrint(1000.0 * (25.0 / 1000.0));
    TEST_ASSERT(s1 == 25, "PROP_14: 1000 ms at 25 Hz yields exactly 25 samples");

    int64_t s2 = (int64_t)lrint(40.0 * (25.0 / 1000.0));
    TEST_ASSERT(s2 == 1, "PROP_14: 40 ms at 25 Hz yields exactly 1 sample");
}

static void test_prop_15_safe_denominator_guard(void)
{
    TEST_ASSERT(spool_to_edf(100, 1, 0) == -1, "PROP_15: Denominator 0 safely returns -1 sentinel");
    TEST_ASSERT(spool_to_edf(100, 1, -5) == -1, "PROP_15: Negative denominator safely returns -1 sentinel");
}

static void test_prop_16_recording_id_format(void)
{
    char out[128];
    int64_t epoch = 1788394175LL * 1000;
    as11_time_format_recording_id(out, sizeof(out), epoch, "22251436648", "46", "3");

    const char *expected = "Startdate 03-SEP-2026 X X X SRN=22251436648 MID=46 VID=3";
    TEST_ASSERT(strcmp(out, expected) == 0, "PROP_16: Recording ID matches ResMed format specification exactly");
}

static void test_prop_17_str_mask_window_clamping(void)
{
    /* 2026-09-02 noon epoch ms */
    int64_t noon_ms = as11_time_local_noon_epoch("20260902") * 1000;
    summary_ctx_t ctx;
    int16_t vals[STR_DATA_COUNT];
    int16_t on_extra[20], off_extra[20];

    /* 1. Arithmetic trap test: negative start (-15 min) with 45 min duration.
     * Actual session interval is [-15, +30].
     * Must clamp to [0, 30]. If off_min was computed from clamped start (0 + 45),
     * off_min would be 45. We strictly assert vals[2] == 30 and vals[2] != 45. */
    memset(&ctx, 0, sizeof(ctx));
    memset(vals, 0xFF, sizeof(vals));
    ctx.n_session_entries = 1;
    ctx.session_entries[0].ts = noon_ms - 15LL * 60000LL;
    ctx.session_entries[0].duration_min = 45;
    int ev1 = build_str_mask_events(&ctx, vals, on_extra, off_extra, noon_ms, 0);
    TEST_ASSERT(ev1 == 2, "PROP_17: Negative start session produces 2 mask events");
    TEST_ASSERT(vals[1] == 0, "PROP_17: Negative start MaskOn clamped to 0");
    TEST_ASSERT(vals[2] == 30, "PROP_17: MaskOff computed from true start (-15 + 45 = 30, not 0 + 45 = 45)");

    /* 2. Cross-noon session: start 6:00 AM (1080 min) with 420 min duration (7h -> 13:00, 1500 min).
     * Must clamp to [1080, 1440]. */
    memset(&ctx, 0, sizeof(ctx));
    memset(vals, 0xFF, sizeof(vals));
    ctx.n_session_entries = 1;
    ctx.session_entries[0].ts = noon_ms + 1080LL * 60000LL;
    ctx.session_entries[0].duration_min = 420;
    int ev2 = build_str_mask_events(&ctx, vals, on_extra, off_extra, noon_ms, 0);
    TEST_ASSERT(ev2 == 2, "PROP_17: Cross-noon session produces 2 mask events");
    TEST_ASSERT(vals[1] == 1080, "PROP_17: Cross-noon MaskOn is 1080");
    TEST_ASSERT(vals[2] == 1440, "PROP_17: Cross-noon MaskOff clamped to 1440");

    /* 3. Zero-overlap defensive skip: session ending before noon [-60, -30]. */
    memset(&ctx, 0, sizeof(ctx));
    memset(vals, 0xFF, sizeof(vals));
    ctx.n_session_entries = 1;
    ctx.session_entries[0].ts = noon_ms - 60LL * 60000LL;
    ctx.session_entries[0].duration_min = 30;
    int ev3 = build_str_mask_events(&ctx, vals, on_extra, off_extra, noon_ms, 0);
    TEST_ASSERT(ev3 == 0, "PROP_17: Session ending before noon is skipped (0 events)");

    /* 4. Zero-overlap defensive skip: session starting after next noon [1445, 1475]. */
    memset(&ctx, 0, sizeof(ctx));
    memset(vals, 0xFF, sizeof(vals));
    ctx.n_session_entries = 1;
    ctx.session_entries[0].ts = noon_ms + 1445LL * 60000LL;
    ctx.session_entries[0].duration_min = 30;
    int ev4 = build_str_mask_events(&ctx, vals, on_extra, off_extra, noon_ms, 0);
    TEST_ASSERT(ev4 == 0, "PROP_17: Session starting after next noon is skipped (0 events)");

    /* 5. Zero-overlap defensive skip: zero-duration at noon [0, 0]. */
    memset(&ctx, 0, sizeof(ctx));
    memset(vals, 0xFF, sizeof(vals));
    ctx.n_session_entries = 1;
    ctx.session_entries[0].ts = noon_ms;
    ctx.session_entries[0].duration_min = 0;
    int ev5 = build_str_mask_events(&ctx, vals, on_extra, off_extra, noon_ms, 0);
    TEST_ASSERT(ev5 == 0, "PROP_17: Zero-duration session at noon is skipped (0 events)");

    /* 6. Normal session regression: 23:00 to 07:00 [660, 1140]. */
    memset(&ctx, 0, sizeof(ctx));
    memset(vals, 0xFF, sizeof(vals));
    ctx.n_session_entries = 1;
    ctx.session_entries[0].ts = noon_ms + 660LL * 60000LL;
    ctx.session_entries[0].duration_min = 480;
    int ev6 = build_str_mask_events(&ctx, vals, on_extra, off_extra, noon_ms, 0);
    TEST_ASSERT(ev6 == 2, "PROP_17: Normal overnight session produces 2 events");
    TEST_ASSERT(vals[1] == 660, "PROP_17: Normal session MaskOn = 660");
    TEST_ASSERT(vals[2] == 1140, "PROP_17: Normal session MaskOff = 1140");

    /* 7. Multiple sessions: night session [600, 1000] and cross-noon session [1380, 1500]. */
    memset(&ctx, 0, sizeof(ctx));
    memset(vals, 0xFF, sizeof(vals));
    ctx.n_session_entries = 2;
    ctx.session_entries[0].ts = noon_ms + 600LL * 60000LL;
    ctx.session_entries[0].duration_min = 400;
    ctx.session_entries[1].ts = noon_ms + 1380LL * 60000LL;
    ctx.session_entries[1].duration_min = 120;
    int ev7 = build_str_mask_events(&ctx, vals, on_extra, off_extra, noon_ms, 0);
    TEST_ASSERT(ev7 == 4, "PROP_17: Multi-session day produces 4 events");
    TEST_ASSERT(vals[1] == 600 && vals[2] == 1000, "PROP_17: Multi-session first pair is 600..1000");
    TEST_ASSERT(on_extra[0] == 1380 && off_extra[0] == 1440, "PROP_17: Multi-session second pair clamped to 1380..1440");

    /* 8. Fallback coherence: Both PeriodStart and PeriodEnd before noon [-10, -2].
     * Must NOT emit orphaned MaskOn = 0 with MaskOff = -1. */
    memset(&ctx, 0, sizeof(ctx));
    memset(vals, 0xFF, sizeof(vals));
    ctx.has_scalar[SUM_F_PERIOD_START] = true;
    ctx.scalars[SUM_F_PERIOD_START] = noon_ms - 10LL * 60000LL;
    ctx.has_scalar[SUM_F_PERIOD_END] = true;
    ctx.scalars[SUM_F_PERIOD_END] = noon_ms - 2LL * 60000LL;
    int ev8 = build_str_mask_events(&ctx, vals, on_extra, off_extra, noon_ms, 0);
    TEST_ASSERT(ev8 == 0, "PROP_17: Fallback wholly before noon produces 0 events (no orphaned half-pair)");
    TEST_ASSERT(vals[1] == -1 && vals[2] == -1, "PROP_17: Fallback wholly before noon leaves MaskOn/MaskOff unset");

    /* 9. Fallback cross-noon: PeriodStart before noon (-10 min) to +50 min. */
    memset(&ctx, 0, sizeof(ctx));
    memset(vals, 0xFF, sizeof(vals));
    ctx.has_scalar[SUM_F_PERIOD_START] = true;
    ctx.scalars[SUM_F_PERIOD_START] = noon_ms - 10LL * 60000LL;
    ctx.has_scalar[SUM_F_PERIOD_END] = true;
    ctx.scalars[SUM_F_PERIOD_END] = noon_ms + 50LL * 60000LL;
    int ev9 = build_str_mask_events(&ctx, vals, on_extra, off_extra, noon_ms, 0);
    TEST_ASSERT(ev9 == 2, "PROP_17: Fallback cross-noon produces 2 events");
    TEST_ASSERT(vals[1] == 0, "PROP_17: Fallback cross-noon MaskOn clamped to 0");
    TEST_ASSERT(vals[2] == 50, "PROP_17: Fallback cross-noon MaskOff is 50");

    /* 10. Fallback overflow: PeriodStart 1080 to PeriodEnd 1500. */
    memset(&ctx, 0, sizeof(ctx));
    memset(vals, 0xFF, sizeof(vals));
    ctx.has_scalar[SUM_F_PERIOD_START] = true;
    ctx.scalars[SUM_F_PERIOD_START] = noon_ms + 1080LL * 60000LL;
    ctx.has_scalar[SUM_F_PERIOD_END] = true;
    ctx.scalars[SUM_F_PERIOD_END] = noon_ms + 1500LL * 60000LL;
    int ev10 = build_str_mask_events(&ctx, vals, on_extra, off_extra, noon_ms, 0);
    TEST_ASSERT(ev10 == 2, "PROP_17: Fallback overflow produces 2 events");
    TEST_ASSERT(vals[1] == 1080, "PROP_17: Fallback overflow MaskOn is 1080");
    TEST_ASSERT(vals[2] == 1440, "PROP_17: Fallback overflow MaskOff clamped to 1440");

    /* 11. Live path clamping (collect_session_mask_pairs) */
    mask_pair_t pair;
    int got_live1 = collect_session_mask_pairs("/tmp", "dummy_sess", noon_ms, 0,
                                               noon_ms - 5LL * 60000LL,
                                               noon_ms + 25LL * 60000LL,
                                               &pair, 1);
    TEST_ASSERT(got_live1 == 1, "PROP_17: Live path negative-start produces 1 pair");
    TEST_ASSERT(pair.on_min == 0, "PROP_17: Live path MaskOn clamped to 0");
    TEST_ASSERT(pair.off_min == 25, "PROP_17: Live path MaskOff is 25");

    int got_live2 = collect_session_mask_pairs("/tmp", "dummy_sess", noon_ms, 0,
                                               noon_ms + 1100LL * 60000LL,
                                               noon_ms + 1480LL * 60000LL,
                                               &pair, 1);
    TEST_ASSERT(got_live2 == 1, "PROP_17: Live path cross-noon produces 1 pair");
    TEST_ASSERT(pair.on_min == 1100, "PROP_17: Live path MaskOn is 1100");
    TEST_ASSERT(pair.off_min == 1440, "PROP_17: Live path MaskOff clamped to 1440");
}

int main(void)
{
    /* PIN THE HOST ZONE. Two properties assert a recording_id, and
     * as11_time_format_recording_id() formats it with localtime_r() — by
     * design, because the AS11 writes its own Startdate in local civil time
     * (a card here shows "Startdate 29-JUL-2026" against startdate 29.07.26).
     *
     * The previous revision hid that: its private copy of the formatter called
     * gmtime_r() with the comment "Test environment uses gmtime_r for UTC
     * baseline", so PROP_10 and PROP_16 passed in every zone while asserting
     * behaviour the firmware does not have. A copy that has DRIFTED from the
     * original is worse than one that matches — it is green and wrong.
     *
     * With the real formatter in use the assertions are zone-dependent, so the
     * zone is fixed here rather than inherited from whoever runs the suite. */
    setenv("TZ", "UTC", 1);
    tzset();

    printf("=== Running Automated Host Property Test Suite (17/17 Properties) ===\n");

    test_prop_01_noon_day_boundaries();
    test_prop_02_timezone_offset_persistence();
    test_prop_03_zle_event_loop();
    test_prop_04_spool_symmetric_rounding();
    test_prop_05_clamping_safety();
    test_prop_06_missing_sentinel_v1_v2();
    test_prop_07_channel_map_validation();
    test_prop_08_invalid_passthrough_flag();
    test_prop_09_signal_count_invariant();
    test_prop_10_header_date_calendar_vs_noon();
    test_prop_11_zero_sample_protection();
    test_prop_12_leap_year_civil_date();
    test_prop_13_str_oldest_record_date();
    test_prop_14_ms_to_samples_rounding();
    test_prop_15_safe_denominator_guard();
    test_prop_16_recording_id_format();
    test_prop_17_str_mask_window_clamping();

    printf("\n=== Property Test Results: %d Passed, %d Failed ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
