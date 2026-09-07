/* Pure host tests for the native History data model. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "touch_history.h"

static void test_signal_contract(void)
{
    assert(TOUCH_HISTORY_SIGNAL_COUNT == 8);
    assert(TOUCH_HISTORY_OVERVIEW_POINTS == 480);
    uint16_t all = 0;
    for (int i = 0; i < TOUCH_HISTORY_SIGNAL_COUNT; ++i)
        all |= TOUCH_HISTORY_SIGNAL_BIT(i);
    assert(all == 0xffU);
    assert(TOUCH_HISTORY_SIGNAL_FLOW == 0);
    assert(TOUCH_HISTORY_SIGNAL_MOTION == 7);
}

static void test_shared_axis_bins(void)
{
    const int64_t start = 1000000;
    const int64_t end = start + 480000;
    assert(touch_history_overview_bin(start, end, start - 1) == -1);
    assert(touch_history_overview_bin(start, end, start) == 0);
    assert(touch_history_overview_bin(start, end, start + 999) == 0);
    assert(touch_history_overview_bin(start, end, start + 1000) == 1);
    assert(touch_history_overview_bin(start, end, end - 1) == 479);
    assert(touch_history_overview_bin(start, end, end) == 479);
    assert(touch_history_overview_bin(start, start, start) == -1);

    assert(touch_history_range_point_count(
               TOUCH_HISTORY_SIGNAL_FLOW, 5000) == 125);
    assert(touch_history_range_point_count(
               TOUCH_HISTORY_SIGNAL_FLOW, 20000) == 480);
    assert(touch_history_range_point_count(
               TOUCH_HISTORY_SIGNAL_PRESSURE, 60000) == 30);
    assert(touch_history_range_point_count(
               TOUCH_HISTORY_SIGNAL_SPO2, 60000) == 60);
    assert(touch_history_range_point_count(
               TOUCH_HISTORY_SIGNAL_SPO2, 0) == 0);
    const uint64_t eight_hours = 8ULL * 60ULL * 60ULL * 1000ULL;
    assert(touch_history_flow_range_prefers_raw(
        22ULL * 60ULL * 1000ULL, eight_hours));
    assert(touch_history_flow_range_prefers_raw(
        90ULL * 60ULL * 1000ULL, eight_hours));
    assert(!touch_history_flow_range_prefers_raw(
        3ULL * 60ULL * 60ULL * 1000ULL, eight_hours));

    /* 25 Hz raw Flow is safe as a line while each sample retains a display
     * bin. Beyond the 480-point view capacity it must become a min/max
     * envelope rather than a signed mean that can cancel around zero. */
    assert(!touch_history_flow_bins_need_envelope(5000, 125, 250));
    assert(!touch_history_flow_bins_need_envelope(19200, 480, 250));
    assert(touch_history_flow_bins_need_envelope(19201, 480, 250));
    assert(touch_history_flow_bins_need_envelope(20000, 480, 250));
    assert(touch_history_flow_bins_need_envelope(
        22ULL * 60ULL * 1000ULL, 480, 250));
    assert(touch_history_flow_bins_need_envelope(UINT64_MAX, 480, 250));
    assert(touch_history_flow_bins_need_envelope(5000, 0, 250));

    /* Regression: 102,730 raw samples at 25 Hz are only about 68.5 minutes.
     * A count cap of 86,400 used to reject this valid physical-card track. */
    assert(touch_history_sample_span_within(
        102730U, 10000000U, 250U, 36ULL * 60ULL * 60ULL * 1000ULL));
    assert(!touch_history_sample_span_within(
        UINT32_MAX, 10000000U, 1U, 36ULL * 60ULL * 60ULL * 1000ULL));
    assert(!touch_history_sample_span_within(1U, 0U, 1U, 1000U));
}

static void test_event_taxonomy_and_combined_ahi(void)
{
    assert(touch_history_event_type_from_name("ObstructiveApneaEnd") ==
           TOUCH_HISTORY_EVENT_OBSTRUCTIVE_APNEA);
    assert(touch_history_event_type_from_name("CentralApneaEnd") ==
           TOUCH_HISTORY_EVENT_CENTRAL_APNEA);
    assert(touch_history_event_type_from_name("HypopneaEnd") ==
           TOUCH_HISTORY_EVENT_HYPOPNEA);
    assert(touch_history_event_type_from_name("ApneaEnd") ==
           TOUCH_HISTORY_EVENT_GENERIC_APNEA);
    assert(touch_history_event_type_from_name("ReraEnd") ==
           TOUCH_HISTORY_EVENT_RERA);
    assert(touch_history_event_type_from_name("MaskOff") ==
           TOUCH_HISTORY_EVENT_UNKNOWN);

    touch_history_event_t replayed[] = {
        {.type = TOUCH_HISTORY_EVENT_OBSTRUCTIVE_APNEA,
         .start_ms = 90000, .end_ms = 100000},
        {.type = TOUCH_HISTORY_EVENT_OBSTRUCTIVE_APNEA,
         .start_ms = 88000, .end_ms = 100250},
        {.type = TOUCH_HISTORY_EVENT_CENTRAL_APNEA,
         .start_ms = 90000, .end_ms = 100000},
        {.type = TOUCH_HISTORY_EVENT_OBSTRUCTIVE_APNEA,
         .start_ms = 91000, .end_ms = 101000},
        {.type = TOUCH_HISTORY_EVENT_OBSTRUCTIVE_APNEA,
         .start_ms = 91000, .end_ms = 101000},
    };
    size_t retained = touch_history_deduplicate_events(
        replayed, sizeof(replayed) / sizeof(replayed[0]));
    assert(retained == 3);
    assert(replayed[0].start_ms <= replayed[1].start_ms);
    assert(replayed[1].start_ms <= replayed[2].start_ms);

    /* Two combined sessions totalling 2h. This must not be an average of
     * per-session indices: all eligible event counts share one denominator. */
    uint32_t counts[TOUCH_HISTORY_EVENT_TYPE_COUNT] = {4, 2, 6, 2, 4};
    touch_history_event_totals_t totals;
    assert(touch_history_compute_event_indices(counts, 2ULL * 3600000ULL,
                                               &totals));
    assert(totals.complete && totals.has_indices);
    assert(fabsf(totals.oai - 2.0f) < 0.0001f);
    assert(fabsf(totals.cai - 1.0f) < 0.0001f);
    assert(fabsf(totals.hi - 3.0f) < 0.0001f);
    assert(fabsf(totals.generic_ai - 1.0f) < 0.0001f);
    assert(fabsf(totals.ahi - 7.0f) < 0.0001f);
    assert(fabsf(totals.rera - 2.0f) < 0.0001f);
    assert(totals.total_count == 18);

    memset(&totals, 0xa5, sizeof(totals));
    assert(!touch_history_compute_event_indices(counts, 0, &totals));
    assert(totals.complete && !totals.has_indices);
    assert(totals.ahi == 0.0f);
}

static void test_source_histogram_percentiles(void)
{
    uint32_t counts[16] = {0};
    int32_t value = -1;
    /* Four 10s followed by four 20s exercise the portal-compatible boundary
     * interpolation; P50 lies midway between the occupied bucket centres. */
    counts[0] = 4;
    counts[10] = 4;
    assert(touch_history_weighted_percentile_histogram(
        counts, 16, 10, 8, 500, &value));
    assert(value == 15);
    assert(touch_history_weighted_percentile_histogram(
        counts, 16, 10, 8, 50, &value));
    assert(value == 10);
    assert(touch_history_weighted_percentile_histogram(
        counts, 16, 10, 8, 995, &value));
    assert(value == 20);
    assert(!touch_history_weighted_percentile_histogram(
        counts, 16, 10, 0, 500, &value));
    /* A corrupt count advertised by a caller must not overflow rank math. */
    assert(!touch_history_weighted_percentile_histogram(
        counts, 16, 10, UINT64_MAX, 995, &value));
    assert(!touch_history_weighted_percentile_histogram(
        counts, 16, INT32_MAX, 8, 995, &value));

    touch_history_stats_t stats = {0};
    assert(TOUCH_HISTORY_STATS_MAX_VALUES == 4);
    assert(sizeof(stats.values) / sizeof(stats.values[0]) == 4);
    assert(TOUCH_HISTORY_STAT_P05 != TOUCH_HISTORY_STAT_P5);
    assert(TOUCH_HISTORY_STAT_TIME_BELOW_88 != TOUCH_HISTORY_STAT_MINIMUM);

    int16_t scaled = 0;
    assert(touch_history_scale_source_x100(
        TOUCH_HISTORY_SIGNAL_FLOW, 35, &scaled));
    assert(scaled == 35); /* 0.35 L/s must not become 21.00 L/min. */
    assert(touch_history_scale_source_x100(
        TOUCH_HISTORY_SIGNAL_LEAK, 13, &scaled));
    assert(scaled == 780); /* Leak remains 7.80 L/min. */

    int64_t corrected = 0;
    assert(touch_history_apply_clock_drift(
        1760000000000LL, 60000, &corrected));
    assert(corrected == 1760000060000LL);
    assert(!touch_history_apply_clock_drift(
        INT64_MAX - 10, 1000, &corrected));
    assert(!touch_history_apply_clock_drift(
        1760000000000LL, 24LL * 60LL * 60LL * 1000LL, &corrected));
    assert(!touch_history_apply_clock_drift(
        1760000000000LL, -24LL * 60LL * 60LL * 1000LL, &corrected));
}

static size_t append_varint(uint8_t *out, size_t offset, uint64_t value)
{
    do {
        uint8_t byte = (uint8_t)(value & 0x7fU);
        value >>= 7;
        if (value) byte |= 0x80U;
        out[offset++] = byte;
    } while (value);
    return offset;
}

static size_t append_scalar(uint8_t *out, size_t offset, unsigned field,
                            uint64_t value)
{
    offset = append_varint(out, offset, (uint64_t)field << 3);
    return append_varint(out, offset, value);
}

static size_t append_bytes(uint8_t *out, size_t offset, unsigned field,
                           const uint8_t *bytes, size_t length)
{
    offset = append_varint(out, offset, ((uint64_t)field << 3) | 2U);
    offset = append_varint(out, offset, length);
    memcpy(out + offset, bytes, length);
    return offset + length;
}

static void test_summary_record_decoder(void)
{
    uint8_t session_one[24];
    size_t session_one_len = 0;
    session_one_len = append_scalar(session_one, session_one_len, 1,
                                    1767268800000ULL);
    session_one_len = append_scalar(session_one, session_one_len, 2, 180);
    uint8_t session_two[24];
    size_t session_two_len = 0;
    session_two_len = append_scalar(session_two, session_two_len, 1,
                                    1767279600000ULL);
    session_two_len = append_scalar(session_two, session_two_len, 2, 240);
    uint8_t sessions[64];
    size_t sessions_len = 0;
    sessions_len = append_bytes(sessions, sessions_len, 1,
                                session_one, session_one_len);
    sessions_len = append_bytes(sessions, sessions_len, 1,
                                session_two, session_two_len);
    uint8_t pressure[8];
    size_t pressure_len = append_scalar(pressure, 0, 3, 1040);
    uint8_t leak[8];
    size_t leak_len = append_scalar(leak, 0, 4, 13);

    uint8_t record[192];
    size_t length = 0;
    length = append_scalar(record, length, 2, 1767268800000ULL);
    length = append_scalar(record, length, 5, 425);
    length = append_bytes(record, length, 6, sessions, sessions_len);
    length = append_scalar(record, length, 7, 170);
    length = append_scalar(record, length, 7, 175); /* last scalar wins */
    length = append_scalar(record, length, 9, 80);
    length = append_scalar(record, length, 10, 40);
    length = append_scalar(record, length, 11, 30);
    length = append_scalar(record, length, 13, 20);
    length = append_bytes(record, length, 14, leak, leak_len);
    length = append_bytes(record, length, 21, pressure, pressure_len);

    touch_history_day_t day = {.sessions = 3};
    strcpy(day.day, "20260101");
    assert(touch_history_decode_summary_record(record, length, &day));
    assert(day.sessions == 3); /* terminal manifest truth is preserved */
    assert(day.has_summary && day.has_mask_off_count);
    assert(day.mask_off_count == 2);
    assert(day.has_usage && day.usage_min == 420);
    assert(day.has_device_ahi && fabsf(day.device_ahi - 1.75f) < 0.001f);
    assert(day.has_ahi && fabsf(day.ahi - 1.75f) < 0.001f);
    assert(day.has_oai && fabsf(day.oai - 0.40f) < 0.001f);
    assert(day.has_cai && fabsf(day.cai - 0.30f) < 0.001f);
    assert(day.has_hi && fabsf(day.hi - 0.80f) < 0.001f);
    assert(day.has_rera && fabsf(day.rera - 0.20f) < 0.001f);
    assert(day.has_pressure_p95 &&
           fabsf(day.pressure_p95 - 10.40f) < 0.001f);
    assert(day.has_leak_p95 && fabsf(day.leak_p95 - 7.8f) < 0.001f);

    touch_history_day_t unchanged = day;
    const uint8_t truncated[] = {0x2a, 0x80};
    assert(!touch_history_decode_summary_record(
        truncated, sizeof(truncated), &day));
    assert(memcmp(&day, &unchanged, sizeof(day)) == 0);

    touch_history_day_t wrong_day = unchanged;
    strcpy(wrong_day.day, "20260103");
    touch_history_day_t wrong_day_before = wrong_day;
    assert(!touch_history_decode_summary_record(record, length, &wrong_day));
    assert(memcmp(&wrong_day, &wrong_day_before, sizeof(wrong_day)) == 0);

    uint8_t unavailable_record[32];
    size_t unavailable_len = 0;
    unavailable_len = append_scalar(unavailable_record, unavailable_len, 2,
                                    1767268800000ULL);
    unavailable_len = append_scalar(unavailable_record, unavailable_len, 7,
                                    UINT64_MAX);
    touch_history_day_t unavailable = {0};
    strcpy(unavailable.day, "20260101");
    assert(touch_history_decode_summary_record(
        unavailable_record, unavailable_len, &unavailable));
    assert(unavailable.has_summary && !unavailable.has_device_ahi);

    /* A wrapped spool response is not a stored day record. */
    uint8_t wrapped[224];
    size_t wrapped_len = append_bytes(wrapped, 0, 2, record, length);
    assert(!touch_history_decode_summary_record(wrapped, wrapped_len, &day));
    assert(memcmp(&day, &unchanged, sizeof(day)) == 0);
}

int main(void)
{
    test_signal_contract();
    test_shared_axis_bins();
    test_event_taxonomy_and_combined_ahi();
    test_source_histogram_percentiles();
    test_summary_record_decoder();
    puts("touch history model tests passed");
    return 0;
}
