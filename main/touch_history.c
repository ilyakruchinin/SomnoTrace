/* SD history adapter for the Waveshare native UI. */
#include "touch_history.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t number;
    uint8_t wire;
    uint64_t varint;
    const uint8_t *bytes;
    size_t length;
} history_pb_field_t;

static bool history_pb_varint(const uint8_t *data, size_t length, size_t *offset,
                              uint64_t *value)
{
    if (!data || !offset || !value) return false;
    uint64_t decoded = 0;
    for (unsigned byte_index = 0; byte_index < 10; ++byte_index) {
        if (*offset >= length) return false;
        uint8_t byte = data[(*offset)++];
        if (byte_index == 9 && (byte & 0xfeU)) return false;
        decoded |= (uint64_t)(byte & 0x7fU) << (byte_index * 7U);
        if (!(byte & 0x80U)) {
            *value = decoded;
            return true;
        }
    }
    return false;
}

static bool history_pb_next(const uint8_t *data, size_t length, size_t *offset,
                            history_pb_field_t *field)
{
    if (!data || !offset || !field || *offset >= length) return false;
    uint64_t tag = 0;
    if (!history_pb_varint(data, length, offset, &tag) ||
        (tag >> 3) == 0 || (tag >> 3) > 0x1fffffffU) return false;
    memset(field, 0, sizeof(*field));
    field->number = (uint32_t)(tag >> 3);
    field->wire = (uint8_t)(tag & 7U);
    switch (field->wire) {
    case 0:
        return history_pb_varint(data, length, offset, &field->varint);
    case 1:
        if (length - *offset < 8U) return false;
        field->bytes = data + *offset;
        field->length = 8;
        *offset += 8;
        return true;
    case 2: {
        uint64_t field_length = 0;
        if (!history_pb_varint(data, length, offset, &field_length) ||
            field_length > length - *offset) return false;
        field->bytes = data + *offset;
        field->length = (size_t)field_length;
        *offset += field->length;
        return true;
    }
    case 5:
        if (length - *offset < 4U) return false;
        field->bytes = data + *offset;
        field->length = 4;
        *offset += 4;
        return true;
    default:
        return false;
    }
}

static bool history_summary_metric(const history_pb_field_t *metric,
                                   uint32_t wanted_subfield,
                                   bool *has_value, int64_t *value)
{
    if (!metric || metric->wire != 2 || !has_value || !value) return false;
    size_t offset = 0;
    while (offset < metric->length) {
        history_pb_field_t field;
        if (!history_pb_next(metric->bytes, metric->length, &offset, &field))
            return false;
        if (field.number == wanted_subfield) {
            if (field.wire != 0) return false;
            *value = (int64_t)field.varint;
            *has_value = true;
        }
    }
    return true;
}

static bool history_summary_sessions(const history_pb_field_t *sessions,
                                     uint32_t *session_count,
                                     int64_t *usage_minutes)
{
    if (!sessions || sessions->wire != 2 || !session_count ||
        !usage_minutes) return false;
    size_t offset = 0;
    while (offset < sessions->length) {
        history_pb_field_t wrapper;
        if (!history_pb_next(sessions->bytes, sessions->length, &offset,
                             &wrapper)) return false;
        if (wrapper.number != 1) continue;
        if (wrapper.wire != 2) return false;
        int64_t duration = 0;
        size_t inner_offset = 0;
        while (inner_offset < wrapper.length) {
            history_pb_field_t entry;
            if (!history_pb_next(wrapper.bytes, wrapper.length, &inner_offset,
                                 &entry)) return false;
            if (entry.number == 2) {
                if (entry.wire != 0) return false;
                duration = (int64_t)entry.varint;
            }
        }
        if (duration < 0 || duration > 1440 || *usage_minutes > 1440 - duration ||
            *session_count == UINT32_MAX) return false;
        *usage_minutes += duration;
        (*session_count)++;
    }
    return true;
}

static bool history_summary_index_value(bool present, int64_t raw,
                                        float *value)
{
    if (!present || raw < 0 || raw > INT16_MAX || !value) return false;
    *value = (float)raw * 0.01f;
    return true;
}

static int64_t history_summary_days_from_civil(int year, int month, int day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = (unsigned)(year - era * 400);
    const unsigned day_of_year = (unsigned)(
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 -
                                year_of_era / 100 + day_of_year;
    return (int64_t)era * 146097 + (int)day_of_era - 719468;
}

static bool history_summary_period_matches_day(const char day[9],
                                               int64_t period_start_ms)
{
    if (!day || day[8] != '\0') return false;
    for (size_t i = 0; i < 8; ++i)
        if (day[i] < '0' || day[i] > '9') return false;
    int year = (day[0] - '0') * 1000 + (day[1] - '0') * 100 +
               (day[2] - '0') * 10 + day[3] - '0';
    int month = (day[4] - '0') * 10 + day[5] - '0';
    int month_day = (day[6] - '0') * 10 + day[7] - '0';
    static const uint8_t month_days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (year < 2000 || year > 2200 || month < 1 || month > 12)
        return false;
    int maximum_day = month_days[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) ||
                       year % 400 == 0)) maximum_day = 29;
    if (month_day < 1 || month_day > maximum_day) return false;
    int64_t midnight_ms = history_summary_days_from_civil(
        year, month, month_day) * 86400000LL;
    /* Local noon spans 22:00 UTC on the preceding day (UTC+14) through
     * 00:00 UTC on the following day (UTC-12). */
    return period_start_ms >= midnight_ms - 2LL * 60LL * 60LL * 1000LL &&
           period_start_ms <= midnight_ms + 24LL * 60LL * 60LL * 1000LL;
}

bool touch_history_decode_summary_record(const uint8_t *record, size_t length,
                                         touch_history_day_t *day)
{
    if (!record || !length || !day) return false;
    enum {
        SUMMARY_PERIOD_START = 2,
        SUMMARY_DURATION = 5,
        SUMMARY_SESSIONS = 6,
        SUMMARY_AHI = 7,
        SUMMARY_HI = 9,
        SUMMARY_OAI = 10,
        SUMMARY_CAI = 11,
        SUMMARY_RERA = 13,
        SUMMARY_LEAK = 14,
        SUMMARY_PRESSURE = 21,
    };
    struct {
        int64_t duration;
        int64_t period_start;
        int64_t ahi;
        int64_t hi;
        int64_t oai;
        int64_t cai;
        int64_t rera;
        int64_t leak_p95;
        int64_t pressure_p95;
        uint32_t session_count;
        int64_t session_usage;
        bool has_duration;
        bool has_period_start;
        bool has_sessions;
        bool has_ahi;
        bool has_hi;
        bool has_oai;
        bool has_cai;
        bool has_rera;
        bool has_leak_p95;
        bool has_pressure_p95;
        bool recognized;
    } parsed = {0};

    size_t offset = 0;
    while (offset < length) {
        history_pb_field_t field;
        if (!history_pb_next(record, length, &offset, &field)) return false;
        int64_t *scalar = NULL;
        bool *present = NULL;
        switch (field.number) {
        case SUMMARY_PERIOD_START:
            scalar = &parsed.period_start;
            present = &parsed.has_period_start;
            break;
        case SUMMARY_DURATION:
            scalar = &parsed.duration;
            present = &parsed.has_duration;
            break;
        case SUMMARY_AHI:
            scalar = &parsed.ahi;
            present = &parsed.has_ahi;
            break;
        case SUMMARY_HI:
            scalar = &parsed.hi;
            present = &parsed.has_hi;
            break;
        case SUMMARY_OAI:
            scalar = &parsed.oai;
            present = &parsed.has_oai;
            break;
        case SUMMARY_CAI:
            scalar = &parsed.cai;
            present = &parsed.has_cai;
            break;
        case SUMMARY_RERA:
            scalar = &parsed.rera;
            present = &parsed.has_rera;
            break;
        default:
            break;
        }
        if (scalar) {
            if (field.wire != 0) return false;
            *scalar = (int64_t)field.varint;
            *present = true;
            parsed.recognized = true;
        } else if (field.number == SUMMARY_SESSIONS) {
            if (!history_summary_sessions(&field, &parsed.session_count,
                                          &parsed.session_usage)) return false;
            parsed.has_sessions = true;
            parsed.recognized = true;
        } else if (field.number == SUMMARY_LEAK) {
            if (!history_summary_metric(&field, 4, &parsed.has_leak_p95,
                                        &parsed.leak_p95)) return false;
            parsed.recognized = true;
        } else if (field.number == SUMMARY_PRESSURE) {
            if (!history_summary_metric(&field, 3,
                                        &parsed.has_pressure_p95,
                                        &parsed.pressure_p95)) return false;
            parsed.recognized = true;
        }
    }
    if (!parsed.recognized || !parsed.has_period_start ||
        parsed.period_start < 946684800000LL ||
        !history_summary_period_matches_day(day->day, parsed.period_start))
        return false;
    if (parsed.has_duration &&
        (parsed.duration < 0 || parsed.duration > 1440)) return false;
    if (parsed.session_count > INT_MAX) return false;
    const int64_t summary_values[] = {
        parsed.ahi, parsed.hi, parsed.oai, parsed.cai, parsed.rera,
        parsed.leak_p95, parsed.pressure_p95,
    };
    const bool summary_value_present[] = {
        parsed.has_ahi, parsed.has_hi, parsed.has_oai, parsed.has_cai,
        parsed.has_rera, parsed.has_leak_p95, parsed.has_pressure_p95,
    };
    for (size_t i = 0; i < sizeof(summary_values) / sizeof(summary_values[0]);
         ++i) {
        if (summary_value_present[i] && summary_values[i] > INT16_MAX)
            return false;
    }

    touch_history_day_t decoded = *day;
    decoded.has_summary = true;
    decoded.has_mask_off_count = parsed.has_sessions;
    if (parsed.has_sessions)
        decoded.mask_off_count = (int)parsed.session_count;
    decoded.has_usage = parsed.has_sessions || parsed.has_duration;
    if (decoded.has_usage) {
        int64_t usage = parsed.session_usage > 0 ? parsed.session_usage
                                                : parsed.duration;
        if (usage < 0 || usage > INT_MAX) return false;
        decoded.usage_min = (int)usage;
    }
    decoded.has_device_ahi = history_summary_index_value(
        parsed.has_ahi, parsed.ahi, &decoded.device_ahi);
    decoded.has_ahi = decoded.has_device_ahi;
    if (decoded.has_device_ahi) decoded.ahi = decoded.device_ahi;
    decoded.has_hi = history_summary_index_value(
        parsed.has_hi, parsed.hi, &decoded.hi);
    decoded.has_oai = history_summary_index_value(
        parsed.has_oai, parsed.oai, &decoded.oai);
    decoded.has_cai = history_summary_index_value(
        parsed.has_cai, parsed.cai, &decoded.cai);
    decoded.has_rera = history_summary_index_value(
        parsed.has_rera, parsed.rera, &decoded.rera);
    decoded.has_pressure_p95 = parsed.has_pressure_p95 &&
        parsed.pressure_p95 >= 0 && parsed.pressure_p95 <= INT16_MAX;
    if (decoded.has_pressure_p95)
        decoded.pressure_p95 = (float)parsed.pressure_p95 * 0.01f;
    decoded.has_leak_p95 = parsed.has_leak_p95 &&
        parsed.leak_p95 >= 0 && parsed.leak_p95 <= INT16_MAX;
    if (decoded.has_leak_p95)
        decoded.leak_p95 = (float)parsed.leak_p95 * 0.6f;
    *day = decoded;
    return true;
}

touch_history_event_type_t touch_history_event_type_from_name(const char *name)
{
    if (!name) return TOUCH_HISTORY_EVENT_UNKNOWN;
    if (!strcmp(name, "ObstructiveApneaEnd"))
        return TOUCH_HISTORY_EVENT_OBSTRUCTIVE_APNEA;
    if (!strcmp(name, "CentralApneaEnd"))
        return TOUCH_HISTORY_EVENT_CENTRAL_APNEA;
    if (!strcmp(name, "HypopneaEnd"))
        return TOUCH_HISTORY_EVENT_HYPOPNEA;
    if (!strcmp(name, "ApneaEnd"))
        return TOUCH_HISTORY_EVENT_GENERIC_APNEA;
    if (!strcmp(name, "ReraEnd"))
        return TOUCH_HISTORY_EVENT_RERA;
    return TOUCH_HISTORY_EVENT_UNKNOWN;
}

static int history_event_identity_order(const void *left, const void *right)
{
    const touch_history_event_t *a = left;
    const touch_history_event_t *b = right;
    int64_t a_second = a->end_ms / 1000;
    int64_t b_second = b->end_ms / 1000;
    if (a_second < b_second) return -1;
    if (a_second > b_second) return 1;
    if (a->type < b->type) return -1;
    if (a->type > b->type) return 1;
    if (a->start_ms < b->start_ms) return -1;
    if (a->start_ms > b->start_ms) return 1;
    return 0;
}

static int history_event_time_order(const void *left, const void *right)
{
    const touch_history_event_t *a = left;
    const touch_history_event_t *b = right;
    if (a->start_ms < b->start_ms) return -1;
    if (a->start_ms > b->start_ms) return 1;
    if (a->type < b->type) return -1;
    if (a->type > b->type) return 1;
    return 0;
}

size_t touch_history_deduplicate_events(touch_history_event_t *events,
                                        size_t count)
{
    if (!events || count < 2) return events ? count : 0;
    qsort(events, count, sizeof(*events), history_event_identity_order);
    size_t retained = 0;
    for (size_t i = 0; i < count; ++i) {
        if (retained > 0 &&
            events[retained - 1].end_ms / 1000 == events[i].end_ms / 1000 &&
            events[retained - 1].type == events[i].type)
            continue;
        events[retained++] = events[i];
    }
    qsort(events, retained, sizeof(*events), history_event_time_order);
    return retained;
}

bool touch_history_compute_event_indices(
    const uint32_t counts[TOUCH_HISTORY_EVENT_TYPE_COUNT],
    uint64_t eligible_therapy_ms,
    touch_history_event_totals_t *totals)
{
    if (!counts || !totals) return false;
    memset(totals, 0, sizeof(*totals));
    memcpy(totals->count, counts, sizeof(totals->count));
    totals->eligible_therapy_ms = eligible_therapy_ms;
    for (size_t i = 0; i < TOUCH_HISTORY_EVENT_TYPE_COUNT; ++i) {
        uint64_t combined = (uint64_t)totals->total_count + counts[i];
        totals->total_count = combined > UINT32_MAX ? UINT32_MAX
                                                     : (uint32_t)combined;
    }
    totals->complete = true;
    if (!eligible_therapy_ms) return false;

    const double per_hour = 3600000.0 / (double)eligible_therapy_ms;
    totals->oai = (float)(counts[TOUCH_HISTORY_EVENT_OBSTRUCTIVE_APNEA] *
                          per_hour);
    totals->cai = (float)(counts[TOUCH_HISTORY_EVENT_CENTRAL_APNEA] *
                          per_hour);
    totals->hi = (float)(counts[TOUCH_HISTORY_EVENT_HYPOPNEA] * per_hour);
    totals->generic_ai = (float)(counts[TOUCH_HISTORY_EVENT_GENERIC_APNEA] *
                                 per_hour);
    totals->rera = (float)(counts[TOUCH_HISTORY_EVENT_RERA] * per_hour);
    totals->ahi = totals->oai + totals->cai + totals->hi +
                  totals->generic_ai;
    totals->has_indices = true;
    return true;
}

int touch_history_overview_bin(int64_t axis_start_ms, int64_t axis_end_ms,
                               int64_t timestamp_ms)
{
    if (axis_end_ms <= axis_start_ms || timestamp_ms < axis_start_ms ||
        timestamp_ms > axis_end_ms) return -1;
    if (timestamp_ms == axis_end_ms) return TOUCH_HISTORY_OVERVIEW_POINTS - 1;
    uint64_t offset = (uint64_t)(timestamp_ms - axis_start_ms);
    uint64_t span = (uint64_t)(axis_end_ms - axis_start_ms);
    if (offset > UINT64_MAX / TOUCH_HISTORY_OVERVIEW_POINTS) return -1;
    return (int)(offset * TOUCH_HISTORY_OVERVIEW_POINTS / span);
}

uint16_t touch_history_range_point_count(touch_history_signal_t signal,
                                         uint64_t duration_ms)
{
    if (signal < TOUCH_HISTORY_SIGNAL_FLOW ||
        signal >= TOUCH_HISTORY_SIGNAL_COUNT || !duration_ms) return 0;
    uint32_t hz_x10 = signal == TOUCH_HISTORY_SIGNAL_FLOW ? 250U :
        (signal >= TOUCH_HISTORY_SIGNAL_SPO2 ? 10U : 5U);
    uint64_t points = duration_ms > (UINT64_MAX - 9999U) / hz_x10
        ? TOUCH_HISTORY_OVERVIEW_POINTS
        : (duration_ms * hz_x10 + 9999U) / 10000U;
    if (points < 2U) points = 2U;
    if (points > TOUCH_HISTORY_OVERVIEW_POINTS)
        points = TOUCH_HISTORY_OVERVIEW_POINTS;
    return (uint16_t)points;
}

bool touch_history_flow_range_prefers_raw(uint64_t duration_ms,
                                          uint64_t night_duration_ms)
{
    const uint64_t minimum_zoom_ms = 22ULL * 60ULL * 1000ULL;
    if (!duration_ms || !night_duration_ms ||
        duration_ms > night_duration_ms) return false;
    return duration_ms <= minimum_zoom_ms ||
           duration_ms <= night_duration_ms / 4U;
}

bool touch_history_flow_bins_need_envelope(uint64_t duration_ms,
                                           uint16_t point_count,
                                           uint32_t sample_hz_x10)
{
    if (!duration_ms || !point_count || !sample_hz_x10) return true;
    if (duration_ms > UINT64_MAX / sample_hz_x10) return true;
    return duration_ms * sample_hz_x10 >
           (uint64_t)point_count * 10000U;
}

bool touch_history_sample_span_within(uint32_t sample_count,
                                      uint32_t period_num_us,
                                      uint32_t period_den,
                                      uint64_t maximum_ms)
{
    if (!sample_count || !period_num_us || !period_den || !maximum_ms ||
        maximum_ms > UINT64_MAX / 1000U)
        return false;
    if ((uint64_t)sample_count > UINT64_MAX / period_num_us)
        return false;
    uint64_t sample_span = (uint64_t)sample_count * period_num_us;
    uint64_t maximum_us = maximum_ms * 1000U;
    if (maximum_us > UINT64_MAX / period_den)
        return true;
    return sample_span <= maximum_us * period_den;
}

bool touch_history_scale_source_x100(touch_history_signal_t signal,
                                     int16_t raw, int16_t *scaled)
{
    if (!scaled) return false;
    int32_t value = raw;
    switch (signal) {
    case TOUCH_HISTORY_SIGNAL_LEAK:
        /* Leak source is hundredths L/s; x100 L/min = raw * 60. */
        value *= 60;
        break;
    case TOUCH_HISTORY_SIGNAL_MOTION:
        value *= TOUCH_HISTORY_VALUE_SCALE;
        break;
    case TOUCH_HISTORY_SIGNAL_FLOW:
        /* Rich Flow remains source-native hundredths L/s. */
    case TOUCH_HISTORY_SIGNAL_PRESSURE:
    case TOUCH_HISTORY_SIGNAL_FLOW_LIMIT:
    case TOUCH_HISTORY_SIGNAL_SNORE:
    case TOUCH_HISTORY_SIGNAL_SPO2:
    case TOUCH_HISTORY_SIGNAL_PULSE:
        break;
    default:
        return false;
    }
    if (value <= INT16_MIN || value > INT16_MAX) return false;
    *scaled = (int16_t)value;
    return true;
}

bool touch_history_apply_clock_drift(int64_t as11_ms, int64_t drift_ms,
                                     int64_t *corrected_ms)
{
    const int64_t maximum_drift_ms = 24LL * 60LL * 60LL * 1000LL;
    if (!corrected_ms || as11_ms <= 0 ||
        drift_ms <= -maximum_drift_ms || drift_ms >= maximum_drift_ms ||
        (drift_ms > 0 && as11_ms > INT64_MAX - drift_ms) ||
        (drift_ms < 0 && as11_ms < INT64_MIN - drift_ms)) return false;
    int64_t corrected = as11_ms + drift_ms;
    if (corrected <= 0) return false;
    *corrected_ms = corrected;
    return true;
}

bool touch_history_weighted_percentile_histogram(
    const uint32_t *counts, size_t bin_count, int32_t first_value,
    uint64_t sample_count, uint16_t percentile_per_mille,
    int32_t *value_out)
{
    if (!counts || !bin_count || !sample_count || !value_out ||
        percentile_per_mille > 1000U) return false;

    /* Avoid overflowing the public helper for a corrupt/inconsistent caller
     * even though a real night is far smaller than UINT64_MAX samples. */
    uint64_t target_index = (sample_count / 1000U) * percentile_per_mille +
        ((sample_count % 1000U) * percentile_per_mille) / 1000U;
    if (target_index >= sample_count) target_index = sample_count - 1U;
    uint64_t cumulative = 0;
    for (size_t i = 0; i < bin_count; ++i) {
        uint32_t weight = counts[i];
        if (!weight) continue;
        if (cumulative > UINT64_MAX - weight) return false;
        cumulative += weight;
        if (cumulative > target_index) {
            int64_t exact = (int64_t)first_value + (int64_t)i;
            if (exact < INT32_MIN || exact > INT32_MAX) return false;
            *value_out = (int32_t)exact;
            return true;
        }
        if (cumulative != target_index) continue;

        size_t next = i + 1U;
        while (next < bin_count && counts[next] == 0) next++;
        if (next == bin_count) {
            int64_t exact = (int64_t)first_value + (int64_t)i;
            if (exact < INT32_MIN || exact > INT32_MAX) return false;
            *value_out = (int32_t)exact;
            return true;
        }
        /* Match portal.html weightedPercentile(): interpolate between the
         * centres of the two adjacent occupied histogram buckets only when
         * the requested rank lands exactly on their cumulative boundary. */
        double p = (double)percentile_per_mille / 10.0;
        double px = 100.0 / (double)sample_count;
        double p1 = px * ((double)cumulative - (double)weight / 2.0);
        double p2 = px * ((double)(cumulative + counts[next]) -
                          (double)counts[next] / 2.0);
        int64_t base = (int64_t)first_value + (int64_t)i;
        if (base < INT32_MIN || base > INT32_MAX) return false;
        double value = (double)base;
        if (p2 > p1) {
            value += ((p - p1) / (p2 - p1)) * (double)(next - i);
        }
        if (value < INT32_MIN || value > INT32_MAX) return false;
        *value_out = (int32_t)llround(value);
        return true;
    }
    return false;
}

void touch_history_make_preview(const touch_history_overview_t *source,
    touch_history_signal_t signal, int64_t start_ms, int64_t end_ms,
    touch_history_overview_t *out)
{
    memset(out, 0, sizeof(*out));
    out->signal = signal;
    out->axis_start_ms = start_ms; out->axis_end_ms = end_ms;
    out->point_count = TOUCH_HISTORY_OVERVIEW_POINTS;
    out->preview = out->loaded = true;
    out->aggregation = signal == TOUCH_HISTORY_SIGNAL_FLOW
        ? TOUCH_HISTORY_AGGREGATION_ENVELOPE : source->aggregation;
    if (end_ms <= start_ms) return;
    int64_t span = end_ms - start_ms;
    out->bin_width_ms = (span + out->point_count - 1) / out->point_count;
    size_t first = 0;
    for (size_t b = 0; b < out->point_count; ++b) {
        int64_t left = start_ms + span * (int64_t)b / out->point_count;
        int64_t right = start_ms + span * (int64_t)(b + 1) / out->point_count;
        out->timestamp_ms[b] = left + (right - left) / 2;
        out->value_x100[b] = out->upper_x100[b] = out->companion_x100[b] = TOUCH_HISTORY_VALUE_MISSING;
        if (!source->loaded || source->signal != signal || !source->point_count ||
            left < source->axis_start_ms || right > source->axis_end_ms) continue;
        int64_t source_span = source->axis_end_ms - source->axis_start_ms;
        while (first < source->point_count && source->axis_start_ms +
               source_span * (int64_t)(first + 1) / source->point_count <= left) ++first;
        bool gap = false, found = false;
        int16_t low = INT16_MAX, high = INT16_MIN;
        int64_t sum = 0, companion = 0;
        uint32_t count = 0, companions = 0;
        for (size_t i = first; i < source->point_count &&
             source->axis_start_ms + source_span * (int64_t)i / source->point_count < right; ++i) {
            if (!(source->flags[i] & TOUCH_HISTORY_POINT_VALID)) { gap = true; break; }
            int16_t v = source->value_x100[i];
            int16_t upper = (source->flags[i] & TOUCH_HISTORY_POINT_UPPER_VALID)
                ? source->upper_x100[i] : v;
            if (v < low) low = v;
            if (upper > high) high = upper;
            sum += v; ++count; found = true;
            if (source->flags[i] & TOUCH_HISTORY_POINT_COMPANION_VALID) {
                companion += source->companion_x100[i]; ++companions;
            }
            out->flags[b] |= source->flags[i] & TOUCH_HISTORY_POINT_THERAPY;
        }
        if (gap || !found) { out->flags[b] = 0; continue; }
        out->flags[b] |= TOUCH_HISTORY_POINT_VALID;
        out->value_x100[b] = out->aggregation == TOUCH_HISTORY_AGGREGATION_MEAN ? sum / count :
            out->aggregation == TOUCH_HISTORY_AGGREGATION_MAXIMUM ? high : low;
        if (out->aggregation == TOUCH_HISTORY_AGGREGATION_ENVELOPE) {
            out->upper_x100[b] = high;
            out->flags[b] |= TOUCH_HISTORY_POINT_UPPER_VALID;
        }
        if (companions == count) {
            out->companion_x100[b] = companion / companions;
            out->flags[b] |= TOUCH_HISTORY_POINT_COMPANION_VALID;
            out->has_companion = true;
        }
        out->sample_count[b] = 1;
        out->has_data = true;
    }
}

#ifndef TOUCH_HISTORY_MODEL_TEST

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "history_cache.h"
#include "history_flow_cache.h"
#include "oximetry_canonical.h"
#include "sd_storage.h"

#define SNT_MAGIC 0x534E5442u
#define HISTORY_READ_VALUES 512U
#define HISTORY_STORAGE_WAIT_MS 15000U
#define HISTORY_RECORDING_POLL_MS 25U
#define HISTORY_INTERNAL_FALLBACK_MAX_BYTES 2048U
#define HISTORY_AXIS_MAX_MS (36LL * 60LL * 60LL * 1000LL)

static const char *const TAG = "touch_history";

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t tier;
    uint8_t n_channels;
    uint8_t sample_bytes;
    uint16_t sample_hz_x10;
    uint16_t reserved;
    int64_t start_epoch_ms;
    uint32_t sample_count;
    uint32_t reserved2;
} touch_snt_header_t;

typedef struct {
    char path[OXIMETRY_CANONICAL_MAX_PATH];
    uint8_t version;
    uint8_t n_channels;
    uint16_t header_bytes;
    uint16_t sample_hz_x10;
    uint32_t period_num_us;
    uint32_t period_den;
    int64_t start_epoch_ms;
    uint32_t records;
    uint32_t valid_spo2_records;
    uint32_t valid_pulse_records;
    uint32_t valid_motion_records;
    /* Requested-channel rank used by the canonical O2 candidate selector. */
    uint32_t valid_records;
} trace_candidate_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t tier;
    uint8_t timing;
    uint8_t n_channels;
    uint8_t sample_bytes;
    uint8_t flags8;
    uint16_t header_bytes;
    uint32_t period_num_us;
    uint32_t period_den;
    int64_t start_epoch_ms;
    uint32_t sample_count;
    uint32_t data_bytes;
    uint32_t data_crc32;
    uint32_t reserved0;
    uint32_t reserved1;
    uint8_t reserved[16];
} touch_ox_snt3_header_t;

_Static_assert(sizeof(touch_ox_snt3_header_t) ==
                   OXIMETRY_CANONICAL_SNT_HEADER_LEN,
               "canonical oximetry header size");

typedef union {
    struct {
        int16_t minimum[TOUCH_HISTORY_TRACE_POINTS];
        int16_t maximum[TOUCH_HISTORY_TRACE_POINTS];
        uint16_t samples[TOUCH_HISTORY_TRACE_POINTS];
    } flow;
    struct {
        int16_t extreme[TOUCH_HISTORY_TRACE_POINTS];
        uint16_t samples[TOUCH_HISTORY_TRACE_POINTS];
    } trend;
} trace_aggregate_t;

typedef struct {
    trace_aggregate_t aggregate;
    int16_t records[HISTORY_READ_VALUES];
} trace_scratch_t;

typedef struct {
    char day_path[OXIMETRY_CANONICAL_MAX_PATH];
    char record_path[OXIMETRY_CANONICAL_MAX_PATH];
    char pointer_path[OXIMETRY_CANONICAL_MAX_PATH];
    char track_path[OXIMETRY_CANONICAL_MAX_PATH];
    char manifest_path[OXIMETRY_CANONICAL_MAX_PATH];
    trace_candidate_t best;
} ox_trace_find_t;

static bool valid_day(const char *name)
{
    if (!name || strlen(name) != 8) return false;
    for (int i = 0; i < 8; ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

/* History runs off the LVGL task, so it can afford to wait for the short
 * storage-finalise/export window after TherapyStop.  One shared deadline
 * bounds both waits; a genuinely active therapy session still returns busy. */
static bool history_operation_cancelled(
    const touch_history_operation_t *operation)
{
    /* A real-time recording outranks all History I/O. This also covers web
     * readers with no UI cancellation callback: once the writer publishes a
     * pending claim, every bounded read loop unwinds and releases UPLOAD. */
    return sd_storage_recording_pending() ||
           (operation && operation->should_cancel &&
            operation->should_cancel(operation->context));
}

static void history_operation_progress(
    const touch_history_operation_t *operation, uint16_t per_mille)
{
    if (operation && operation->progress)
        operation->progress(operation->context,
                            per_mille > 1000U ? 1000U : per_mille);
}

static uint16_t history_progress_fraction(uint16_t start, uint16_t end,
                                          uint64_t numerator,
                                          uint64_t denominator)
{
    if (end <= start || !denominator) return start;
    if (numerator > denominator) numerator = denominator;
    return (uint16_t)(start +
        ((uint64_t)(end - start) * numerator) / denominator);
}

static void *history_alloc(size_t bytes, bool clear)
{
    void *value = clear
        ? heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* Large History transients must never consume the last internal DMA/BLE
     * heap merely because PSRAM is exhausted. */
    if (!value && bytes <= HISTORY_INTERNAL_FALLBACK_MAX_BYTES)
        value = clear ? calloc(1, bytes) : malloc(bytes);
    return value;
}

static void *history_grow_array(void *old, size_t old_capacity,
                                size_t new_capacity, size_t item_size)
{
    if (new_capacity <= old_capacity || item_size == 0 ||
        new_capacity > SIZE_MAX / item_size) return NULL;
    void *next = history_alloc(new_capacity * item_size, true);
    if (!next) return NULL;
    if (old && old_capacity) memcpy(next, old, old_capacity * item_size);
    free(old);
    return next;
}

static esp_err_t history_lease_acquire_operation(
    const touch_history_operation_t *operation)
{
    if (!sd_storage_is_ready()) return ESP_ERR_NOT_FOUND;

    const int64_t deadline_us = esp_timer_get_time() +
                                (int64_t)HISTORY_STORAGE_WAIT_MS * 1000;
    while (sd_storage_recording_active() && esp_timer_get_time() < deadline_us) {
        if (history_operation_cancelled(operation))
            return TOUCH_HISTORY_ERR_CANCELLED;
        vTaskDelay(pdMS_TO_TICKS(HISTORY_RECORDING_POLL_MS));
    }
    if (sd_storage_recording_active()) return ESP_ERR_INVALID_STATE;

    bool acquired = false;
    while (!acquired) {
        if (history_operation_cancelled(operation))
            return TOUCH_HISTORY_ERR_CANCELLED;
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) return ESP_ERR_TIMEOUT;
        uint32_t wait_ms = (uint32_t)((remaining_us + 999) / 1000);
        /* Poll the semaphore in short slices so a cancelled automatic load
         * does not remain hidden behind the full storage timeout. */
        if (operation && wait_ms > 100U) wait_ms = 100U;
        acquired = sd_storage_lease_acquire(SD_LEASE_UPLOAD, wait_ms);
    }

    /* A fresh session can start between the poll and lease acquisition. */
    if (sd_storage_recording_active()) {
        sd_storage_lease_release(SD_LEASE_UPLOAD);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t history_lease_acquire(void)
{
    return history_lease_acquire_operation(NULL);
}

static bool terminal_session(const char *state)
{
    return state && (!strcmp(state, "completed") ||
                     !strcmp(state, "interrupted") ||
                     !strcmp(state, "timed_out") ||
                     !strcmp(state, "rotated") ||
                     !strcmp(state, "split"));
}

/* Session manifests are generated locally and state/fmt are scalar fields.
 * Extracting those two values directly avoids allocating a cJSON tree from
 * the already tight internal heap once for every session in a night. */
static bool manifest_terminal_and_format(const char *json, int *format)
{
    const char *state_key = json ? strstr(json, "\"state\"") : NULL;
    const char *colon = state_key ? strchr(state_key + 7, ':') : NULL;
    const char *quote = colon ? strchr(colon + 1, '"') : NULL;
    if (!quote) return false;
    quote++;
    const char *end = strchr(quote, '"');
    if (!end || end == quote || (size_t)(end - quote) >= 24) return false;
    char state[24];
    memcpy(state, quote, (size_t)(end - quote));
    state[end - quote] = '\0';
    if (!terminal_session(state)) return false;

    *format = 1;
    const char *format_key = strstr(json, "\"fmt\"");
    colon = format_key ? strchr(format_key + 5, ':') : NULL;
    if (colon) {
        char *number_end = NULL;
        long parsed = strtol(colon + 1, &number_end, 10);
        if (number_end != colon + 1 && parsed >= 1 && parsed <= INT_MAX)
            *format = (int)parsed;
    }
    return true;
}

static bool json_string_equals(const char *json, const char *key,
                               const char *expected)
{
    char token[48];
    if (snprintf(token, sizeof(token), "\"%s\"", key) >= (int)sizeof(token))
        return false;
    size_t expected_len = strlen(expected);
    const char *found = json;
    while ((found = found ? strstr(found, token) : NULL) != NULL) {
        const char *colon = strchr(found + strlen(token), ':');
        const char *quote = colon ? strchr(colon + 1, '"') : NULL;
        if (!quote) return false;
        quote++;
        if (!strncmp(quote, expected, expected_len) &&
            quote[expected_len] == '"') return true;
        found += strlen(token);
    }
    return false;
}

static esp_err_t read_json_text(const char *path, char **out)
{
    if (!path || !out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    struct stat st;
    if (stat(path, &st) != 0) {
        return errno == ENOENT || errno == ENOTDIR
                   ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > OXIMETRY_CANONICAL_MAX_JSON_BYTES) return ESP_FAIL;
    FILE *file = fopen(path, "rb");
    /* stat already observed the file. Disappearance/open failure after that
     * is a raced or transient read, not proof that the package is absent. */
    if (!file) return ESP_FAIL;
    char *text = history_alloc((size_t)st.st_size + 1, false);
    if (!text) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(text, 1, (size_t)st.st_size, file);
    bool read_failed = got != (size_t)st.st_size || ferror(file);
    int close_result = fclose(file);
    if (read_failed || close_result != 0) {
        free(text);
        return ESP_FAIL;
    }
    text[got] = '\0';
    *out = text;
    return ESP_OK;
}

static esp_err_t read_ready_generation(const char *path, int *generation)
{
    char *text = NULL;
    esp_err_t result = read_json_text(path, &text);
    if (result != ESP_OK) return result;
    bool ready = json_string_equals(text, "schema",
                                    "somnotrace.oximetry.recording/1") &&
                 json_string_equals(text, "state", "ready");
    const char *key = ready ? strstr(text, "\"active_generation\"") : NULL;
    const char *colon = key ? strchr(key + 19, ':') : NULL;
    char *end = NULL;
    errno = 0;
    long parsed = colon ? strtol(colon + 1, &end, 10) : 0;
    free(text);
    if (!ready || !colon || end == colon + 1 || errno == ERANGE ||
        parsed <= 0 || parsed > 100000) return ESP_FAIL;
    *generation = (int)parsed;
    return ESP_OK;
}

static esp_err_t validate_generation_manifest(const char *path)
{
    char *text = NULL;
    esp_err_t result = read_json_text(path, &text);
    /* A ready pointer is a publication promise. A missing or partial active
     * generation is therefore a retryable/corrupt read, not genuine no-data. */
    if (result == ESP_ERR_NOT_FOUND) return ESP_FAIL;
    if (result != ESP_OK) return result;
    bool valid = json_string_equals(text, "schema",
                                    "somnotrace.oximetry.generation/1") &&
                 json_string_equals(text, "state", "ready") &&
                 json_string_equals(text, "path", "data/vitals.snt");
    free(text);
    return valid ? ESP_OK : ESP_FAIL;
}

static esp_err_t inspect_ox_trace_candidate(
    const char *path, trace_candidate_t *candidate,
    int64_t range_start_ms, int64_t range_end_ms,
    const touch_history_operation_t *operation)
{
    FILE *file = fopen(path, "rb");
    /* The ready pointer and generation manifest both name this track, so even
     * ENOENT here is an incomplete publication rather than an empty night. */
    if (!file) return ESP_FAIL;
    touch_ox_snt3_header_t header;
    bool header_read = fread(&header, sizeof(header), 1, file) == 1;
    bool valid = header_read &&
                 header.magic == OXIMETRY_CANONICAL_SNT_MAGIC &&
                 header.version == OXIMETRY_CANONICAL_SNT_VERSION &&
                 header.tier == 0 && header.timing == 0 &&
                 header.header_bytes == OXIMETRY_CANONICAL_SNT_HEADER_LEN &&
                 header.n_channels == OXIMETRY_CANONICAL_VITALS_CHANNELS &&
                 header.sample_bytes == sizeof(int16_t) &&
                 header.period_num_us > 0 && header.period_den > 0;
    if (!valid || fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    long size = ftell(file);
    if (size < header.header_bytes) {
        fclose(file);
        return ESP_FAIL;
    }
    uint32_t record_bytes = header.n_channels * sizeof(int16_t);
    uint64_t expected_bytes = (uint64_t)header.sample_count * record_bytes;
    if (header.data_bytes != expected_bytes ||
        (uint64_t)(size - header.header_bytes) != expected_bytes) {
        fclose(file);
        return ESP_FAIL;
    }
    uint32_t actual = header.sample_count;
    if (actual < 2) {
        fclose(file);
        return ESP_ERR_NOT_FOUND;
    }
    /* Bound elapsed time, not record count: canonical sources can have a
     * sub-second cadence, just like raw AirSense Flow. */
    if (!touch_history_sample_span_within(
            actual, header.period_num_us, header.period_den,
            HISTORY_AXIS_MAX_MS)) {
        ESP_LOGW(TAG,
                 "inspect O2 duration invalid path=%s count=%u period=%u/%u us",
                 path, (unsigned)actual, (unsigned)header.period_num_us,
                 (unsigned)header.period_den);
        fclose(file);
        return ESP_FAIL;
    }
    uint64_t duration_us = (uint64_t)actual * header.period_num_us /
                           header.period_den;
    if (header.start_epoch_ms < 946684800000LL || !duration_us ||
        duration_us > (uint64_t)HISTORY_AXIS_MAX_MS * 1000U ||
        duration_us / 1000U > (uint64_t)(INT64_MAX - header.start_epoch_ms)) {
        fclose(file);
        return ESP_FAIL;
    }
    if (fseek(file, header.header_bytes, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    int16_t *probe = history_alloc(HISTORY_READ_VALUES * sizeof(*probe), false);
    if (!probe) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    const size_t probe_records = HISTORY_READ_VALUES / header.n_channels;
    uint32_t checked = 0;
    uint32_t valid_spo2 = 0;
    uint32_t valid_pulse = 0;
    uint32_t valid_motion = 0;
    uint32_t crc = 0;
    bool probe_ok = true;
    while (checked < actual) {
        if (history_operation_cancelled(operation)) {
            free(probe);
            fclose(file);
            return TOUCH_HISTORY_ERR_CANCELLED;
        }
        size_t wanted = actual - checked;
        if (wanted > probe_records) wanted = probe_records;
        size_t got = fread(probe, header.n_channels * sizeof(int16_t),
                           wanted, file);
        crc = esp_rom_crc32_le(crc, (const uint8_t *)probe,
                               got * record_bytes);
        for (size_t i = 0; i < got; ++i) {
            uint32_t record_index = checked + (uint32_t)i;
            uint64_t offset_us = (uint64_t)record_index *
                                 header.period_num_us / header.period_den;
            int64_t timestamp_ms = header.start_epoch_ms +
                                   (int64_t)(offset_us / 1000U);
            if (range_end_ms > range_start_ms &&
                (timestamp_ms < range_start_ms ||
                 timestamp_ms >= range_end_ms)) continue;
            const int16_t *record = &probe[i * header.n_channels];
            int16_t value = record[OXIMETRY_CANONICAL_VITALS_SPO2];
            bool status_missing =
                ((uint16_t)record[OXIMETRY_CANONICAL_VITALS_STATUS] & 1U) != 0;
            if (!status_missing && value > 0 && value <= 10000)
                valid_spo2++;
            int16_t pulse = record[OXIMETRY_CANONICAL_VITALS_PULSE];
            bool pulse_missing =
                ((uint16_t)record[OXIMETRY_CANONICAL_VITALS_STATUS] & 2U) != 0;
            if (!pulse_missing && pulse > 0 && pulse <= 30000)
                valid_pulse++;
            if (record[OXIMETRY_CANONICAL_VITALS_MOTION_FLAGS] !=
                OXIMETRY_CANONICAL_SNT_MISSING)
                valid_motion++;
        }
        checked += (uint32_t)got;
        if (got != wanted || ferror(file)) {
            probe_ok = false;
            break;
        }
    }
    free(probe);
    if (fclose(file) != 0) probe_ok = false;
    if (!probe_ok || crc != header.data_crc32) return ESP_FAIL;
    if (valid_spo2 < 2 && valid_pulse < 2 && valid_motion < 2)
        return ESP_ERR_NOT_FOUND;
    strlcpy(candidate->path, path, sizeof(candidate->path));
    candidate->version = header.version;
    candidate->n_channels = header.n_channels;
    candidate->header_bytes = header.header_bytes;
    candidate->period_num_us = header.period_num_us;
    candidate->period_den = header.period_den;
    candidate->start_epoch_ms = header.start_epoch_ms;
    candidate->records = actual;
    candidate->valid_spo2_records = valid_spo2;
    candidate->valid_pulse_records = valid_pulse;
    candidate->valid_motion_records = valid_motion;
    candidate->valid_records = valid_spo2;
    return ESP_OK;
}

static uint64_t candidate_duration_us(const trace_candidate_t *candidate)
{
    if (candidate->period_num_us && candidate->period_den)
        return (uint64_t)candidate->records * candidate->period_num_us /
               candidate->period_den;
    return candidate->sample_hz_x10
               ? (uint64_t)candidate->records * 10000000U /
                     candidate->sample_hz_x10
               : 0;
}

static uint64_t candidate_valid_coverage_us(const trace_candidate_t *candidate)
{
    if (!candidate->period_num_us || !candidate->period_den) return 0;
    return (uint64_t)candidate->valid_records * candidate->period_num_us /
           candidate->period_den;
}

static void remember_discovery_error(esp_err_t *saved, esp_err_t error)
{
    if (!saved || error == ESP_OK || error == ESP_ERR_NOT_FOUND) return;
    if (*saved == ESP_OK || error == ESP_ERR_NO_MEM) *saved = error;
}

static uint32_t ox_candidate_valid_records(const trace_candidate_t *candidate,
                                           touch_history_signal_t signal)
{
    switch (signal) {
    case TOUCH_HISTORY_SIGNAL_SPO2:
        return candidate->valid_spo2_records;
    case TOUCH_HISTORY_SIGNAL_PULSE:
        return candidate->valid_pulse_records;
    case TOUCH_HISTORY_SIGNAL_MOTION:
        return candidate->valid_motion_records;
    case TOUCH_HISTORY_SIGNAL_COUNT: {
        uint32_t valid = candidate->valid_spo2_records;
        if (candidate->valid_pulse_records > valid)
            valid = candidate->valid_pulse_records;
        if (candidate->valid_motion_records > valid)
            valid = candidate->valid_motion_records;
        return valid;
    }
    default:
        return 0;
    }
}

static int ox_candidate_start_order(const void *left, const void *right)
{
    const trace_candidate_t *a = left;
    const trace_candidate_t *b = right;
    if (a->start_epoch_ms < b->start_epoch_ms) return -1;
    if (a->start_epoch_ms > b->start_epoch_ms) return 1;
    return strcmp(a->path, b->path);
}

static esp_err_t history_collect_ox_candidates(
    const char *day, touch_history_signal_t signal,
    int64_t range_start_ms, int64_t range_end_ms,
    bool retain_overlap_owners,
    trace_candidate_t **candidates_out, size_t *count_out,
    const touch_history_operation_t *operation)
{
    if (!candidates_out || !count_out) return ESP_ERR_INVALID_ARG;
    *candidates_out = NULL;
    *count_out = 0;
    if (signal != TOUCH_HISTORY_SIGNAL_COUNT &&
        signal != TOUCH_HISTORY_SIGNAL_SPO2 &&
        signal != TOUCH_HISTORY_SIGNAL_PULSE &&
        signal != TOUCH_HISTORY_SIGNAL_MOTION) return ESP_ERR_INVALID_ARG;
    ox_trace_find_t *ctx = history_alloc(sizeof(*ctx), true);
    if (!ctx) return ESP_ERR_NO_MEM;
    snprintf(ctx->day_path, sizeof(ctx->day_path),
             SD_OXYMETRY_DIR "/recordings/%s", day);
    DIR *dir = opendir(ctx->day_path);
    if (!dir) {
        int err = errno;
        free(ctx);
        return err == ENOENT || err == ENOTDIR ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    int read_err = 0;
    esp_err_t discovery_error = ESP_OK;
    trace_candidate_t *candidates = NULL;
    size_t candidate_count = 0;
    size_t candidate_capacity = 0;
    bool has_selected_data = false;
    for (;;) {
        if (history_operation_cancelled(operation)) {
            discovery_error = TOUCH_HISTORY_ERR_CANCELLED;
            break;
        }
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            read_err = errno;
            break;
        }
        if (entry->d_name[0] == '.') continue;
        if (snprintf(ctx->record_path, sizeof(ctx->record_path), "%s/%s",
                     ctx->day_path, entry->d_name) >=
            (int)sizeof(ctx->record_path)) {
            remember_discovery_error(&discovery_error, ESP_FAIL);
            continue;
        }
        struct stat st;
        if (stat(ctx->record_path, &st) != 0) {
            /* readdir already observed this entry; a failed follow-up stat
             * makes the scan incomplete even when the failure is ENOENT. */
            remember_discovery_error(&discovery_error, ESP_FAIL);
            continue;
        }
        if (!S_ISDIR(st.st_mode)) continue;
        if (snprintf(ctx->pointer_path, sizeof(ctx->pointer_path),
                     "%s/recording.json", ctx->record_path) >=
            (int)sizeof(ctx->pointer_path)) {
            remember_discovery_error(&discovery_error, ESP_FAIL);
            continue;
        }
        int generation = 0;
        esp_err_t pointer_result =
            read_ready_generation(ctx->pointer_path, &generation);
        /* Published recording directories must contain their ready pointer.
         * A night with no recording directories is genuine absence; an
         * observed directory with no pointer is an incomplete package. */
        if (pointer_result == ESP_ERR_NOT_FOUND) pointer_result = ESP_FAIL;
        if (pointer_result != ESP_OK) {
            remember_discovery_error(&discovery_error, pointer_result);
            continue;
        }
        if (snprintf(ctx->track_path, sizeof(ctx->track_path),
                     "%s/generations/%d/data/vitals.snt",
                     ctx->record_path, generation) >=
            (int)sizeof(ctx->track_path)) {
            remember_discovery_error(&discovery_error, ESP_FAIL);
            continue;
        }
        if (snprintf(ctx->manifest_path, sizeof(ctx->manifest_path),
                     "%s/generations/%d/manifest.json",
                     ctx->record_path, generation) >=
            (int)sizeof(ctx->manifest_path)) {
            remember_discovery_error(&discovery_error, ESP_FAIL);
            continue;
        }
        esp_err_t manifest_result =
            validate_generation_manifest(ctx->manifest_path);
        if (manifest_result != ESP_OK) {
            remember_discovery_error(&discovery_error, manifest_result);
            continue;
        }
        trace_candidate_t current = {0};
        esp_err_t inspect_result =
            inspect_ox_trace_candidate(ctx->track_path, &current,
                                       range_start_ms, range_end_ms,
                                       operation);
        if (inspect_result == ESP_ERR_NOT_FOUND) continue;
        if (inspect_result != ESP_OK) {
            remember_discovery_error(&discovery_error, inspect_result);
            continue;
        }
        current.valid_records = ox_candidate_valid_records(&current, signal);
        if (current.valid_records >= 2) {
            has_selected_data = true;
        } else if (!retain_overlap_owners) {
            continue;
        }
        if (candidate_count == candidate_capacity) {
            size_t next_capacity = candidate_capacity
                ? candidate_capacity * 2 : 4;
            trace_candidate_t *grown = history_grow_array(
                candidates, candidate_capacity, next_capacity,
                sizeof(*candidates));
            if (!grown) {
                remember_discovery_error(&discovery_error, ESP_ERR_NO_MEM);
                break;
            }
            candidates = grown;
            candidate_capacity = next_capacity;
        }
        candidates[candidate_count++] = current;
    }
    int close_err = closedir(dir) == 0 ? 0 : errno;
    free(ctx);
    if (read_err || close_err) {
        free(candidates);
        return ESP_FAIL;
    }
    /* Do not cache an incomplete directory scan as genuine no-data. The
     * canonical tree is published ready-only, so any non-absence discovery
     * failure means candidate selection may be incomplete and must retry. */
    if (discovery_error != ESP_OK) {
        free(candidates);
        return discovery_error;
    }
    if (!candidate_count) {
        free(candidates);
        return ESP_ERR_NOT_FOUND;
    }
    if (!has_selected_data) {
        free(candidates);
        return ESP_ERR_NOT_FOUND;
    }
    qsort(candidates, candidate_count, sizeof(*candidates),
          ox_candidate_start_order);
    *candidates_out = candidates;
    *count_out = candidate_count;
    return ESP_OK;
}

static esp_err_t find_ox_candidate(
    const char *day, touch_history_signal_t signal, trace_candidate_t *best,
    const touch_history_operation_t *operation)
{
    if (!best) return ESP_ERR_INVALID_ARG;
    trace_candidate_t *candidates = NULL;
    size_t count = 0;
    esp_err_t result = history_collect_ox_candidates(
        day, signal, 0, 0, false, &candidates, &count, operation);
    if (result != ESP_OK) return result;
    size_t selected = 0;
    for (size_t i = 1; i < count; ++i) {
        uint64_t coverage = candidate_valid_coverage_us(&candidates[i]);
        uint64_t best_coverage =
            candidate_valid_coverage_us(&candidates[selected]);
        uint64_t duration = candidate_duration_us(&candidates[i]);
        uint64_t best_duration = candidate_duration_us(&candidates[selected]);
        if (coverage > best_coverage ||
            (coverage == best_coverage && duration > best_duration) ||
            (coverage == best_coverage && duration == best_duration &&
             candidates[i].start_epoch_ms >
                 candidates[selected].start_epoch_ms)) selected = i;
    }
    *best = candidates[selected];
    free(candidates);
    return ESP_OK;
}

static esp_err_t find_spo2_candidate(const char *day, trace_candidate_t *best)
{
    return find_ox_candidate(day, TOUCH_HISTORY_SIGNAL_SPO2, best, NULL);
}

static esp_err_t history_ox_metadata(
    const char *day, int64_t *start_ms, int64_t *end_ms,
    uint16_t *available_signals,
    const touch_history_operation_t *operation)
{
    trace_candidate_t *candidates = NULL;
    size_t count = 0;
    esp_err_t result = history_collect_ox_candidates(
        day, TOUCH_HISTORY_SIGNAL_COUNT, 0, 0, false, &candidates, &count,
        operation);
    if (result != ESP_OK) return result;
    int64_t start = INT64_MAX;
    int64_t end = 0;
    uint16_t signals = 0;
    for (size_t i = 0; i < count; ++i) {
        const trace_candidate_t *candidate = &candidates[i];
        uint64_t duration_us = candidate_duration_us(candidate);
        if (!duration_us ||
            duration_us / 1000U >
                (uint64_t)(INT64_MAX - candidate->start_epoch_ms)) {
            result = ESP_FAIL;
            break;
        }
        int64_t candidate_end = candidate->start_epoch_ms +
                                (int64_t)(duration_us / 1000U);
        if (candidate->start_epoch_ms < start)
            start = candidate->start_epoch_ms;
        if (candidate_end > end) end = candidate_end;
        if (candidate->valid_spo2_records >= 2)
            signals |= TOUCH_HISTORY_SIGNAL_BIT(TOUCH_HISTORY_SIGNAL_SPO2);
        if (candidate->valid_pulse_records >= 2)
            signals |= TOUCH_HISTORY_SIGNAL_BIT(TOUCH_HISTORY_SIGNAL_PULSE);
        if (candidate->valid_motion_records >= 2)
            signals |= TOUCH_HISTORY_SIGNAL_BIT(TOUCH_HISTORY_SIGNAL_MOTION);
    }
    free(candidates);
    if (result != ESP_OK) return result;
    if (start == INT64_MAX || end <= start || end - start > HISTORY_AXIS_MAX_MS)
        return ESP_FAIL;
    if (start_ms) *start_ms = start;
    if (end_ms) *end_ms = end;
    if (available_signals) *available_signals = signals;
    return ESP_OK;
}

static esp_err_t inspect_trace_candidate(const char *path, uint8_t tier,
                                         uint8_t channels, uint16_t hz_x10,
                                         trace_candidate_t *candidate)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        int open_error = errno;
        if (open_error != ENOENT && open_error != ENOTDIR)
            ESP_LOGW(TAG, "inspect open failed path=%s errno=%d", path,
                     open_error);
        return open_error == ENOENT || open_error == ENOTDIR
                   ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    touch_snt_header_t header = {0};
    size_t header_items = fread(&header, sizeof(header), 1, file);
    if (header_items != 1) {
        ESP_LOGW(TAG, "inspect header read failed path=%s errno=%d", path,
                 errno);
        fclose(file);
        return ESP_FAIL;
    }
    bool valid = header.magic == SNT_MAGIC && header.version >= 1 &&
                 header.version <= 2 && header.tier == tier &&
                 header.n_channels == channels && header.sample_bytes == 2 &&
                 header.sample_hz_x10 == hz_x10 && hz_x10 > 0;
    if (!valid) {
        ESP_LOGW(TAG,
                 "inspect header invalid path=%s magic=%08lx v=%u tier=%u/%u channels=%u/%u bytes=%u hz10=%u/%u",
                 path, (unsigned long)header.magic, (unsigned)header.version,
                 (unsigned)header.tier, (unsigned)tier,
                 (unsigned)header.n_channels, (unsigned)channels,
                 (unsigned)header.sample_bytes,
                 (unsigned)header.sample_hz_x10, (unsigned)hz_x10);
        fclose(file);
        return ESP_FAIL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        ESP_LOGW(TAG, "inspect end seek failed path=%s errno=%d", path,
                 errno);
        fclose(file);
        return ESP_FAIL;
    }
    long size = ftell(file);
    int close_result = fclose(file);
    if (size < (long)sizeof(header)) {
        ESP_LOGW(TAG, "inspect size failed path=%s size=%ld errno=%d", path,
                 size, errno);
        return ESP_FAIL;
    }
    if (close_result != 0) {
        ESP_LOGW(TAG, "inspect close failed path=%s errno=%d", path, errno);
        return ESP_FAIL;
    }
    const uint32_t record_bytes = header.n_channels * sizeof(int16_t);
    const uint64_t body_bytes = (uint64_t)(size - (long)sizeof(header));
    if (body_bytes % record_bytes != 0 ||
        body_bytes / record_bytes != header.sample_count) {
        ESP_LOGW(TAG,
                 "inspect geometry invalid path=%s size=%ld body=%llu record=%u count=%u hz10=%u",
                 path, size, (unsigned long long)body_bytes,
                 (unsigned)record_bytes, (unsigned)header.sample_count,
                 (unsigned)header.sample_hz_x10);
        return ESP_FAIL;
    }
    uint32_t actual = header.sample_count;
    if (actual < 2) return ESP_ERR_NOT_FOUND;
    if (!touch_history_sample_span_within(
            actual, 10000000U, header.sample_hz_x10,
            HISTORY_AXIS_MAX_MS)) {
        ESP_LOGW(TAG,
                 "inspect duration invalid path=%s count=%u hz10=%u",
                 path, (unsigned)actual,
                 (unsigned)header.sample_hz_x10);
        return ESP_FAIL;
    }
    uint64_t duration_us = (uint64_t)actual * 10000000U /
                           header.sample_hz_x10;
    if (header.start_epoch_ms < 946684800000LL || !duration_us ||
        duration_us / 1000U >
            (uint64_t)(INT64_MAX - header.start_epoch_ms)) {
        ESP_LOGW(TAG,
                 "inspect timestamp invalid path=%s start=%lld duration_us=%llu",
                 path, (long long)header.start_epoch_ms,
                 (unsigned long long)duration_us);
        return ESP_FAIL;
    }
    strlcpy(candidate->path, path, sizeof(candidate->path));
    candidate->version = header.version;
    candidate->n_channels = header.n_channels;
    candidate->header_bytes = sizeof(header);
    candidate->sample_hz_x10 = header.sample_hz_x10;
    candidate->start_epoch_ms = header.start_epoch_ms;
    candidate->records = actual;
    return ESP_OK;
}

/* Pick the longest terminal session in the noon-day folder. A day summary can
 * contain several short mask-on periods; choosing one real session avoids
 * drawing a false continuous line across hours where nothing was recorded. */
static esp_err_t load_trace_leased(const char *day,
                                   touch_history_channel_t channel,
                                   touch_history_trace_t *trace)
{
    trace_candidate_t best = {0};
    if (channel == TOUCH_HISTORY_CHANNEL_SPO2) {
        esp_err_t find_result = find_spo2_candidate(day, &best);
        if (find_result != ESP_OK) return find_result;
        goto candidate_ready;
    }

    char day_path[320];
    snprintf(day_path, sizeof(day_path), "%s/%s", SD_STREAMS_DIR, day);
    DIR *dir = opendir(day_path);
    if (!dir) return errno == ENOENT || errno == ENOTDIR
                         ? ESP_ERR_NOT_FOUND : ESP_FAIL;

    const char *suffix = "_session.json";
    const size_t suffix_len = strlen(suffix);
    int read_err = 0;
    esp_err_t candidate_error = ESP_OK;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            read_err = errno;
            break;
        }
        size_t name_len = strlen(entry->d_name);
        if (name_len <= suffix_len ||
            strcmp(entry->d_name + name_len - suffix_len, suffix)) continue;

        char manifest_path[384];
        snprintf(manifest_path, sizeof(manifest_path), "%s/%s",
                 day_path, entry->d_name);
        FILE *manifest = fopen(manifest_path, "rb");
        if (!manifest) {
            /* The directory entry came from this same scan; failure to reopen
             * it is transient/incomplete and must remain retryable. */
            remember_discovery_error(&candidate_error, ESP_FAIL);
            continue;
        }
        if (fseek(manifest, 0, SEEK_END) != 0) {
            remember_discovery_error(&candidate_error, ESP_FAIL);
            fclose(manifest);
            continue;
        }
        long manifest_size = ftell(manifest);
        if (manifest_size <= 0 || manifest_size > 4096 ||
            fseek(manifest, 0, SEEK_SET) != 0) {
            remember_discovery_error(&candidate_error, ESP_FAIL);
            fclose(manifest);
            continue;
        }
        char *json_text = history_alloc((size_t)manifest_size + 1, false);
        if (!json_text) {
            fclose(manifest);
            remember_discovery_error(&candidate_error, ESP_ERR_NO_MEM);
            continue;
        }
        size_t got = fread(json_text, 1, (size_t)manifest_size, manifest);
        bool manifest_read_failed =
            got != (size_t)manifest_size || ferror(manifest);
        int manifest_close_result = fclose(manifest);
        if (manifest_read_failed || manifest_close_result != 0) {
            remember_discovery_error(&candidate_error, ESP_FAIL);
            free(json_text);
            continue;
        }
        json_text[got] = '\0';
        int fmt = 1;
        bool usable = manifest_terminal_and_format(json_text, &fmt);
        free(json_text);
        if (!usable) continue;

        size_t prefix_len = name_len - suffix_len;
        if (prefix_len == 0 || prefix_len >= 32) continue;
        char prefix[32];
        memcpy(prefix, entry->d_name, prefix_len);
        prefix[prefix_len] = '\0';
        char trace_path[384];
        uint8_t tier = 0;
        uint8_t channels = 0;
        uint16_t hz_x10 = 0;
        switch (channel) {
        case TOUCH_HISTORY_CHANNEL_FLOW:
            tier = 1;
            hz_x10 = 10;
            if (fmt >= 2) {
                snprintf(trace_path, sizeof(trace_path), "%s/%s_flow_mm.snt",
                         day_path, prefix);
                channels = 2;
            } else {
                snprintf(trace_path, sizeof(trace_path), "%s/%s_brp_mm.snt",
                         day_path, prefix);
                channels = 4;
            }
            break;
        case TOUCH_HISTORY_CHANNEL_LEAK:
            snprintf(trace_path, sizeof(trace_path), "%s/%s_pld.snt",
                     day_path, prefix);
            channels = 12;
            hz_x10 = 5;
            break;
        default:
            continue;
        }
        trace_candidate_t current = {0};
        esp_err_t inspect_result = inspect_trace_candidate(
            trace_path, tier, channels, hz_x10, &current);
        if (inspect_result == ESP_ERR_NOT_FOUND) continue;
        if (inspect_result != ESP_OK) {
            remember_discovery_error(&candidate_error, inspect_result);
            continue;
        }
        uint64_t duration = (uint64_t)current.records * 10000U /
                            current.sample_hz_x10;
        uint64_t best_duration = best.records
                                     ? (uint64_t)best.records * 10000U /
                                           best.sample_hz_x10
                                     : 0;
        if (duration > best_duration ||
            (duration == best_duration &&
             current.start_epoch_ms > best.start_epoch_ms)) {
            best = current;
        }
    }
    int close_err = closedir(dir) == 0 ? 0 : errno;
    if (read_err || close_err) return ESP_FAIL;
    if (candidate_error != ESP_OK) return candidate_error;
    if (!best.records) return ESP_ERR_NOT_FOUND;

candidate_ready:
    ;
    FILE *file = fopen(best.path, "rb");
    if (!file || fseek(file, best.header_bytes, SEEK_SET) != 0) {
        if (file) fclose(file);
        return ESP_FAIL;
    }
    trace_scratch_t *scratch = history_alloc(sizeof(*scratch), true);
    if (!scratch) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    /* Flow is an overnight envelope. Preserve both ends of every time bin as
     * parallel arrays; serialising low/high into one connected line would
     * fabricate a sawtooth that never occurred in the recording. */
    const size_t bins = TOUCH_HISTORY_TRACE_POINTS;
    trace_aggregate_t *aggregate = &scratch->aggregate;
    for (size_t i = 0; i < bins; ++i) {
        if (channel == TOUCH_HISTORY_CHANNEL_FLOW) {
            aggregate->flow.minimum[i] = INT16_MAX;
            aggregate->flow.maximum[i] = INT16_MIN;
        } else {
            aggregate->trend.extreme[i] =
                channel == TOUCH_HISTORY_CHANNEL_SPO2 ? INT16_MAX : INT16_MIN;
        }
        trace->points[i] = TOUCH_HISTORY_TRACE_MISSING;
        trace->upper_points[i] = TOUCH_HISTORY_TRACE_MISSING;
    }
    const int16_t missing = best.version >= 2 ? INT16_MIN : -1;
    /* Flat storage is deliberate. A [N][4] array combined with a 2-channel
     * fread advances rows by four int16s and silently skips every other v2
     * flow record (then reads uninitialised stack at the tail). */
    int16_t *records = scratch->records;
    const size_t records_per_read =
        HISTORY_READ_VALUES / best.n_channels;
    uint32_t processed = 0;
    bool read_ok = true;
    while (processed < best.records) {
        size_t wanted = best.records - processed;
        if (wanted > records_per_read) wanted = records_per_read;
        size_t got = fread(records, best.n_channels * sizeof(int16_t),
                           wanted, file);
        for (size_t r = 0; r < got; ++r) {
            uint32_t index = processed + (uint32_t)r;
            size_t bin = (size_t)(((uint64_t)index * bins) / best.records);
            if (bin >= bins) bin = bins - 1;
            const int16_t *record = &records[r * best.n_channels];
            if (channel == TOUCH_HISTORY_CHANNEL_FLOW) {
                for (size_t value_index = 0; value_index < 2; ++value_index) {
                    int16_t value = record[value_index];
                    if (value == missing) continue;
                    if (value < aggregate->flow.minimum[bin])
                        aggregate->flow.minimum[bin] = value;
                    if (value > aggregate->flow.maximum[bin])
                        aggregate->flow.maximum[bin] = value;
                    if (aggregate->flow.samples[bin] < UINT16_MAX)
                        aggregate->flow.samples[bin]++;
                }
            } else {
                size_t value_index = channel == TOUCH_HISTORY_CHANNEL_SPO2
                                         ? OXIMETRY_CANONICAL_VITALS_SPO2 : 3;
                int16_t value = record[value_index];
                /* Older capture paths wrote -1 when an optional PLD/SA2
                 * channel was absent even in v2 files. Neither SpO2 nor Leak
                 * has a valid negative physical value, so treat it as a gap. */
                bool invalid_spo2 = channel == TOUCH_HISTORY_CHANNEL_SPO2 &&
                    (value <= 0 || value > 10000 ||
                     (((uint16_t)record[OXIMETRY_CANONICAL_VITALS_STATUS] & 1U) != 0));
                if (value == missing || value < 0 || invalid_spo2) continue;
                if (channel == TOUCH_HISTORY_CHANNEL_SPO2) {
                    if (value < aggregate->trend.extreme[bin])
                        aggregate->trend.extreme[bin] = value;
                } else if (value > aggregate->trend.extreme[bin]) {
                    aggregate->trend.extreme[bin] = value;
                }
                if (aggregate->trend.samples[bin] < UINT16_MAX)
                    aggregate->trend.samples[bin]++;
            }
        }
        processed += (uint32_t)got;
        if (got != wanted) {
            read_ok = false;
            break;
        }
    }
    if (fclose(file) != 0) read_ok = false;

    if (!read_ok) {
        free(scratch);
        return ESP_FAIL;
    }

    unsigned populated = 0;
    for (size_t i = 0; i < bins; ++i) {
        uint16_t samples = channel == TOUCH_HISTORY_CHANNEL_FLOW
                               ? aggregate->flow.samples[i]
                               : aggregate->trend.samples[i];
        if (!samples) continue;
        if (channel == TOUCH_HISTORY_CHANNEL_FLOW) {
            int32_t low_lpm = (int32_t)lroundf(
                (float)aggregate->flow.minimum[i] * 0.6f);
            int32_t high_lpm = (int32_t)lroundf(
                (float)aggregate->flow.maximum[i] * 0.6f);
            if (low_lpm <= INT16_MIN) low_lpm = INT16_MIN + 1;
            if (high_lpm > INT16_MAX) high_lpm = INT16_MAX;
            trace->points[i] = (int16_t)low_lpm;
            trace->upper_points[i] = (int16_t)high_lpm;
            populated++;
        } else {
            float extreme = (float)aggregate->trend.extreme[i];
            /* Canonical SpO2 is percent x100; PLD Leak is L/s x100. Keep
             * desaturation nadirs and leak peaks instead of averaging away
             * the clinically useful excursions in each overnight bin. */
            int32_t physical = channel == TOUCH_HISTORY_CHANNEL_SPO2
                                   ? (int32_t)lroundf(extreme / 100.0f)
                                   : (int32_t)lroundf(extreme * 0.6f);
            if (physical > INT16_MAX) physical = INT16_MAX;
            trace->points[i] = (int16_t)physical;
            populated++;
        }
    }
    free(scratch);
    if (populated < 2) return ESP_ERR_NOT_FOUND;
    trace->count = TOUCH_HISTORY_TRACE_POINTS;
    trace->start_ms = best.start_epoch_ms;
    if (best.period_num_us && best.period_den) {
        trace->end_ms = best.start_epoch_ms +
            (int64_t)((uint64_t)processed * best.period_num_us /
                      best.period_den / 1000U);
    } else {
        trace->end_ms = best.start_epoch_ms +
            (int64_t)processed * 10000 / best.sample_hz_x10;
    }
    trace->has_data = true;
    return ESP_OK;
}

esp_err_t touch_history_load_trace(const char *day,
                                   touch_history_channel_t channel,
                                   touch_history_trace_t *trace)
{
    if (!trace || !valid_day(day) || channel < TOUCH_HISTORY_CHANNEL_FLOW ||
        channel >= TOUCH_HISTORY_CHANNEL_COUNT) return ESP_ERR_INVALID_ARG;
    memset(trace, 0, sizeof(*trace));
    trace->channel = channel;
    for (size_t i = 0; i < TOUCH_HISTORY_TRACE_POINTS; ++i) {
        trace->points[i] = TOUCH_HISTORY_TRACE_MISSING;
        trace->upper_points[i] = TOUCH_HISTORY_TRACE_MISSING;
    }
    esp_err_t lease_result = history_lease_acquire();
    if (lease_result != ESP_OK) return lease_result;
    esp_err_t result = load_trace_leased(day, channel, trace);
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    trace->loaded = result == ESP_OK || result == ESP_ERR_NOT_FOUND;
    return result;
}

/* -------------------------------------------------------------------------
 * Rev B full-night service.  The compatibility trace above deliberately
 * remains small until bsp_display_7b.c is migrated; the APIs below own the
 * complete card/night semantics and use caller-owned output buffers. */

#define HISTORY_EVENT_LINE_BYTES (16U * 1024U)
#define HISTORY_SUMMARY_MAX_BYTES (64U * 1024U)
#define HISTORY_INDEX_INITIAL_CAPACITY 32U
#define HISTORY_SESSION_INITIAL_CAPACITY 4U
#define HISTORY_EVENT_INITIAL_CAPACITY 32U

enum { HISTORY_MEM_SESSIONS = 1, HISTORY_MEM_EVENTS, HISTORY_MEM_VIEW,
       HISTORY_MEM_STATS, HISTORY_MEM_NIGHT, HISTORY_MEM_AXIS,
       HISTORY_MEM_FLOW_VERIFIED, HISTORY_MEM_FLOW_PREPARED, HISTORY_MEM_INTERVALS };
static history_cache_key_t history_cache_key(const char *day, uint16_t kind,
    touch_history_signal_t signal, int64_t start, int64_t end, bool filter)
{
    history_cache_key_t key = {.generation = sd_storage_content_generation(),
        .kind = kind, .signal = signal, .start_ms = start, .end_ms = end,
        .therapy_only = filter};
    strlcpy(key.day, day, sizeof(key.day));
    return key;
}

typedef struct {
    char id[TOUCH_HISTORY_SESSION_ID_LEN];
    int64_t start_ms;
    int64_t end_ms;
    int64_t clock_drift_ms;
    uint32_t brp_samples;
    uint32_t event_dropped;
    uint32_t pld_records;
    uint16_t available_signals;
    int fmt;
    bool partial;
    bool clock_drift_usable;
    bool end_estimated;
    bool has_epr_companion;
} history_session_info_t;

typedef struct {
    int64_t start_ms;
    int64_t end_ms;
} history_therapy_interval_t;

static esp_err_t history_collect_eligible_intervals_leased(
    const char *day, const history_session_info_t *sessions,
    size_t session_count, history_therapy_interval_t **intervals_out,
    size_t *interval_count_out, uint64_t *eligible_ms_out,
    const touch_history_operation_t *operation);
static bool history_interval_contains(
    const history_therapy_interval_t *intervals, size_t interval_count,
    int64_t timestamp_ms);

typedef struct {
    char day[9];
    int sessions;
    uint16_t skipped_sessions;
    bool has_therapy;
    bool has_oximetry;
} history_day_index_t;

typedef struct {
    int64_t sum[TOUCH_HISTORY_OVERVIEW_POINTS];
    int64_t companion_sum[TOUCH_HISTORY_OVERVIEW_POINTS];
    int16_t minimum[TOUCH_HISTORY_OVERVIEW_POINTS];
    int16_t maximum[TOUCH_HISTORY_OVERVIEW_POINTS];
    uint32_t count[TOUCH_HISTORY_OVERVIEW_POINTS];
    uint32_t companion_count[TOUCH_HISTORY_OVERVIEW_POINTS];
} history_overview_aggregate_t;

static const char *json_value_start(const char *json, const char *key)
{
    if (!json || !key) return NULL;
    char token[64];
    int length = snprintf(token, sizeof(token), "\"%s\"", key);
    if (length <= 0 || length >= (int)sizeof(token)) return NULL;
    const char *found = strstr(json, token);
    const char *colon = found ? strchr(found + length, ':') : NULL;
    if (!colon) return NULL;
    const char *value = colon + 1;
    while (*value == ' ' || *value == '\t' || *value == '\r' ||
           *value == '\n') value++;
    return value;
}

static bool json_text_string(const char *json, const char *key, char *out,
                             size_t out_size)
{
    const char *value = json_value_start(json, key);
    if (!value || *value != '"' || !out || out_size == 0) return false;
    value++;
    const char *end = strchr(value, '"');
    size_t length = end ? (size_t)(end - value) : SIZE_MAX;
    if (!end || length >= out_size) return false;
    memcpy(out, value, length);
    out[length] = '\0';
    return true;
}

static bool json_text_i64(const char *json, const char *key, int64_t *out)
{
    const char *value = json_value_start(json, key);
    if (!value || !out) return false;
    char *end = NULL;
    errno = 0;
    long long parsed = strtoll(value, &end, 10);
    if (end == value || errno == ERANGE) return false;
    *out = (int64_t)parsed;
    return true;
}

static bool json_text_bool(const char *json, const char *key, bool *out)
{
    const char *value = json_value_start(json, key);
    if (!value || !out) return false;
    if (!strncmp(value, "true", 4)) {
        *out = true;
        return true;
    }
    if (!strncmp(value, "false", 5)) {
        *out = false;
        return true;
    }
    return false;
}

static int history_session_start_order(const void *left, const void *right)
{
    const history_session_info_t *a = left;
    const history_session_info_t *b = right;
    if (a->start_ms < b->start_ms) return -1;
    if (a->start_ms > b->start_ms) return 1;
    return strcmp(a->id, b->id);
}

static esp_err_t parse_session_manifest(const char *json, const char *id,
                                        history_session_info_t *session,
                                        bool *is_terminal)
{
    if (!json || !id || !session || !is_terminal) return ESP_ERR_INVALID_ARG;
    *is_terminal = false;
    char state[24];
    if (!json_text_string(json, "state", state, sizeof(state))) return ESP_FAIL;
    if (!terminal_session(state)) return ESP_OK;

    int64_t start_ms = 0;
    if (!json_text_i64(json, "start_epoch_ms", &start_ms) ||
        start_ms < 946684800000LL) return ESP_FAIL;

    memset(session, 0, sizeof(*session));
    strlcpy(session->id, id, sizeof(session->id));
    session->start_ms = start_ms;
    (void)json_text_i64(json, "end_epoch_ms", &session->end_ms);
    (void)json_text_i64(json, "clock_drift_ms", &session->clock_drift_ms);
    int64_t numeric = 0;
    session->fmt = 1;
    if (json_text_i64(json, "fmt", &numeric) && numeric >= 1 &&
        numeric <= INT_MAX) session->fmt = (int)numeric;
    if (json_text_i64(json, "brp_samples", &numeric) && numeric >= 0 &&
        numeric <= UINT32_MAX) session->brp_samples = (uint32_t)numeric;
    if (json_text_i64(json, "event_dropped", &numeric) && numeric > 0 &&
        numeric <= UINT32_MAX) session->event_dropped = (uint32_t)numeric;
    bool drift_flag = false;
    if (json_text_bool(json, "clock_drift_usable", &drift_flag) ||
        json_text_bool(json, "clock_drift_valid", &drift_flag))
        session->clock_drift_usable = drift_flag;
    session->partial = strcmp(state, "completed") != 0;
    *is_terminal = true;
    return ESP_OK;
}

static esp_err_t history_load_sessions_uncached(
    const char *day, history_session_info_t **sessions_out, size_t *count_out,
    size_t *skipped_out,
    const touch_history_operation_t *operation)
{
    if (!valid_day(day) || !sessions_out || !count_out)
        return ESP_ERR_INVALID_ARG;
    *sessions_out = NULL;
    *count_out = 0;
    if (skipped_out) *skipped_out = 0;
    char day_path[320];
    if (snprintf(day_path, sizeof(day_path), "%s/%s", SD_STREAMS_DIR, day) >=
        (int)sizeof(day_path)) return ESP_FAIL;
    DIR *dir = opendir(day_path);
    if (!dir) return errno == ENOENT || errno == ENOTDIR
                         ? ESP_ERR_NOT_FOUND : ESP_FAIL;

    size_t count = 0;
    size_t capacity = 0;
    size_t skipped = 0;
    history_session_info_t *sessions = NULL;
    esp_err_t result = ESP_OK;
    const char *suffix = "_session.json";
    const size_t suffix_len = strlen(suffix);
    for (;;) {
        if (history_operation_cancelled(operation)) {
            result = TOUCH_HISTORY_ERR_CANCELLED;
            break;
        }
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            if (errno != 0) result = ESP_FAIL;
            break;
        }
        size_t name_len = strlen(entry->d_name);
        if (name_len <= suffix_len ||
            strcmp(entry->d_name + name_len - suffix_len, suffix)) continue;
        size_t id_len = name_len - suffix_len;
        if (id_len == 0 || id_len >= TOUCH_HISTORY_SESSION_ID_LEN) {
            ESP_LOGW(TAG, "skip malformed session filename day=%s name=%s",
                     day, entry->d_name);
            skipped++;
            continue;
        }
        char id[TOUCH_HISTORY_SESSION_ID_LEN];
        memcpy(id, entry->d_name, id_len);
        id[id_len] = '\0';
        char path[384];
        if (snprintf(path, sizeof(path), "%s/%s", day_path, entry->d_name) >=
            (int)sizeof(path)) {
            ESP_LOGW(TAG, "skip overlong session path day=%s id=%s", day, id);
            skipped++;
            continue;
        }
        char *json = NULL;
        result = read_json_text(path, &json);
        if (result != ESP_OK) {
            if (result == ESP_ERR_NO_MEM) break;
            ESP_LOGW(TAG, "skip unreadable session manifest day=%s id=%s: %s",
                     day, id, esp_err_to_name(result));
            skipped++;
            result = ESP_OK;
            continue;
        }
        history_session_info_t parsed;
        bool is_terminal = false;
        result = parse_session_manifest(json, id, &parsed, &is_terminal);
        free(json);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "skip malformed session manifest day=%s id=%s: %s",
                     day, id, esp_err_to_name(result));
            skipped++;
            result = ESP_OK;
            continue;
        }
        if (!is_terminal) continue;
        if (count == capacity) {
            size_t next_capacity = capacity ? capacity * 2
                                            : HISTORY_SESSION_INITIAL_CAPACITY;
            history_session_info_t *grown = history_grow_array(
                sessions, capacity, next_capacity, sizeof(*sessions));
            if (!grown) {
                result = ESP_ERR_NO_MEM;
                break;
            }
            sessions = grown;
            capacity = next_capacity;
        }
        sessions[count++] = parsed;
    }
    int close_error = closedir(dir) == 0 ? 0 : errno;
    if (close_error && result == ESP_OK) result = ESP_FAIL;
    if (result != ESP_OK) {
        free(sessions);
        return result;
    }
    if (!count) {
        free(sessions);
        return ESP_ERR_NOT_FOUND;
    }
    if (skipped)
        ESP_LOGW(TAG, "day=%s loaded %u sessions; skipped %u bad manifests",
                 day, (unsigned)count, (unsigned)skipped);
    if (skipped_out) *skipped_out = skipped;
    qsort(sessions, count, sizeof(*sessions), history_session_start_order);
    *sessions_out = sessions;
    *count_out = count;
    return ESP_OK;
}

static esp_err_t history_load_sessions_leased(
    const char *day, history_session_info_t **sessions_out, size_t *count_out,
    size_t *skipped_out, const touch_history_operation_t *operation)
{
    if (history_operation_cancelled(operation)) return TOUCH_HISTORY_ERR_CANCELLED;
    history_cache_key_t key = history_cache_key(day, HISTORY_MEM_SESSIONS, 0, 0, 0, false);
    typedef struct { size_t count, skipped; history_session_info_t items[]; } cached_t;
    size_t bytes = history_cache_get(&key, NULL, 0);
    cached_t *cached = bytes ? history_alloc(bytes, true) : NULL;
    if (cached && history_cache_get(&key, cached, bytes)) {
        history_session_info_t *items = history_alloc(cached->count * sizeof(*items), true);
        if (items) {
            memcpy(items, cached->items, cached->count * sizeof(*items));
            *sessions_out = items; *count_out = cached->count;
            if (skipped_out) *skipped_out = cached->skipped;
            free(cached);
            return ESP_OK;
        }
    }
    free(cached);
    size_t skipped = 0;
    esp_err_t result = history_load_sessions_uncached(day, sessions_out, count_out,
                                                      &skipped, operation);
    if (skipped_out) *skipped_out = skipped;
    if (result == ESP_OK && *count_out <= 64 && !history_operation_cancelled(operation)) {
        bytes = sizeof(cached_t) + *count_out * sizeof(**sessions_out);
        cached = history_alloc(bytes, true);
        if (cached) {
            cached->count = *count_out; cached->skipped = skipped;
            memcpy(cached->items, *sessions_out, *count_out * sizeof(**sessions_out));
            history_cache_put(&key, cached, bytes);
            free(cached);
        }
    }
    return result;
}

static esp_err_t history_session_candidate(const char *day,
                                           const history_session_info_t *session,
                                           touch_history_signal_t signal,
                                           trace_candidate_t *candidate)
{
    if (!day || !session || !candidate) return ESP_ERR_INVALID_ARG;
    char path[384];
    uint8_t tier = 0;
    uint8_t channels = 0;
    uint16_t hz_x10 = 0;
    const char *suffix = NULL;
    if (signal == TOUCH_HISTORY_SIGNAL_FLOW) {
        tier = 1;
        hz_x10 = 10;
        suffix = session->fmt >= 2 ? "flow_mm" : "brp_mm";
        channels = session->fmt >= 2 ? 2 : 4;
    } else if (signal >= TOUCH_HISTORY_SIGNAL_PRESSURE &&
               signal <= TOUCH_HISTORY_SIGNAL_SNORE) {
        suffix = "pld";
        channels = 12;
        hz_x10 = 5;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(path, sizeof(path), "%s/%s/%s_%s.snt", SD_STREAMS_DIR,
                 day, session->id, suffix) >= (int)sizeof(path)) return ESP_FAIL;
    memset(candidate, 0, sizeof(*candidate));
    return inspect_trace_candidate(path, tier, channels, hz_x10, candidate);
}

static esp_err_t history_flow_raw_candidate(
    const char *day, const history_session_info_t *session,
    trace_candidate_t *candidate)
{
    if (!day || !session || !candidate) return ESP_ERR_INVALID_ARG;
    char path[384];
    const char *suffix = session->fmt >= 2 ? "flow" : "brp";
    uint8_t channels = session->fmt >= 2 ? 1 : 4;
    if (snprintf(path, sizeof(path), "%s/%s/%s_%s.snt", SD_STREAMS_DIR,
                 day, session->id, suffix) >= (int)sizeof(path)) return ESP_FAIL;
    memset(candidate, 0, sizeof(*candidate));
    return inspect_trace_candidate(path, 0, channels, 250, candidate);
}

static int64_t history_candidate_end_ms(const trace_candidate_t *candidate);

static bool history_candidate_overlaps(const trace_candidate_t *candidate,
                                       int64_t start_ms, int64_t end_ms)
{
    if (!candidate) return false;
    int64_t candidate_end = history_candidate_end_ms(candidate);
    return candidate_end > start_ms &&
           candidate->start_epoch_ms < end_ms;
}

static esp_err_t history_select_flow_range_source(
    const char *day, const history_session_info_t *sessions,
    size_t session_count, int64_t start_ms, int64_t end_ms,
    bool prefer_raw, bool *use_raw, bool *used_fallback,
    const touch_history_operation_t *operation)
{
    if (!day || (session_count && !sessions) || !use_raw || !used_fallback)
        return ESP_ERR_INVALID_ARG;
    *use_raw = false;
    *used_fallback = false;
    if (!prefer_raw) return ESP_OK;

    bool have_raw = false;
    bool need_sidecar = false;
    for (size_t i = 0; i < session_count; ++i) {
        if (history_operation_cancelled(operation))
            return TOUCH_HISTORY_ERR_CANCELLED;
        if (sessions[i].end_ms <= start_ms || sessions[i].start_ms >= end_ms)
            continue;
        trace_candidate_t raw = {0};
        esp_err_t result = history_flow_raw_candidate(day, &sessions[i], &raw);
        if (result == ESP_OK &&
            history_candidate_overlaps(&raw, start_ms, end_ms)) {
            have_raw = true;
            continue;
        }
        if (result == ESP_ERR_NO_MEM || result == TOUCH_HISTORY_ERR_CANCELLED)
            return result;
        if (result != ESP_OK && result != ESP_ERR_NOT_FOUND)
            ESP_LOGW(TAG, "ignore corrupt raw Flow day=%s session=%s: %s",
                     day, sessions[i].id, esp_err_to_name(result));

        trace_candidate_t sidecar = {0};
        result = history_session_candidate(
            day, &sessions[i], TOUCH_HISTORY_SIGNAL_FLOW, &sidecar);
        if (result == ESP_OK &&
            history_candidate_overlaps(&sidecar, start_ms, end_ms)) {
            need_sidecar = true;
            continue;
        }
        if (result == ESP_ERR_NO_MEM || result == TOUCH_HISTORY_ERR_CANCELLED)
            return result;
        if (result != ESP_OK && result != ESP_ERR_NOT_FOUND)
            ESP_LOGW(TAG, "ignore corrupt Flow sidecar day=%s session=%s: %s",
                     day, sessions[i].id, esp_err_to_name(result));
    }
    *used_fallback = need_sidecar;
    *use_raw = !need_sidecar && (have_raw || session_count > 0);
    return ESP_OK;
}

static int64_t history_candidate_end_ms(const trace_candidate_t *candidate)
{
    uint64_t duration_us = candidate_duration_us(candidate);
    if (!duration_us || duration_us / 1000U > INT64_MAX) return 0;
    return candidate->start_epoch_ms + (int64_t)(duration_us / 1000U);
}

static esp_err_t history_resolve_session_ends(
    const char *day, history_session_info_t *sessions, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        history_session_info_t *session = &sessions[i];
        if (session->end_ms <= session->start_ms) {
            trace_candidate_t candidate = {0};
            esp_err_t result = history_session_candidate(
                day, session, TOUCH_HISTORY_SIGNAL_PRESSURE, &candidate);
            if (result == ESP_ERR_NOT_FOUND)
                result = history_session_candidate(
                    day, session, TOUCH_HISTORY_SIGNAL_FLOW, &candidate);
            if (result == ESP_OK) {
                session->end_ms = history_candidate_end_ms(&candidate);
                session->end_estimated = true;
            } else if (result != ESP_ERR_NOT_FOUND) {
                if (result == ESP_ERR_NO_MEM ||
                    result == TOUCH_HISTORY_ERR_CANCELLED) return result;
                ESP_LOGW(TAG,
                         "ignore corrupt end source day=%s session=%s: %s",
                         day, session->id, esp_err_to_name(result));
                session->end_ms = session->start_ms;
            }
        }
        if (session->end_ms < session->start_ms ||
            session->end_ms - session->start_ms > HISTORY_AXIS_MAX_MS) {
            ESP_LOGW(TAG, "ignore invalid session extent day=%s session=%s",
                     day, session->id);
            session->end_ms = session->start_ms;
        }
    }
    return ESP_OK;
}

static bool history_axis(const history_session_info_t *sessions, size_t count,
                         int64_t *start_ms, int64_t *end_ms)
{
    if (!sessions || !count || !start_ms || !end_ms) return false;
    int64_t start = INT64_MAX;
    int64_t end = 0;
    for (size_t i = 0; i < count; ++i) {
        if (sessions[i].end_ms <= sessions[i].start_ms) continue;
        if (sessions[i].start_ms < start) start = sessions[i].start_ms;
        if (sessions[i].end_ms > end) end = sessions[i].end_ms;
    }
    if (start == INT64_MAX || end <= start || end - start > HISTORY_AXIS_MAX_MS)
        return false;
    *start_ms = start;
    *end_ms = end;
    return true;
}

static esp_err_t history_unified_axis_uncached(
    const char *day, const history_session_info_t *sessions,
    size_t session_count, int64_t *start_ms, int64_t *end_ms,
    uint16_t *oximetry_signals, bool *oximetry_error,
    const touch_history_operation_t *operation)
{
    if (!day || !start_ms || !end_ms ||
        (session_count && !sessions)) return ESP_ERR_INVALID_ARG;
    bool have_axis = history_axis(sessions, session_count, start_ms, end_ms);
    if (oximetry_error) *oximetry_error = false;
    int64_t ox_start = 0;
    int64_t ox_end = 0;
    uint16_t ox_signals = 0;
    esp_err_t ox_result = history_ox_metadata(
        day, &ox_start, &ox_end, &ox_signals, operation);
    if (ox_result != ESP_OK && ox_result != ESP_ERR_NOT_FOUND) {
        if (ox_result == ESP_ERR_NO_MEM ||
            ox_result == TOUCH_HISTORY_ERR_CANCELLED || !have_axis)
            return ox_result;
        ESP_LOGW(TAG, "ignore unreadable O2 metadata day=%s: %s", day,
                 esp_err_to_name(ox_result));
        if (oximetry_error) *oximetry_error = true;
        ox_result = ESP_ERR_NOT_FOUND;
    }
    if (ox_result == ESP_OK) {
        if (!have_axis) {
            *start_ms = ox_start;
            *end_ms = ox_end;
            have_axis = true;
        } else {
            if (ox_start < *start_ms) *start_ms = ox_start;
            if (ox_end > *end_ms) *end_ms = ox_end;
        }
    }
    if (!have_axis) return ESP_ERR_NOT_FOUND;
    if (*end_ms <= *start_ms || *end_ms - *start_ms > HISTORY_AXIS_MAX_MS)
        return ESP_FAIL;
    if (oximetry_signals) *oximetry_signals = ox_signals;
    return ESP_OK;
}

static esp_err_t history_unified_axis(
    const char *day, const history_session_info_t *sessions,
    size_t session_count, int64_t *start_ms, int64_t *end_ms,
    uint16_t *oximetry_signals, bool *oximetry_error,
    const touch_history_operation_t *operation)
{
    if (history_operation_cancelled(operation)) return TOUCH_HISTORY_ERR_CANCELLED;
    history_cache_key_t key = history_cache_key(day, HISTORY_MEM_AXIS, 0, 0, 0, false);
    struct { int64_t start, end; uint16_t signals; bool error; } cached = {0};
    esp_err_t result = ESP_OK;
    if (!history_cache_get(&key, &cached, sizeof(cached))) {
        result = history_unified_axis_uncached(day, sessions, session_count,
            &cached.start, &cached.end, &cached.signals, &cached.error, operation);
        if (result == ESP_OK && !history_operation_cancelled(operation))
            history_cache_put(&key, &cached, sizeof(cached));
    }
    if (result == ESP_OK) {
        *start_ms = cached.start; *end_ms = cached.end;
        if (oximetry_signals) *oximetry_signals = cached.signals;
        if (oximetry_error) *oximetry_error = cached.error;
    }
    return result;
}

static bool history_bin_overlaps_session(const history_session_info_t *sessions,
                                         size_t count, int64_t start_ms,
                                         int64_t end_ms)
{
    for (size_t i = 0; i < count; ++i) {
        if (sessions[i].end_ms <= start_ms) continue;
        if (sessions[i].start_ms >= end_ms) break;
        return true;
    }
    return false;
}

static bool history_raw_value_valid(int16_t value, uint8_t version,
                                    bool signed_signal)
{
    int16_t missing = version >= 2 ? INT16_MIN : -1;
    if (value == missing) return false;
    /* Early capture paths retained -1 for absent optional PLD fields even in
     * v2.  Flow is signed, while every other source used here is not. */
    return signed_signal || value >= 0;
}

static bool history_scale_x100(touch_history_signal_t signal, int16_t raw,
                               int16_t *scaled)
{
    return touch_history_scale_source_x100(signal, raw, scaled);
}

static esp_err_t history_probe_session(const char *day,
                                       history_session_info_t *session,
                                       const touch_history_operation_t *operation)
{
    if (history_operation_cancelled(operation)) return TOUCH_HISTORY_ERR_CANCELLED;
    int16_t *records = history_alloc(HISTORY_READ_VALUES * sizeof(*records),
                                     false);
    if (!records) return ESP_ERR_NO_MEM;
    esp_err_t result = ESP_OK;

    trace_candidate_t flow = {0};
    esp_err_t inspect = history_session_candidate(
        day, session, TOUCH_HISTORY_SIGNAL_FLOW, &flow);
    if (history_operation_cancelled(operation)) {
        free(records);
        return TOUCH_HISTORY_ERR_CANCELLED;
    }
    if (inspect == ESP_OK) {
        FILE *file = fopen(flow.path, "rb");
        if (!file || fseek(file, flow.header_bytes, SEEK_SET) != 0) {
            if (file) fclose(file);
            free(records);
            return ESP_FAIL;
        }
        size_t per_read = HISTORY_READ_VALUES / flow.n_channels;
        uint32_t processed = 0;
        bool have_flow = false;
        while (processed < flow.records) {
            if (history_operation_cancelled(operation)) {
                result = TOUCH_HISTORY_ERR_CANCELLED;
                break;
            }
            size_t wanted = flow.records - processed;
            if (wanted > per_read) wanted = per_read;
            size_t got = fread(records, flow.n_channels * sizeof(int16_t),
                               wanted, file);
            for (size_t i = 0; i < got; ++i) {
                int16_t *record = &records[i * flow.n_channels];
                if (history_raw_value_valid(record[0], flow.version, true) ||
                    history_raw_value_valid(record[1], flow.version, true))
                    have_flow = true;
            }
            processed += (uint32_t)got;
            if (got != wanted || ferror(file)) {
                result = ESP_FAIL;
                break;
            }
        }
        if (fclose(file) != 0 && result == ESP_OK) result = ESP_FAIL;
        if (result == ESP_OK && have_flow)
            session->available_signals |=
                TOUCH_HISTORY_SIGNAL_BIT(TOUCH_HISTORY_SIGNAL_FLOW);
    } else if (inspect != ESP_ERR_NOT_FOUND) {
        result = inspect;
    }

    trace_candidate_t pld = {0};
    if (history_operation_cancelled(operation)) result = TOUCH_HISTORY_ERR_CANCELLED;
    if (result == ESP_OK) inspect = history_session_candidate(
        day, session, TOUCH_HISTORY_SIGNAL_PRESSURE, &pld);
    if (history_operation_cancelled(operation)) result = TOUCH_HISTORY_ERR_CANCELLED;
    if (result == ESP_OK && inspect == ESP_OK) {
        FILE *file = fopen(pld.path, "rb");
        if (!file || fseek(file, pld.header_bytes, SEEK_SET) != 0) {
            if (file) fclose(file);
            free(records);
            return ESP_FAIL;
        }
        session->pld_records = pld.records;
        size_t per_read = HISTORY_READ_VALUES / pld.n_channels;
        uint32_t processed = 0;
        while (processed < pld.records) {
            if (history_operation_cancelled(operation)) {
                result = TOUCH_HISTORY_ERR_CANCELLED;
                break;
            }
            size_t wanted = pld.records - processed;
            if (wanted > per_read) wanted = per_read;
            size_t got = fread(records, pld.n_channels * sizeof(int16_t),
                               wanted, file);
            for (size_t i = 0; i < got; ++i) {
                int16_t *record = &records[i * pld.n_channels];
                if (history_raw_value_valid(record[1], pld.version, false))
                    session->available_signals |= TOUCH_HISTORY_SIGNAL_BIT(
                        TOUCH_HISTORY_SIGNAL_PRESSURE);
                if (history_raw_value_valid(record[2], pld.version, false))
                    session->has_epr_companion = true;
                if (history_raw_value_valid(record[3], pld.version, false))
                    session->available_signals |= TOUCH_HISTORY_SIGNAL_BIT(
                        TOUCH_HISTORY_SIGNAL_LEAK);
                if (history_raw_value_valid(record[10], pld.version, false))
                    session->available_signals |= TOUCH_HISTORY_SIGNAL_BIT(
                        TOUCH_HISTORY_SIGNAL_FLOW_LIMIT);
                if (history_raw_value_valid(record[9], pld.version, false))
                    session->available_signals |= TOUCH_HISTORY_SIGNAL_BIT(
                        TOUCH_HISTORY_SIGNAL_SNORE);
            }
            processed += (uint32_t)got;
            if (got != wanted || ferror(file)) {
                result = ESP_FAIL;
                break;
            }
        }
        if (fclose(file) != 0 && result == ESP_OK) result = ESP_FAIL;
    } else if (result == ESP_OK && inspect != ESP_ERR_NOT_FOUND) {
        result = inspect;
    }
    if (history_operation_cancelled(operation)) result = TOUCH_HISTORY_ERR_CANCELLED;
    free(records);
    return result;
}

static touch_history_aggregation_t history_aggregation_for_signal(
    touch_history_signal_t signal)
{
    switch (signal) {
    case TOUCH_HISTORY_SIGNAL_FLOW:
        return TOUCH_HISTORY_AGGREGATION_ENVELOPE;
    case TOUCH_HISTORY_SIGNAL_SPO2:
        return TOUCH_HISTORY_AGGREGATION_MINIMUM;
    case TOUCH_HISTORY_SIGNAL_LEAK:
    case TOUCH_HISTORY_SIGNAL_FLOW_LIMIT:
    case TOUCH_HISTORY_SIGNAL_SNORE:
    case TOUCH_HISTORY_SIGNAL_MOTION:
        return TOUCH_HISTORY_AGGREGATION_MAXIMUM;
    default:
        return TOUCH_HISTORY_AGGREGATION_MEAN;
    }
}

static int history_overview_bin_count(int64_t axis_start_ms,
                                      int64_t axis_end_ms,
                                      int64_t timestamp_ms,
                                      uint16_t point_count)
{
    if (!point_count || point_count > TOUCH_HISTORY_OVERVIEW_POINTS ||
        axis_end_ms <= axis_start_ms || timestamp_ms < axis_start_ms ||
        timestamp_ms >= axis_end_ms) return -1;
    uint64_t offset = (uint64_t)(timestamp_ms - axis_start_ms);
    uint64_t span = (uint64_t)(axis_end_ms - axis_start_ms);
    if (offset > UINT64_MAX / point_count) return -1;
    int bin = (int)(offset * point_count / span);
    return bin < point_count ? bin : point_count - 1;
}

static void history_overview_prepare(
    touch_history_overview_t *overview, touch_history_signal_t signal,
    int64_t axis_start_ms, int64_t axis_end_ms,
    const history_session_info_t *sessions, size_t session_count,
    uint16_t point_count, history_overview_aggregate_t *aggregate)
{
    memset(overview, 0, sizeof(*overview));
    overview->signal = signal;
    overview->aggregation = history_aggregation_for_signal(signal);
    overview->axis_start_ms = axis_start_ms;
    overview->axis_end_ms = axis_end_ms;
    overview->point_count = point_count;
    uint64_t span = (uint64_t)(axis_end_ms - axis_start_ms);
    overview->bin_width_ms = (uint32_t)((span + point_count - 1) /
                                        point_count);
    for (size_t i = 0; i < TOUCH_HISTORY_OVERVIEW_POINTS; ++i) {
        overview->value_x100[i] = TOUCH_HISTORY_VALUE_MISSING;
        overview->upper_x100[i] = TOUCH_HISTORY_VALUE_MISSING;
        overview->companion_x100[i] = TOUCH_HISTORY_VALUE_MISSING;
        aggregate->minimum[i] = INT16_MAX;
        aggregate->maximum[i] = INT16_MIN;
        if (i >= point_count) continue;
        overview->timestamp_ms[i] = axis_start_ms + (int64_t)(
            ((uint64_t)(2 * i + 1) * span) /
            (2U * point_count));
        uint64_t bin_start_offset = (uint64_t)i * span /
                                    point_count;
        uint64_t bin_end_offset = (uint64_t)(i + 1) * span /
                                  point_count;
        if (history_bin_overlaps_session(
                sessions, session_count,
                axis_start_ms + (int64_t)bin_start_offset,
                axis_start_ms + (int64_t)bin_end_offset))
            overview->flags[i] |= TOUCH_HISTORY_POINT_THERAPY;
    }
}

static void history_aggregate_value(history_overview_aggregate_t *aggregate,
                                    size_t bin, int16_t value)
{
    if (value < aggregate->minimum[bin]) aggregate->minimum[bin] = value;
    if (value > aggregate->maximum[bin]) aggregate->maximum[bin] = value;
    aggregate->sum[bin] += value;
    if (aggregate->count[bin] < UINT32_MAX) aggregate->count[bin]++;
}

static int16_t history_mean_x100(int64_t sum, uint32_t count)
{
    if (!count) return TOUCH_HISTORY_VALUE_MISSING;
    int64_t rounded = sum >= 0 ? sum + count / 2 : sum - count / 2;
    int64_t value = rounded / (int64_t)count;
    if (value <= INT16_MIN) value = INT16_MIN + 1;
    if (value > INT16_MAX) value = INT16_MAX;
    return (int16_t)value;
}

static void history_overview_finalize(touch_history_overview_t *overview,
                                      history_overview_aggregate_t *aggregate)
{
    for (size_t i = 0; i < overview->point_count; ++i) {
        uint32_t count = aggregate->count[i];
        if (!count) continue;
        switch (overview->aggregation) {
        case TOUCH_HISTORY_AGGREGATION_ENVELOPE:
            overview->value_x100[i] = aggregate->minimum[i];
            overview->upper_x100[i] = aggregate->maximum[i];
            overview->flags[i] |= TOUCH_HISTORY_POINT_UPPER_VALID;
            break;
        case TOUCH_HISTORY_AGGREGATION_MINIMUM:
            overview->value_x100[i] = aggregate->minimum[i];
            break;
        case TOUCH_HISTORY_AGGREGATION_MAXIMUM:
            overview->value_x100[i] = aggregate->maximum[i];
            break;
        case TOUCH_HISTORY_AGGREGATION_MEAN:
            overview->value_x100[i] = history_mean_x100(
                aggregate->sum[i], count);
            break;
        }
        overview->sample_count[i] = count > UINT16_MAX ? UINT16_MAX
                                                       : (uint16_t)count;
        overview->flags[i] |= TOUCH_HISTORY_POINT_VALID;
        if (aggregate->companion_count[i]) {
            overview->companion_x100[i] = history_mean_x100(
                aggregate->companion_sum[i],
                aggregate->companion_count[i]);
            overview->flags[i] |= TOUCH_HISTORY_POINT_COMPANION_VALID;
            overview->has_companion = true;
        }
        overview->has_data = true;
    }
}

static int history_pld_channel(touch_history_signal_t signal)
{
    switch (signal) {
    case TOUCH_HISTORY_SIGNAL_PRESSURE: return 1;
    case TOUCH_HISTORY_SIGNAL_LEAK: return 3;
    case TOUCH_HISTORY_SIGNAL_FLOW_LIMIT: return 10;
    case TOUCH_HISTORY_SIGNAL_SNORE: return 9;
    default: return -1;
    }
}

static uint32_t history_as11_record_at_or_after(
    const trace_candidate_t *candidate, int64_t timestamp_ms)
{
    if (!candidate || !candidate->sample_hz_x10 ||
        timestamp_ms <= candidate->start_epoch_ms) return 0;
    uint64_t delta_ms = (uint64_t)(timestamp_ms -
                                   candidate->start_epoch_ms);
    uint64_t numerator = delta_ms * candidate->sample_hz_x10;
    uint64_t index = (numerator + 9999U) / 10000U;
    return index > candidate->records ? candidate->records : (uint32_t)index;
}

static bool history_flow_cancel(void *context);
static bool history_accumulate_flow_pyramid(
    const char *day, const trace_candidate_t *candidate, size_t session_index,
    int64_t start, int64_t end, history_overview_aggregate_t *aggregate,
    touch_history_overview_t *overview, const touch_history_operation_t *operation)
{
    history_flow_bin_t *bins = history_alloc(overview->point_count * sizeof(*bins), true);
    if (!bins) return false;
    history_cache_key_t key = history_cache_key(day, HISTORY_MEM_FLOW_VERIFIED,
        0, candidate->start_epoch_ms, session_index, false);
    bool verified = false;
    (void)history_cache_get(&key, &verified, sizeof(verified));
    int read = history_flow_cache_read(candidate->path, start, end,
        overview->point_count, bins, verified, history_flow_cancel,
        (void *)operation, NULL);
    if (read == 0) {
        bool contributed = false;
        for (size_t b = 0; b < overview->point_count; ++b) {
            if (!bins[b].count) continue;
            int16_t low, high;
            if (!history_scale_x100(TOUCH_HISTORY_SIGNAL_FLOW, bins[b].low, &low) ||
                !history_scale_x100(TOUCH_HISTORY_SIGNAL_FLOW, bins[b].high, &high)) continue;
            if (low < aggregate->minimum[b]) aggregate->minimum[b] = low;
            if (high > aggregate->maximum[b]) aggregate->maximum[b] = high;
            aggregate->count[b] += bins[b].count;
            overview->valid_sample_count += bins[b].count;
            contributed = true;
        }
        overview->source_sample_count += history_as11_record_at_or_after(candidate, end) -
                                          history_as11_record_at_or_after(candidate, start);
        if (contributed && overview->contributing_sessions < UINT16_MAX)
            ++overview->contributing_sessions;
        verified = true;
        history_cache_put(&key, &verified, sizeof(verified));
    }
    free(bins);
    return read == 0;
}

static esp_err_t history_accumulate_as11(
    const char *day, touch_history_signal_t signal,
    const history_session_info_t *sessions, size_t session_count,
    int64_t axis_start_ms, int64_t axis_end_ms,
    history_overview_aggregate_t *aggregate,
    touch_history_overview_t *overview,
    bool raw_flow, bool mixed_flow_fallback,
    const touch_history_operation_t *operation,
    uint16_t progress_start, uint16_t progress_end)
{
    int16_t *records = history_alloc(HISTORY_READ_VALUES * sizeof(*records),
                                     false);
    if (!records) return ESP_ERR_NO_MEM;
    esp_err_t result = ESP_OK;
    for (size_t s = 0; s < session_count; ++s) {
        if (history_operation_cancelled(operation)) {
            result = TOUCH_HISTORY_ERR_CANCELLED;
            break;
        }
        uint16_t session_progress_start = history_progress_fraction(
            progress_start, progress_end, s, session_count);
        uint16_t session_progress_end = history_progress_fraction(
            progress_start, progress_end, s + 1, session_count);
        trace_candidate_t candidate = {0};
        bool candidate_raw = raw_flow &&
                             signal == TOUCH_HISTORY_SIGNAL_FLOW;
        esp_err_t inspect = candidate_raw
            ? history_flow_raw_candidate(day, &sessions[s], &candidate)
            : history_session_candidate(day, &sessions[s], signal, &candidate);
        if (inspect == ESP_ERR_NOT_FOUND && mixed_flow_fallback &&
            signal == TOUCH_HISTORY_SIGNAL_FLOW) {
            inspect = history_flow_raw_candidate(
                day, &sessions[s], &candidate);
            candidate_raw = inspect == ESP_OK;
        }
        if (inspect == ESP_ERR_NOT_FOUND) {
            history_operation_progress(operation, session_progress_end);
            continue;
        }
        if (inspect != ESP_OK) {
            if (inspect == ESP_ERR_NO_MEM ||
                inspect == TOUCH_HISTORY_ERR_CANCELLED) {
                result = inspect;
                break;
            }
            ESP_LOGW(TAG, "skip corrupt graph source day=%s session=%s signal=%u: %s",
                     day, sessions[s].id, (unsigned)signal,
                     esp_err_to_name(inspect));
            if (overview->unreadable_sessions < UINT16_MAX)
                overview->unreadable_sessions++;
            history_operation_progress(operation, session_progress_end);
            continue;
        }
        uint32_t first_record = history_as11_record_at_or_after(
            &candidate, axis_start_ms);
        uint32_t end_record = history_as11_record_at_or_after(
            &candidate, axis_end_ms);
        if (end_record <= first_record) {
            history_operation_progress(operation, session_progress_end);
            continue;
        }
        if (!candidate_raw && signal == TOUCH_HISTORY_SIGNAL_FLOW &&
            history_accumulate_flow_pyramid(day, &candidate, s, axis_start_ms,
                axis_end_ms, aggregate, overview, operation)) {
            history_operation_progress(operation, session_progress_end);
            continue;
        }
        if (history_operation_cancelled(operation)) {
            result = TOUCH_HISTORY_ERR_CANCELLED;
            break;
        }
        FILE *file = fopen(candidate.path, "rb");
        uint64_t byte_offset = (uint64_t)candidate.header_bytes +
            (uint64_t)first_record * candidate.n_channels * sizeof(int16_t);
        if (!file || byte_offset > LONG_MAX ||
            fseek(file, (long)byte_offset, SEEK_SET) != 0) {
            if (file) fclose(file);
            result = ESP_FAIL;
            break;
        }
        const size_t per_read = HISTORY_READ_VALUES / candidate.n_channels;
        uint32_t processed = first_record;
        bool session_contributed = false;
        while (processed < end_record) {
            if (history_operation_cancelled(operation)) {
                result = TOUCH_HISTORY_ERR_CANCELLED;
                break;
            }
            size_t wanted = end_record - processed;
            if (wanted > per_read) wanted = per_read;
            size_t got = fread(records,
                               candidate.n_channels * sizeof(int16_t),
                               wanted, file);
            for (size_t r = 0; r < got; ++r) {
                uint32_t record_index = processed + (uint32_t)r;
                int64_t timestamp_ms = candidate.start_epoch_ms +
                    (int64_t)((uint64_t)record_index * 10000U /
                              candidate.sample_hz_x10);
                int bin = history_overview_bin_count(
                    axis_start_ms, axis_end_ms, timestamp_ms,
                    overview->point_count);
                if (bin < 0) continue;
                int16_t *record = &records[r * candidate.n_channels];
                overview->source_sample_count++;
                if (signal == TOUCH_HISTORY_SIGNAL_FLOW) {
                    int16_t low = 0;
                    int16_t high = 0;
                    bool have_low = history_raw_value_valid(
                        record[0], candidate.version, true) &&
                        history_scale_x100(signal, record[0], &low);
                    bool have_high = !candidate_raw &&
                        history_raw_value_valid(
                            record[1], candidate.version, true) &&
                        history_scale_x100(signal, record[1], &high);
                    if (!have_low && !have_high) continue;
                    if (!have_low) low = high;
                    if (!have_high) high = low;
                    if (candidate_raw &&
                        overview->aggregation == TOUCH_HISTORY_AGGREGATION_MEAN) {
                        history_aggregate_value(aggregate, (size_t)bin, low);
                    } else {
                        if (low < aggregate->minimum[bin])
                            aggregate->minimum[bin] = low;
                        if (high > aggregate->maximum[bin])
                            aggregate->maximum[bin] = high;
                        if (aggregate->count[bin] < UINT32_MAX)
                            aggregate->count[bin]++;
                    }
                    overview->valid_sample_count++;
                    session_contributed = true;
                } else {
                    int channel = history_pld_channel(signal);
                    int16_t value = 0;
                    if (channel < 0 || !history_raw_value_valid(
                            record[channel], candidate.version, false) ||
                        !history_scale_x100(signal, record[channel], &value))
                        continue;
                    history_aggregate_value(aggregate, (size_t)bin, value);
                    overview->valid_sample_count++;
                    session_contributed = true;
                    if (signal == TOUCH_HISTORY_SIGNAL_PRESSURE &&
                        history_raw_value_valid(record[2], candidate.version,
                                                false)) {
                        int16_t companion = 0;
                        if (history_scale_x100(signal, record[2], &companion)) {
                            aggregate->companion_sum[bin] += companion;
                            if (aggregate->companion_count[bin] < UINT32_MAX)
                                aggregate->companion_count[bin]++;
                        }
                    }
                }
            }
            processed += (uint32_t)got;
            history_operation_progress(operation, history_progress_fraction(
                session_progress_start, session_progress_end,
                processed - first_record, end_record - first_record));
            if (got != wanted || ferror(file)) {
                result = ESP_FAIL;
                break;
            }
        }
        if (fclose(file) != 0) result = ESP_FAIL;
        if (result != ESP_OK) break;
        if (session_contributed && overview->contributing_sessions < UINT16_MAX)
            overview->contributing_sessions++;
    }
    free(records);
    return result;
}

static bool history_ox_record_value(touch_history_signal_t signal,
                                    const int16_t *record, int16_t *value)
{
    size_t channel = 0;
    uint16_t missing_bit = 0;
    switch (signal) {
    case TOUCH_HISTORY_SIGNAL_SPO2:
        channel = OXIMETRY_CANONICAL_VITALS_SPO2;
        missing_bit = 1U;
        break;
    case TOUCH_HISTORY_SIGNAL_PULSE:
        channel = OXIMETRY_CANONICAL_VITALS_PULSE;
        missing_bit = 2U;
        break;
    case TOUCH_HISTORY_SIGNAL_MOTION:
        channel = OXIMETRY_CANONICAL_VITALS_MOTION_FLAGS;
        break;
    default:
        return false;
    }
    if (missing_bit &&
        (((uint16_t)record[OXIMETRY_CANONICAL_VITALS_STATUS] & missing_bit) != 0))
        return false;
    int16_t raw = record[channel];
    if (raw == OXIMETRY_CANONICAL_SNT_MISSING) return false;
    if (signal == TOUCH_HISTORY_SIGNAL_SPO2 && (raw <= 0 || raw > 10000))
        return false;
    if (signal == TOUCH_HISTORY_SIGNAL_PULSE && (raw <= 0 || raw > 30000))
        return false;
    if (signal == TOUCH_HISTORY_SIGNAL_MOTION && raw < 0) return false;
    return history_scale_x100(signal, raw, value);
}

static void history_coverage_mark(
    uint8_t *bits, size_t bit_count, int64_t axis_start_ms,
    int64_t sample_start_ms, int64_t sample_end_ms,
    const history_therapy_interval_t *intervals, size_t interval_count)
{
    if (!bits || !bit_count || sample_end_ms <= sample_start_ms) return;
    for (size_t i = 0; i < interval_count; ++i) {
        int64_t start = sample_start_ms > intervals[i].start_ms
            ? sample_start_ms : intervals[i].start_ms;
        int64_t end = sample_end_ms < intervals[i].end_ms
            ? sample_end_ms : intervals[i].end_ms;
        if (end <= start) continue;
        size_t first = (size_t)((start - axis_start_ms) / 1000);
        size_t last = (size_t)((end - 1 - axis_start_ms) / 1000);
        if (first >= bit_count) continue;
        if (last >= bit_count) last = bit_count - 1;
        for (size_t bit = first; bit <= last; ++bit)
            bits[bit >> 3] |= (uint8_t)(1U << (bit & 7));
    }
}

static uint64_t history_coverage_milliseconds(
    const uint8_t *bits, size_t bit_count, int64_t axis_start_ms,
    const history_therapy_interval_t *intervals, size_t interval_count)
{
    uint64_t total = 0;
    for (size_t i = 0; i < interval_count; ++i) {
        size_t first = (size_t)((intervals[i].start_ms - axis_start_ms) / 1000);
        size_t last = (size_t)((intervals[i].end_ms - 1 - axis_start_ms) / 1000);
        if (first >= bit_count) continue;
        if (last >= bit_count) last = bit_count - 1;
        for (size_t bit = first; bit <= last; ++bit) {
            if (!(bits[bit >> 3] & (uint8_t)(1U << (bit & 7)))) continue;
            int64_t slot_start = axis_start_ms + (int64_t)bit * 1000;
            int64_t slot_end = slot_start + 1000;
            int64_t start = slot_start > intervals[i].start_ms
                ? slot_start : intervals[i].start_ms;
            int64_t end = slot_end < intervals[i].end_ms
                ? slot_end : intervals[i].end_ms;
            if (end > start) total += (uint64_t)(end - start);
        }
    }
    return total;
}

static esp_err_t history_accumulate_oximetry(
    const char *day, touch_history_signal_t signal,
    const history_session_info_t *sessions, size_t session_count,
    int64_t axis_start_ms, int64_t axis_end_ms,
    history_overview_aggregate_t *aggregate,
    touch_history_overview_t *overview,
    bool therapy_only,
    const touch_history_operation_t *operation,
    uint16_t progress_start, uint16_t progress_end)
{
    history_operation_progress(operation, progress_start);
    trace_candidate_t *candidates = NULL;
    size_t candidate_count = 0;
    esp_err_t result = history_collect_ox_candidates(
        day, signal, axis_start_ms, axis_end_ms,
        true,
        &candidates, &candidate_count, operation);
    if (result != ESP_OK) return result;
    history_therapy_interval_t *eligible_intervals = NULL;
    size_t eligible_interval_count = 0;
    uint64_t eligible_ms = 0;
    overview->has_therapy_coverage = false;
    esp_err_t coverage_result = history_collect_eligible_intervals_leased(
        day, sessions, session_count, &eligible_intervals,
        &eligible_interval_count, &eligible_ms, operation);
    if (coverage_result == TOUCH_HISTORY_ERR_CANCELLED) {
        free(candidates);
        return coverage_result;
    }
    if (coverage_result == ESP_OK) {
        size_t kept = 0;
        eligible_ms = 0;
        for (size_t i = 0; i < eligible_interval_count; ++i) {
            int64_t start = eligible_intervals[i].start_ms > axis_start_ms
                ? eligible_intervals[i].start_ms : axis_start_ms;
            int64_t end = eligible_intervals[i].end_ms < axis_end_ms
                ? eligible_intervals[i].end_ms : axis_end_ms;
            if (end <= start) continue;
            eligible_intervals[kept].start_ms = start;
            eligible_intervals[kept].end_ms = end;
            eligible_ms += (uint64_t)(end - start);
            kept++;
        }
        eligible_interval_count = kept;
        if (!kept || !eligible_ms) coverage_result = ESP_ERR_NOT_FOUND;
    }
    if (therapy_only && coverage_result != ESP_OK) {
        free(eligible_intervals);
        free(candidates);
        return coverage_result;
    }
    uint64_t axis_span_ms = (uint64_t)(axis_end_ms - axis_start_ms);
    size_t coverage_bit_count = (size_t)((axis_span_ms + 999U) / 1000U);
    size_t coverage_bytes = (coverage_bit_count + 7U) / 8U;
    uint8_t *coverage_bits = NULL;
    if (coverage_result == ESP_OK && eligible_ms && coverage_bytes) {
        coverage_bits = history_alloc(coverage_bytes, true);
        if (!coverage_bits) coverage_result = ESP_ERR_NO_MEM;
    }
    uint16_t read_progress_start = history_progress_fraction(
        progress_start, progress_end, 1, 4);
    history_operation_progress(operation, read_progress_start);
    int16_t *records = history_alloc(HISTORY_READ_VALUES * sizeof(*records),
                                     false);
    if (!records) {
        free(coverage_bits);
        free(eligible_intervals);
        free(candidates);
        return ESP_ERR_NO_MEM;
    }
    uint64_t total_records = 0;
    for (size_t i = 0; i < candidate_count; ++i)
        total_records += candidates[i].records;
    uint64_t all_processed = 0;
    uint16_t contributing = 0;
    for (size_t c = 0; c < candidate_count; ++c) {
        if (history_operation_cancelled(operation)) {
            result = TOUCH_HISTORY_ERR_CANCELLED;
            break;
        }
        const trace_candidate_t *candidate = &candidates[c];
        FILE *file = fopen(candidate->path, "rb");
        if (!file || fseek(file, candidate->header_bytes, SEEK_SET) != 0) {
            if (file) fclose(file);
            result = ESP_FAIL;
            break;
        }
        const size_t per_read = HISTORY_READ_VALUES / candidate->n_channels;
        uint32_t processed = 0;
        bool candidate_contributed = false;
        int last_claimed_bin = -1;
        while (processed < candidate->records) {
            if (history_operation_cancelled(operation)) {
                result = TOUCH_HISTORY_ERR_CANCELLED;
                break;
            }
            size_t wanted = candidate->records - processed;
            if (wanted > per_read) wanted = per_read;
            size_t got = fread(records,
                               candidate->n_channels * sizeof(int16_t),
                               wanted, file);
            for (size_t r = 0; r < got; ++r) {
                uint32_t record_index = processed + (uint32_t)r;
                uint64_t offset_us = (uint64_t)record_index *
                                     candidate->period_num_us /
                                     candidate->period_den;
                int64_t timestamp_ms = candidate->start_epoch_ms +
                                       (int64_t)(offset_us / 1000U);
                if (therapy_only && !history_interval_contains(
                        eligible_intervals, eligible_interval_count,
                        timestamp_ms)) continue;
                int bin = history_overview_bin_count(
                    axis_start_ms, axis_end_ms, timestamp_ms,
                    overview->point_count);
                if (bin < 0) continue;
                /* Match the browser's deterministic overlap rule: a later
                 * recording owns every display bin it touches, including a
                 * bin containing only missing samples. */
                if (bin != last_claimed_bin) {
                    aggregate->sum[bin] = 0;
                    aggregate->minimum[bin] = INT16_MAX;
                    aggregate->maximum[bin] = INT16_MIN;
                    aggregate->count[bin] = 0;
                    last_claimed_bin = bin;
                }
                overview->source_sample_count++;
                int16_t value = 0;
                int16_t *record = &records[r * candidate->n_channels];
                if (!history_ox_record_value(signal, record, &value)) continue;
                history_aggregate_value(aggregate, (size_t)bin, value);
                overview->valid_sample_count++;
                candidate_contributed = true;
                if (coverage_bits) {
                    uint64_t denominator_us =
                        (uint64_t)candidate->period_den * 1000U;
                    uint64_t sample_ms = denominator_us
                        ? ((uint64_t)candidate->period_num_us +
                           denominator_us - 1U) / denominator_us : 0;
                    if (!sample_ms) sample_ms = 1;
                    int64_t sample_end_ms = timestamp_ms +
                        (int64_t)sample_ms;
                    history_coverage_mark(
                        coverage_bits, coverage_bit_count, axis_start_ms,
                        timestamp_ms, sample_end_ms, eligible_intervals,
                        eligible_interval_count);
                }
            }
            processed += (uint32_t)got;
            all_processed += got;
            history_operation_progress(operation, history_progress_fraction(
                read_progress_start, progress_end, all_processed,
                total_records));
            if (got != wanted || ferror(file)) {
                result = ESP_FAIL;
                break;
            }
        }
        if (fclose(file) != 0) result = ESP_FAIL;
        if (result != ESP_OK) break;
        if (candidate_contributed && contributing < UINT16_MAX) contributing++;
    }
    free(records);
    if (result == ESP_OK && coverage_bits && eligible_ms) {
        uint64_t valid_ms = history_coverage_milliseconds(
            coverage_bits, coverage_bit_count, axis_start_ms,
            eligible_intervals, eligible_interval_count);
        uint64_t per_mille = valid_ms * 1000U / eligible_ms;
        if (per_mille > 1000U) per_mille = 1000U;
        overview->therapy_coverage_per_mille = (uint16_t)per_mille;
        overview->has_therapy_coverage = true;
    }
    free(coverage_bits);
    free(eligible_intervals);
    free(candidates);
    overview->contributing_sessions = contributing;
    return result;
}

static esp_err_t history_load_overview_leased(
    const char *day, touch_history_signal_t signal,
    int64_t requested_start_ms, int64_t requested_end_ms,
    touch_history_overview_t *overview,
    bool therapy_only,
    const touch_history_operation_t *operation,
    uint16_t progress_start, uint16_t progress_end)
{
    history_operation_progress(operation, progress_start);
    history_session_info_t *sessions = NULL;
    size_t session_count = 0;
    esp_err_t result = history_load_sessions_leased(
        day, &sessions, &session_count, NULL, operation);
    if (result == ESP_ERR_NOT_FOUND) result = ESP_OK;
    if (result != ESP_OK) return result;
    uint16_t data_progress_start = history_progress_fraction(
        progress_start, progress_end, 1, 8);
    history_operation_progress(operation, data_progress_start);
    if (session_count)
        result = history_resolve_session_ends(day, sessions, session_count);
    int64_t night_start_ms = 0;
    int64_t night_end_ms = 0;
    if (result == ESP_OK)
        result = history_unified_axis(
            day, sessions, session_count, &night_start_ms, &night_end_ms,
            NULL, NULL, operation);
    bool ranged = requested_start_ms != 0 || requested_end_ms != 0;
    int64_t axis_start_ms = night_start_ms;
    int64_t axis_end_ms = night_end_ms;
    if (result == ESP_OK && ranged) {
        if (requested_start_ms < night_start_ms ||
            requested_end_ms > night_end_ms ||
            requested_end_ms <= requested_start_ms) {
            result = ESP_ERR_INVALID_ARG;
        } else {
            axis_start_ms = requested_start_ms;
            axis_end_ms = requested_end_ms;
        }
    }
    uint64_t span_ms = result == ESP_OK
        ? (uint64_t)(axis_end_ms - axis_start_ms) : 0;
    uint16_t point_count = ranged
        ? touch_history_range_point_count(signal, span_ms)
        : TOUCH_HISTORY_OVERVIEW_POINTS;
    bool raw_flow = false;
    bool source_fallback = false;
    if (result == ESP_OK && ranged &&
        signal == TOUCH_HISTORY_SIGNAL_FLOW) {
        uint64_t night_span_ms = (uint64_t)(night_end_ms - night_start_ms);
        bool prefer_raw = touch_history_flow_range_prefers_raw(
            span_ms, night_span_ms);
        result = history_select_flow_range_source(
            day, sessions, session_count, axis_start_ms, axis_end_ms,
            prefer_raw, &raw_flow, &source_fallback, operation);
    }
    history_overview_aggregate_t *aggregate = NULL;
    if (result == ESP_OK) {
        aggregate = history_alloc(sizeof(*aggregate), true);
        if (!aggregate) result = ESP_ERR_NO_MEM;
    }
    if (result == ESP_OK) {
        history_overview_prepare(overview, signal, axis_start_ms, axis_end_ms,
                                 sessions, session_count, point_count,
                                 aggregate);
        overview->source_raw = raw_flow;
        overview->source_fallback = source_fallback;
        /* A point-for-point raw window is a conventional waveform. Once the
         * 25 Hz source outnumbers the fixed display bins, signed means can
         * cancel an entire breath around zero; retain its min/max envelope. */
        if (raw_flow && !touch_history_flow_bins_need_envelope(
                span_ms, point_count, 250U))
            overview->aggregation = TOUCH_HISTORY_AGGREGATION_MEAN;
        if (signal <= TOUCH_HISTORY_SIGNAL_SNORE) {
            result = history_accumulate_as11(
                day, signal, sessions, session_count, axis_start_ms,
                axis_end_ms, aggregate, overview, raw_flow,
                source_fallback, operation,
                data_progress_start, progress_end);
        } else {
            result = history_accumulate_oximetry(
                day, signal, sessions, session_count, axis_start_ms,
                axis_end_ms, aggregate, overview, therapy_only, operation,
                data_progress_start, progress_end);
        }
        if (result == ESP_OK) {
            history_overview_finalize(overview, aggregate);
            if (!overview->has_data) result = ESP_ERR_NOT_FOUND;
        }
    }
    free(aggregate);
    free(sessions);
    overview->loaded = result == ESP_OK || result == ESP_ERR_NOT_FOUND;
    if (result == ESP_OK || result == ESP_ERR_NOT_FOUND)
        history_operation_progress(operation, progress_end);
    return result;
}

esp_err_t touch_history_load_overview(const char *day,
                                      touch_history_signal_t signal,
                                      touch_history_overview_t *overview)
{
    return touch_history_load_overview_ex(day, signal, overview, NULL);
}

esp_err_t touch_history_load_overview_ex(
    const char *day, touch_history_signal_t signal,
    touch_history_overview_t *overview,
    const touch_history_operation_t *operation)
{
    if (!overview || !valid_day(day) || signal < TOUCH_HISTORY_SIGNAL_FLOW ||
        signal >= TOUCH_HISTORY_SIGNAL_COUNT) return ESP_ERR_INVALID_ARG;
    memset(overview, 0, sizeof(*overview));
    overview->signal = signal;
    for (size_t i = 0; i < TOUCH_HISTORY_OVERVIEW_POINTS; ++i) {
        overview->value_x100[i] = TOUCH_HISTORY_VALUE_MISSING;
        overview->upper_x100[i] = TOUCH_HISTORY_VALUE_MISSING;
        overview->companion_x100[i] = TOUCH_HISTORY_VALUE_MISSING;
    }
    history_operation_progress(operation, 0);
    esp_err_t lease = history_lease_acquire_operation(operation);
    if (lease != ESP_OK) return lease;
    esp_err_t result = history_load_overview_leased(
        day, signal, 0, 0, overview, false, operation, 50, 1000);
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    return result;
}

esp_err_t touch_history_load_range(
    const char *day, touch_history_signal_t signal,
    int64_t start_ms, int64_t end_ms, touch_history_overview_t *range)
{
    return touch_history_load_range_ex(
        day, signal, start_ms, end_ms, range, NULL);
}

esp_err_t touch_history_load_range_ex(
    const char *day, touch_history_signal_t signal,
    int64_t start_ms, int64_t end_ms, touch_history_overview_t *range,
    const touch_history_operation_t *operation)
{
    if (!range || !valid_day(day) || signal < TOUCH_HISTORY_SIGNAL_FLOW ||
        signal >= TOUCH_HISTORY_SIGNAL_COUNT || start_ms <= 0 ||
        end_ms <= start_ms || end_ms - start_ms > HISTORY_AXIS_MAX_MS)
        return ESP_ERR_INVALID_ARG;
    memset(range, 0, sizeof(*range));
    range->signal = signal;
    for (size_t i = 0; i < TOUCH_HISTORY_OVERVIEW_POINTS; ++i) {
        range->value_x100[i] = TOUCH_HISTORY_VALUE_MISSING;
        range->upper_x100[i] = TOUCH_HISTORY_VALUE_MISSING;
        range->companion_x100[i] = TOUCH_HISTORY_VALUE_MISSING;
    }
    history_operation_progress(operation, 0);
    esp_err_t lease = history_lease_acquire_operation(operation);
    if (lease != ESP_OK) return lease;
    esp_err_t result = history_load_overview_leased(
        day, signal, start_ms, end_ms, range, false, operation, 50, 1000);
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    return result;
}

esp_err_t touch_history_load_view_ex(
    const char *day, touch_history_signal_t signal,
    int64_t start_ms, int64_t end_ms, bool therapy_only,
    touch_history_overview_t *view,
    const touch_history_operation_t *operation)
{
    bool all_night = start_ms == 0 && end_ms == 0;
    if (!view || !valid_day(day) || signal < TOUCH_HISTORY_SIGNAL_FLOW ||
        signal >= TOUCH_HISTORY_SIGNAL_COUNT ||
        (!all_night && (start_ms <= 0 || end_ms <= start_ms ||
                        end_ms - start_ms > HISTORY_AXIS_MAX_MS)) ||
        (therapy_only && signal != TOUCH_HISTORY_SIGNAL_SPO2))
        return ESP_ERR_INVALID_ARG;
    memset(view, 0, sizeof(*view));
    view->signal = signal;
    for (size_t i = 0; i < TOUCH_HISTORY_OVERVIEW_POINTS; ++i) {
        view->value_x100[i] = TOUCH_HISTORY_VALUE_MISSING;
        view->upper_x100[i] = TOUCH_HISTORY_VALUE_MISSING;
        view->companion_x100[i] = TOUCH_HISTORY_VALUE_MISSING;
    }
    history_operation_progress(operation, 0);
    esp_err_t lease = history_lease_acquire_operation(operation);
    if (lease != ESP_OK) return lease;
    history_cache_key_t key = history_cache_key(day, HISTORY_MEM_VIEW,
        signal, start_ms, end_ms, therapy_only);
    esp_err_t result = ESP_OK;
    if (!history_cache_get(&key, view, sizeof(*view))) {
        result = history_load_overview_leased(
            day, signal, start_ms, end_ms, view, therapy_only, operation, 50, 1000);
        if (result == ESP_OK && !history_operation_cancelled(operation))
            history_cache_put(&key, view, sizeof(*view));
    }
    if (history_operation_cancelled(operation)) result = TOUCH_HISTORY_ERR_CANCELLED;
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    return result;
}

typedef struct {
    int64_t rise_ms;
    int64_t fall_ms;
    int64_t mask_on_ms;
    int64_t mask_off_ms;
} history_gate_t;

typedef struct {
    touch_history_event_t *items;
    size_t count;
    size_t capacity;
    uint32_t counts[TOUCH_HISTORY_EVENT_TYPE_COUNT];
} history_event_vector_t;

static int64_t history_days_from_civil(int year, int month, int day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = (unsigned)(year - era * 400);
    const unsigned day_of_year = (unsigned)(
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 -
                                year_of_era / 100 + day_of_year;
    return (int64_t)era * 146097 + (int)day_of_era - 719468;
}

static uint8_t history_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month < 1 || month > 12) return 0;
    if (month != 2) return days[month - 1];
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return leap ? 29 : 28;
}

static int64_t history_parse_iso8601_utc_ms(const char *text)
{
    if (!text) return -1;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millis = 0;
    int parsed = sscanf(text, "%d-%d-%dT%d:%d:%d.%dZ", &year, &month,
                        &day, &hour, &minute, &second, &millis);
    if (parsed < 6) {
        parsed = sscanf(text, "%d-%d-%dT%d:%d:%dZ", &year, &month, &day,
                        &hour, &minute, &second);
        millis = 0;
    }
    if (parsed < 6 || year < 2000 || year > 2200 || month < 1 || month > 12 ||
        day < 1 || day > history_days_in_month((uint16_t)year, (uint8_t)month) ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60 || millis < 0 || millis > 999) return -1;
    int64_t days = history_days_from_civil(year, month, day);
    return (days * 86400 + hour * 3600 + minute * 60 + second) * 1000 +
           millis;
}

static esp_err_t history_event_append(history_event_vector_t *events,
                                      const touch_history_event_t *event)
{
    if (!events || !event) return ESP_ERR_INVALID_ARG;
    if (events->count == events->capacity) {
        size_t next_capacity = events->capacity ? events->capacity * 2
                                                : HISTORY_EVENT_INITIAL_CAPACITY;
        touch_history_event_t *grown = history_grow_array(
            events->items, events->capacity, next_capacity,
            sizeof(*events->items));
        if (!grown) return ESP_ERR_NO_MEM;
        events->items = grown;
        events->capacity = next_capacity;
    }
    events->items[events->count++] = *event;
    return ESP_OK;
}

static esp_err_t history_parse_event_file(
    const char *path, const history_session_info_t *session,
    size_t session_index, history_gate_t *gate,
    history_event_vector_t *events,
    const touch_history_operation_t *operation)
{
    if (!path || !session || !gate) return ESP_ERR_INVALID_ARG;
    FILE *file = fopen(path, "r");
    if (!file) return errno == ENOENT || errno == ENOTDIR
                         ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    char *line = history_alloc(HISTORY_EVENT_LINE_BYTES, false);
    if (!line) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = ESP_OK;
    while (fgets(line, HISTORY_EVENT_LINE_BYTES, file)) {
        if (history_operation_cancelled(operation)) {
            result = TOUCH_HISTORY_ERR_CANCELLED;
            break;
        }
        size_t length = strlen(line);
        if (length == HISTORY_EVENT_LINE_BYTES - 1 && line[length - 1] != '\n' &&
            !feof(file)) {
            result = ESP_FAIL;
            break;
        }
        const char *cursor = line;
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
               *cursor == '\n') cursor++;
        if (!*cursor) continue;
        cJSON *root = cJSON_Parse(cursor);
        if (!root) {
            result = ESP_FAIL;
            break;
        }
        cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
        cJSON *data_id = params ? cJSON_GetObjectItemCaseSensitive(
            params, "dataId") : NULL;
        cJSON *array = params ? cJSON_GetObjectItemCaseSensitive(
            params, "events") : NULL;
        bool respiratory = cJSON_IsString(data_id) &&
            !strcmp(data_id->valuestring,
                    "TherapyEvents-RespiratoryEvents");
        if (cJSON_IsArray(array)) {
            int array_size = cJSON_GetArraySize(array);
            for (int i = 0; i < array_size; ++i) {
                if (history_operation_cancelled(operation)) {
                    result = TOUCH_HISTORY_ERR_CANCELLED;
                    break;
                }
                cJSON *item = cJSON_GetArrayItem(array, i);
                cJSON *report_time = item ? cJSON_GetObjectItemCaseSensitive(
                    item, "reportTime") : NULL;
                int64_t as11_ms = cJSON_IsString(report_time)
                    ? history_parse_iso8601_utc_ms(report_time->valuestring) : -1;
                cJSON *ntp_time = item ? cJSON_GetObjectItemCaseSensitive(
                    item, "ntpTimeMs") : NULL;
                int64_t ntp_ms = cJSON_IsNumber(ntp_time) &&
                                 isfinite(ntp_time->valuedouble) &&
                                 ntp_time->valuedouble > 0 &&
                                 ntp_time->valuedouble <= (double)INT64_MAX
                    ? (int64_t)ntp_time->valuedouble : -1;
                /* Markers and gates must share the NTP/session axis. Raw AS11
                 * reportTime is not substituted when drift is unknown. */
                int64_t corrected_ms = 0;
                bool time_corrected = session->clock_drift_usable &&
                    touch_history_apply_clock_drift(
                        as11_ms, session->clock_drift_ms, &corrected_ms);
                int64_t event_ms = time_corrected ? corrected_ms : ntp_ms;
                int64_t gate_ms = event_ms;
                cJSON *name = item ? cJSON_GetObjectItemCaseSensitive(
                    item, "event") : NULL;
                if (gate_ms > 0 && cJSON_IsString(name)) {
                    if (!strcmp(name->valuestring, "MaskOn") &&
                        gate->mask_on_ms <= 0) gate->mask_on_ms = gate_ms;
                    if (!strcmp(name->valuestring, "MaskOff"))
                        gate->mask_off_ms = gate_ms;
                }
                if (gate_ms > 0 && cJSON_IsString(data_id) &&
                    !strcmp(data_id->valuestring, "_ZLE")) {
                    cJSON *value = cJSON_GetObjectItemCaseSensitive(
                        item, "value");
                    if (cJSON_IsNumber(value)) {
                        if ((int)value->valuedouble == 1 && gate->rise_ms <= 0)
                            gate->rise_ms = gate_ms;
                        else if ((int)value->valuedouble == 0)
                            gate->fall_ms = gate_ms;
                    }
                }
                if (!respiratory || !cJSON_IsString(name)) continue;
                touch_history_event_type_t type =
                    touch_history_event_type_from_name(name->valuestring);
                if (type == TOUCH_HISTORY_EVENT_UNKNOWN) continue;
                if (!events) continue;
                /* A recognized event without a usable time must not quietly
                 * lower AHI or disappear from the marker lane. */
                if (event_ms <= 0) {
                    result = ESP_FAIL;
                    break;
                }
                cJSON *duration = cJSON_GetObjectItemCaseSensitive(
                    item, "durationSeconds");
                double seconds = cJSON_IsNumber(duration) &&
                                 isfinite(duration->valuedouble) &&
                                 duration->valuedouble > 0
                    ? duration->valuedouble : 0.0;
                if (seconds > 24.0 * 60.0 * 60.0) {
                    result = ESP_FAIL;
                    break;
                }
                touch_history_event_t event = {
                    .type = type,
                    .end_ms = event_ms,
                    .start_ms = event_ms - (int64_t)llround(seconds * 1000.0),
                    .session_index = session_index > UINT16_MAX
                        ? UINT16_MAX : (uint16_t)session_index,
                    .time_corrected = time_corrected,
                };
                result = history_event_append(events, &event);
                if (result != ESP_OK) break;
                if (events->counts[type] < UINT32_MAX) events->counts[type]++;
            }
        }
        cJSON_Delete(root);
        if (result != ESP_OK) break;
    }
    if (ferror(file)) result = ESP_FAIL;
    if (fclose(file) != 0) result = ESP_FAIL;
    free(line);
    return result;
}

static uint32_t history_round_ms_to_samples(int64_t duration_ms,
                                            uint16_t hz_x10)
{
    if (duration_ms <= 0 || !hz_x10) return 0;
    uint64_t numerator = (uint64_t)duration_ms * hz_x10 + 5000U;
    uint64_t samples = numerator / 10000U;
    return samples > UINT32_MAX ? UINT32_MAX : (uint32_t)samples;
}

static uint64_t history_eligible_session_interval(
    const trace_candidate_t *pld, const history_gate_t *gate,
    history_therapy_interval_t *interval)
{
    if (!pld || !pld->sample_hz_x10 || !pld->records) return 0;
    uint32_t skip = 0;
    uint32_t keep = pld->records;
    int64_t gate_start = gate->rise_ms > 0 ? gate->rise_ms : gate->mask_on_ms;
    int64_t gate_end = gate->fall_ms > 0 ? gate->fall_ms : gate->mask_off_ms;
    int64_t effective_start = pld->start_epoch_ms;
    if (gate_start > effective_start) {
        skip = history_round_ms_to_samples(
            gate_start - pld->start_epoch_ms, pld->sample_hz_x10);
        if (skip > pld->records) skip = pld->records;
        keep = pld->records - skip;
        effective_start = pld->start_epoch_ms +
            (int64_t)((uint64_t)skip * 10000U / pld->sample_hz_x10);
    }
    if (gate_end > 0) {
        if (gate_end <= effective_start) return 0;
        uint32_t gated = history_round_ms_to_samples(
            gate_end - effective_start, pld->sample_hz_x10);
        if (gated < keep) keep = gated;
    }
    uint32_t samples_per_minute = (uint32_t)pld->sample_hz_x10 * 6U;
    if (!samples_per_minute) return 0;
    keep = (keep / samples_per_minute) * samples_per_minute;
    uint64_t duration_ms = (uint64_t)keep * 10000U / pld->sample_hz_x10;
    if (interval && duration_ms) {
        uint64_t skip_ms = (uint64_t)skip * 10000U / pld->sample_hz_x10;
        interval->start_ms = pld->start_epoch_ms + (int64_t)skip_ms;
        interval->end_ms = interval->start_ms + (int64_t)duration_ms;
    }
    return duration_ms;
}

static esp_err_t history_collect_eligible_intervals_uncached(
    const char *day, const history_session_info_t *sessions,
    size_t session_count, history_therapy_interval_t **intervals_out,
    size_t *interval_count_out, uint64_t *eligible_ms_out,
    const touch_history_operation_t *operation)
{
    if (!day || !intervals_out || !interval_count_out || !eligible_ms_out ||
        (session_count && !sessions)) return ESP_ERR_INVALID_ARG;
    *intervals_out = NULL;
    *interval_count_out = 0;
    *eligible_ms_out = 0;
    if (!session_count) return ESP_ERR_NOT_FOUND;
    if (session_count > SIZE_MAX / sizeof(history_therapy_interval_t))
        return ESP_ERR_NO_MEM;
    history_therapy_interval_t *intervals = history_alloc(
        session_count * sizeof(*intervals), true);
    if (!intervals) return ESP_ERR_NO_MEM;
    size_t count = 0;
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < session_count; ++i) {
        if (history_operation_cancelled(operation)) {
            result = TOUCH_HISTORY_ERR_CANCELLED;
            break;
        }
        trace_candidate_t pld = {0};
        esp_err_t inspect = history_session_candidate(
            day, &sessions[i], TOUCH_HISTORY_SIGNAL_PRESSURE, &pld);
        if (inspect == ESP_ERR_NOT_FOUND) {
            if (sessions[i].brp_samples >= 1500U) {
                result = ESP_ERR_NOT_FOUND;
                break;
            }
            continue;
        }
        if (inspect != ESP_OK) {
            result = (inspect == ESP_ERR_NO_MEM ||
                      inspect == TOUCH_HISTORY_ERR_CANCELLED)
                         ? inspect : ESP_ERR_NOT_FOUND;
            break;
        }
        if (pld.records < pld.sample_hz_x10 * 6U) continue;
        char path[384];
        if (snprintf(path, sizeof(path), "%s/%s/%s_events.snt",
                     SD_STREAMS_DIR, day, sessions[i].id) >=
            (int)sizeof(path)) {
            result = ESP_FAIL;
            break;
        }
        history_gate_t gate = {0};
        result = history_parse_event_file(
            path, &sessions[i], i, &gate, NULL, operation);
        if (result != ESP_OK) {
            if (result != ESP_ERR_NO_MEM &&
                result != TOUCH_HISTORY_ERR_CANCELLED) {
                ESP_LOGW(TAG,
                         "exact therapy gate unavailable day=%s session=%s: %s",
                         day, sessions[i].id, esp_err_to_name(result));
                result = ESP_ERR_NOT_FOUND;
            }
            break;
        }
        int64_t gate_start = gate.rise_ms > 0 ? gate.rise_ms : gate.mask_on_ms;
        int64_t gate_end = gate.fall_ms > 0 ? gate.fall_ms : gate.mask_off_ms;
        if (gate_start > 0 && gate_end > 0 && gate_end <= gate_start) {
            result = ESP_FAIL;
            break;
        }
        history_therapy_interval_t interval = {0};
        if (!history_eligible_session_interval(&pld, &gate, &interval))
            continue;
        if (count && interval.start_ms <= intervals[count - 1].end_ms) {
            if (interval.end_ms > intervals[count - 1].end_ms)
                intervals[count - 1].end_ms = interval.end_ms;
        } else {
            intervals[count++] = interval;
        }
    }
    if (result == ESP_OK && !count) result = ESP_ERR_NOT_FOUND;
    uint64_t total = 0;
    if (result == ESP_OK) {
        for (size_t i = 0; i < count; ++i)
            total += (uint64_t)(intervals[i].end_ms - intervals[i].start_ms);
        *intervals_out = intervals;
        *interval_count_out = count;
        *eligible_ms_out = total;
        return ESP_OK;
    }
    free(intervals);
    return result;
}

static esp_err_t history_collect_eligible_intervals_leased(
    const char *day, const history_session_info_t *sessions, size_t session_count,
    history_therapy_interval_t **out, size_t *count_out, uint64_t *ms_out,
    const touch_history_operation_t *operation)
{
    if (history_operation_cancelled(operation)) return TOUCH_HISTORY_ERR_CANCELLED;
    history_cache_key_t key = history_cache_key(day, HISTORY_MEM_INTERVALS, 0, 0, 0, false);
    typedef struct { size_t count; uint64_t ms; history_therapy_interval_t items[]; } cached_t;
    size_t bytes = history_cache_get(&key, NULL, 0);
    cached_t *cached = bytes ? history_alloc(bytes, true) : NULL;
    if (cached && history_cache_get(&key, cached, bytes)) {
        history_therapy_interval_t *items = history_alloc(cached->count * sizeof(*items), true);
        if (items) {
            memcpy(items, cached->items, cached->count * sizeof(*items));
            *out = items; *count_out = cached->count; *ms_out = cached->ms;
            free(cached);
            return ESP_OK;
        }
    }
    free(cached);
    esp_err_t result = history_collect_eligible_intervals_uncached(day, sessions,
        session_count, out, count_out, ms_out, operation);
    if (result == ESP_OK && *count_out <= 64 && !history_operation_cancelled(operation)) {
        bytes = sizeof(cached_t) + *count_out * sizeof(**out);
        cached = history_alloc(bytes, true);
        if (cached) {
            cached->count = *count_out; cached->ms = *ms_out;
            memcpy(cached->items, *out, *count_out * sizeof(**out));
            history_cache_put(&key, cached, bytes);
            free(cached);
        }
    }
    return result;
}

/* Exact statistics intentionally use a full integer-domain histogram.  The
 * 128 KiB count table plus read slab live only in PSRAM and are released when
 * one serialized History worker finishes; no source waveform is retained. */
#define HISTORY_STATS_BIN_COUNT 32768U

typedef struct {
    uint32_t counts[HISTORY_STATS_BIN_COUNT];
    int16_t records[HISTORY_READ_VALUES];
    uint64_t sample_count;
    uint64_t below_88_ms;
    int32_t minimum;
    int32_t maximum;
} history_stats_scratch_t;

static history_stats_scratch_t *history_stats_scratch_create(void)
{
    history_stats_scratch_t *scratch = heap_caps_calloc(
        1, sizeof(*scratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (scratch) {
        scratch->minimum = INT32_MAX;
        scratch->maximum = INT32_MIN;
    }
    return scratch;
}

static bool history_interval_contains(
    const history_therapy_interval_t *intervals, size_t interval_count,
    int64_t timestamp_ms)
{
    for (size_t i = 0; i < interval_count; ++i) {
        if (timestamp_ms < intervals[i].start_ms) return false;
        if (timestamp_ms < intervals[i].end_ms) return true;
    }
    return false;
}

static bool history_intervals_overlap(
    const history_therapy_interval_t *intervals, size_t interval_count,
    int64_t start_ms, int64_t end_ms)
{
    if (end_ms <= start_ms) return false;
    for (size_t i = 0; i < interval_count; ++i) {
        if (intervals[i].end_ms <= start_ms) continue;
        if (intervals[i].start_ms >= end_ms) return false;
        return true;
    }
    return false;
}

static uint64_t history_interval_overlap_ms(
    const history_therapy_interval_t *intervals, size_t interval_count,
    int64_t start_ms, int64_t end_ms)
{
    uint64_t total = 0;
    if (end_ms <= start_ms) return 0;
    for (size_t i = 0; i < interval_count; ++i) {
        int64_t start = intervals[i].start_ms > start_ms
            ? intervals[i].start_ms : start_ms;
        int64_t end = intervals[i].end_ms < end_ms
            ? intervals[i].end_ms : end_ms;
        if (end > start) total += (uint64_t)(end - start);
        if (intervals[i].start_ms >= end_ms) break;
    }
    return total;
}

static bool history_stats_add(history_stats_scratch_t *scratch,
                              int32_t value_x100)
{
    if (!scratch || value_x100 < 0 ||
        value_x100 >= (int32_t)HISTORY_STATS_BIN_COUNT ||
        scratch->counts[value_x100] == UINT32_MAX ||
        scratch->sample_count == UINT64_MAX) return false;
    scratch->counts[value_x100]++;
    scratch->sample_count++;
    if (value_x100 < scratch->minimum) scratch->minimum = value_x100;
    if (value_x100 > scratch->maximum) scratch->maximum = value_x100;
    return true;
}

static bool history_stats_set_percentile(
    const history_stats_scratch_t *scratch, uint16_t per_mille,
    touch_history_stat_value_t *value)
{
    int32_t result = 0;
    if (!scratch || !value || scratch->sample_count < 5U ||
        !touch_history_weighted_percentile_histogram(
            scratch->counts, HISTORY_STATS_BIN_COUNT, 0,
            scratch->sample_count, per_mille, &result)) return false;
    value->value_x100 = result;
    value->available = true;
    return true;
}

static void history_stats_prepare(touch_history_stats_t *stats,
                                  touch_history_signal_t signal,
                                  int64_t start_ms, int64_t end_ms,
                                  bool therapy_only)
{
    memset(stats, 0, sizeof(*stats));
    stats->signal = signal;
    stats->start_ms = start_ms;
    stats->end_ms = end_ms;
    stats->therapy_only = therapy_only;
    switch (signal) {
    case TOUCH_HISTORY_SIGNAL_FLOW:
        stats->value_count = 3;
        stats->values[0].kind = TOUCH_HISTORY_STAT_ABSOLUTE_P50;
        stats->values[1].kind = TOUCH_HISTORY_STAT_ABSOLUTE_P95;
        stats->values[2].kind = TOUCH_HISTORY_STAT_ABSOLUTE_P995;
        break;
    case TOUCH_HISTORY_SIGNAL_PRESSURE:
    case TOUCH_HISTORY_SIGNAL_LEAK:
    case TOUCH_HISTORY_SIGNAL_FLOW_LIMIT:
    case TOUCH_HISTORY_SIGNAL_SNORE:
        stats->value_count = 3;
        stats->values[0].kind = TOUCH_HISTORY_STAT_P50;
        stats->values[1].kind = TOUCH_HISTORY_STAT_P95;
        stats->values[2].kind = TOUCH_HISTORY_STAT_P995;
        break;
    case TOUCH_HISTORY_SIGNAL_SPO2:
        stats->value_count = 4;
        stats->values[0].kind = TOUCH_HISTORY_STAT_MINIMUM;
        stats->values[1].kind = TOUCH_HISTORY_STAT_P5;
        stats->values[2].kind = TOUCH_HISTORY_STAT_P05;
        stats->values[3].kind = TOUCH_HISTORY_STAT_TIME_BELOW_88;
        break;
    case TOUCH_HISTORY_SIGNAL_PULSE:
        stats->value_count = 3;
        stats->values[0].kind = TOUCH_HISTORY_STAT_MINIMUM;
        stats->values[1].kind = TOUCH_HISTORY_STAT_MEDIAN;
        stats->values[2].kind = TOUCH_HISTORY_STAT_MAXIMUM;
        break;
    case TOUCH_HISTORY_SIGNAL_MOTION:
    case TOUCH_HISTORY_SIGNAL_COUNT:
        break;
    }
}

static void history_stats_finalize(history_stats_scratch_t *scratch,
                                   touch_history_stats_t *stats)
{
    stats->sample_count = scratch->sample_count;
    if (scratch->sample_count < 5U) return;
    switch (stats->signal) {
    case TOUCH_HISTORY_SIGNAL_FLOW:
        (void)history_stats_set_percentile(scratch, 500, &stats->values[0]);
        (void)history_stats_set_percentile(scratch, 950, &stats->values[1]);
        (void)history_stats_set_percentile(scratch, 995, &stats->values[2]);
        break;
    case TOUCH_HISTORY_SIGNAL_PRESSURE:
    case TOUCH_HISTORY_SIGNAL_LEAK:
    case TOUCH_HISTORY_SIGNAL_FLOW_LIMIT:
    case TOUCH_HISTORY_SIGNAL_SNORE:
        (void)history_stats_set_percentile(scratch, 500, &stats->values[0]);
        (void)history_stats_set_percentile(scratch, 950, &stats->values[1]);
        (void)history_stats_set_percentile(scratch, 995, &stats->values[2]);
        break;
    case TOUCH_HISTORY_SIGNAL_SPO2:
        stats->values[0].value_x100 = scratch->minimum;
        stats->values[0].available = true;
        (void)history_stats_set_percentile(scratch, 50, &stats->values[1]);
        (void)history_stats_set_percentile(scratch, 5, &stats->values[2]);
        /* x100 minutes: milliseconds / 600 is exact to 0.01 minute. */
        stats->values[3].value_x100 = scratch->below_88_ms / 600U > INT32_MAX
            ? INT32_MAX : (int32_t)(scratch->below_88_ms / 600U);
        stats->values[3].available = true;
        break;
    case TOUCH_HISTORY_SIGNAL_PULSE:
        stats->values[0].value_x100 = scratch->minimum;
        stats->values[0].available = true;
        (void)history_stats_set_percentile(scratch, 500, &stats->values[1]);
        stats->values[2].value_x100 = scratch->maximum;
        stats->values[2].available = true;
        break;
    default:
        break;
    }
    stats->exact = true;
}

static esp_err_t history_stats_accumulate_as11(
    const char *day, touch_history_signal_t signal,
    const history_session_info_t *sessions, size_t session_count,
    const history_therapy_interval_t *eligible, size_t eligible_count,
    int64_t start_ms, int64_t end_ms, history_stats_scratch_t *scratch,
    const touch_history_operation_t *operation)
{
    int channel = signal == TOUCH_HISTORY_SIGNAL_FLOW
        ? 0 : history_pld_channel(signal);
    if (channel < 0) return ESP_ERR_INVALID_ARG;
    for (size_t s = 0; s < session_count; ++s) {
        if (history_operation_cancelled(operation))
            return TOUCH_HISTORY_ERR_CANCELLED;
        int64_t session_start = sessions[s].start_ms > start_ms
            ? sessions[s].start_ms : start_ms;
        int64_t session_end = sessions[s].end_ms < end_ms
            ? sessions[s].end_ms : end_ms;
        if (!history_intervals_overlap(eligible, eligible_count,
                                       session_start, session_end)) {
            history_operation_progress(operation, history_progress_fraction(
                250, 900, s + 1, session_count));
            continue;
        }

        trace_candidate_t candidate = {0};
        esp_err_t inspect = signal == TOUCH_HISTORY_SIGNAL_FLOW
            ? history_flow_raw_candidate(day, &sessions[s], &candidate)
            : history_session_candidate(day, &sessions[s], signal, &candidate);
        /* Exact Flow statistics are never manufactured from the min/max
         * sidecar. A missing source for any eligible contributing session
         * invalidates the result instead of silently returning a partial. */
        if (inspect != ESP_OK) {
            if (inspect != ESP_ERR_NO_MEM &&
                inspect != TOUCH_HISTORY_ERR_CANCELLED)
                ESP_LOGW(TAG,
                         "exact stats source unavailable day=%s session=%s signal=%u: %s",
                         day, sessions[s].id, (unsigned)signal,
                         esp_err_to_name(inspect));
            return (inspect == ESP_ERR_NO_MEM ||
                    inspect == TOUCH_HISTORY_ERR_CANCELLED)
                       ? inspect : ESP_ERR_NOT_FOUND;
        }
        uint32_t first = history_as11_record_at_or_after(&candidate, start_ms);
        uint32_t end = history_as11_record_at_or_after(&candidate, end_ms);
        if (end <= first) continue;
        uint64_t byte_offset = (uint64_t)candidate.header_bytes +
            (uint64_t)first * candidate.n_channels * sizeof(int16_t);
        FILE *file = fopen(candidate.path, "rb");
        if (!file || byte_offset > LONG_MAX ||
            fseek(file, (long)byte_offset, SEEK_SET) != 0) {
            if (file) fclose(file);
            return ESP_FAIL;
        }
        size_t per_read = HISTORY_READ_VALUES / candidate.n_channels;
        uint32_t processed = first;
        esp_err_t result = ESP_OK;
        while (processed < end) {
            if (history_operation_cancelled(operation)) {
                result = TOUCH_HISTORY_ERR_CANCELLED;
                break;
            }
            size_t wanted = end - processed;
            if (wanted > per_read) wanted = per_read;
            size_t got = fread(scratch->records,
                               candidate.n_channels * sizeof(int16_t),
                               wanted, file);
            for (size_t r = 0; r < got; ++r) {
                uint32_t record_index = processed + (uint32_t)r;
                int64_t timestamp_ms = candidate.start_epoch_ms +
                    (int64_t)((uint64_t)record_index * 10000U /
                              candidate.sample_hz_x10);
                if (timestamp_ms < start_ms || timestamp_ms >= end_ms ||
                    !history_interval_contains(eligible, eligible_count,
                                               timestamp_ms)) continue;
                int16_t *record = &scratch->records[r * candidate.n_channels];
                bool signed_signal = signal == TOUCH_HISTORY_SIGNAL_FLOW;
                if (!history_raw_value_valid(record[channel],
                                              candidate.version,
                                              signed_signal)) continue;
                int16_t scaled = 0;
                if (!history_scale_x100(signal, record[channel], &scaled))
                    continue;
                int32_t value = scaled;
                if (signal == TOUCH_HISTORY_SIGNAL_FLOW && value < 0)
                    value = -value;
                if (!history_stats_add(scratch, value)) {
                    result = ESP_FAIL;
                    break;
                }
            }
            processed += (uint32_t)got;
            if (got != wanted || ferror(file)) result = ESP_FAIL;
            if (result != ESP_OK) break;
        }
        if (fclose(file) != 0 && result == ESP_OK) result = ESP_FAIL;
        if (result != ESP_OK) return result;
        history_operation_progress(operation, history_progress_fraction(
            250, 900, s + 1, session_count));
    }
    return scratch->sample_count >= 5U ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static bool history_ox_timestamp_owned_by_later(
    const trace_candidate_t *candidates, size_t count, size_t current,
    int64_t timestamp_ms)
{
    for (size_t i = current + 1; i < count; ++i) {
        if (candidates[i].start_epoch_ms > timestamp_ms) break;
        if (history_candidate_end_ms(&candidates[i]) > timestamp_ms)
            return true;
    }
    return false;
}

static int64_t history_ox_sample_end(
    const trace_candidate_t *candidates, size_t count, size_t current,
    uint32_t record_index, int64_t timestamp_ms, int64_t range_end_ms)
{
    const trace_candidate_t *candidate = &candidates[current];
    uint64_t next_offset_us = (uint64_t)(record_index + 1U) *
                              candidate->period_num_us /
                              candidate->period_den;
    int64_t end_ms = candidate->start_epoch_ms +
                     (int64_t)(next_offset_us / 1000U);
    if (end_ms <= timestamp_ms) end_ms = timestamp_ms + 1;
    int64_t candidate_end = history_candidate_end_ms(candidate);
    if (end_ms > candidate_end) end_ms = candidate_end;
    if (end_ms > range_end_ms) end_ms = range_end_ms;
    for (size_t i = current + 1; i < count; ++i) {
        if (candidates[i].start_epoch_ms <= timestamp_ms) continue;
        if (candidates[i].start_epoch_ms >= end_ms) break;
        end_ms = candidates[i].start_epoch_ms;
        break;
    }
    return end_ms;
}

static esp_err_t history_stats_accumulate_oximetry(
    const char *day, touch_history_signal_t signal,
    const history_therapy_interval_t *eligible, size_t eligible_count,
    int64_t start_ms, int64_t end_ms, bool therapy_only,
    history_stats_scratch_t *scratch,
    const touch_history_operation_t *operation)
{
    trace_candidate_t *candidates = NULL;
    size_t candidate_count = 0;
    esp_err_t result = history_collect_ox_candidates(
        day, signal, start_ms, end_ms, true, &candidates, &candidate_count,
        operation);
    if (result != ESP_OK) return result;
    uint64_t total_records = 0;
    for (size_t i = 0; i < candidate_count; ++i)
        total_records += candidates[i].records;
    uint64_t processed_total = 0;
    for (size_t c = 0; c < candidate_count && result == ESP_OK; ++c) {
        const trace_candidate_t *candidate = &candidates[c];
        FILE *file = fopen(candidate->path, "rb");
        if (!file || fseek(file, candidate->header_bytes, SEEK_SET) != 0) {
            if (file) fclose(file);
            result = ESP_FAIL;
            break;
        }
        size_t per_read = HISTORY_READ_VALUES / candidate->n_channels;
        uint32_t processed = 0;
        while (processed < candidate->records) {
            if (history_operation_cancelled(operation)) {
                result = TOUCH_HISTORY_ERR_CANCELLED;
                break;
            }
            size_t wanted = candidate->records - processed;
            if (wanted > per_read) wanted = per_read;
            size_t got = fread(scratch->records,
                               candidate->n_channels * sizeof(int16_t),
                               wanted, file);
            for (size_t r = 0; r < got; ++r) {
                uint32_t record_index = processed + (uint32_t)r;
                uint64_t offset_us = (uint64_t)record_index *
                                     candidate->period_num_us /
                                     candidate->period_den;
                int64_t timestamp_ms = candidate->start_epoch_ms +
                                       (int64_t)(offset_us / 1000U);
                if (timestamp_ms < start_ms || timestamp_ms >= end_ms ||
                    history_ox_timestamp_owned_by_later(
                        candidates, candidate_count, c, timestamp_ms))
                    continue;
                if (therapy_only && !history_interval_contains(
                        eligible, eligible_count, timestamp_ms)) continue;
                int16_t *record = &scratch->records[r * candidate->n_channels];
                int16_t scaled = 0;
                if (!history_ox_record_value(signal, record, &scaled)) continue;
                if (!history_stats_add(scratch, scaled)) {
                    result = ESP_FAIL;
                    break;
                }
                if (signal == TOUCH_HISTORY_SIGNAL_SPO2 && scaled < 8800) {
                    int64_t sample_end = history_ox_sample_end(
                        candidates, candidate_count, c, record_index,
                        timestamp_ms, end_ms);
                    if (sample_end > timestamp_ms) {
                        scratch->below_88_ms += therapy_only
                            ? history_interval_overlap_ms(
                                  eligible, eligible_count, timestamp_ms,
                                  sample_end)
                            : (uint64_t)(sample_end - timestamp_ms);
                    }
                }
            }
            processed += (uint32_t)got;
            processed_total += got;
            history_operation_progress(operation, history_progress_fraction(
                250, 900, processed_total, total_records));
            if (got != wanted || ferror(file)) result = ESP_FAIL;
            if (result != ESP_OK) break;
        }
        if (fclose(file) != 0 && result == ESP_OK) result = ESP_FAIL;
    }
    free(candidates);
    if (result != ESP_OK) return result;
    return scratch->sample_count >= 5U ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t history_load_stats_leased(
    const char *day, touch_history_signal_t signal,
    int64_t start_ms, int64_t end_ms, bool therapy_only,
    touch_history_stats_t *stats,
    const touch_history_operation_t *operation)
{
    if (signal == TOUCH_HISTORY_SIGNAL_MOTION) {
        stats->loaded = true;
        return ESP_OK;
    }
    history_session_info_t *sessions = NULL;
    size_t session_count = 0;
    esp_err_t sessions_result = history_load_sessions_leased(
        day, &sessions, &session_count, NULL, operation);
    if (sessions_result == ESP_ERR_NOT_FOUND &&
        (signal == TOUCH_HISTORY_SIGNAL_SPO2 ||
         signal == TOUCH_HISTORY_SIGNAL_PULSE)) sessions_result = ESP_OK;
    if (sessions_result != ESP_OK) return sessions_result;
    if (session_count) {
        sessions_result = history_resolve_session_ends(
            day, sessions, session_count);
        if (sessions_result != ESP_OK) {
            free(sessions);
            return sessions_result;
        }
    }
    int64_t night_start_ms = 0;
    int64_t night_end_ms = 0;
    sessions_result = history_unified_axis(
        day, sessions, session_count, &night_start_ms, &night_end_ms,
        NULL, NULL, operation);
    if (sessions_result != ESP_OK) {
        free(sessions);
        return sessions_result;
    }
    if (start_ms < night_start_ms || end_ms > night_end_ms) {
        free(sessions);
        return ESP_ERR_INVALID_ARG;
    }

    history_therapy_interval_t *eligible = NULL;
    size_t eligible_count = 0;
    uint64_t eligible_ms = 0;
    bool needs_therapy = signal <= TOUCH_HISTORY_SIGNAL_SNORE ||
                         (signal == TOUCH_HISTORY_SIGNAL_SPO2 && therapy_only);
    esp_err_t eligible_result = needs_therapy
        ? history_collect_eligible_intervals_leased(
              day, sessions, session_count, &eligible, &eligible_count,
              &eligible_ms, operation)
        : ESP_OK;
    if (eligible_result != ESP_OK) {
        free(sessions);
        return eligible_result;
    }
    (void)eligible_ms;
    history_stats_scratch_t *scratch = history_stats_scratch_create();
    if (!scratch) {
        free(eligible);
        free(sessions);
        return ESP_ERR_NO_MEM;
    }
    history_operation_progress(operation, 250);
    esp_err_t result = signal <= TOUCH_HISTORY_SIGNAL_SNORE
        ? history_stats_accumulate_as11(
              day, signal, sessions, session_count, eligible, eligible_count,
              start_ms, end_ms, scratch, operation)
        : history_stats_accumulate_oximetry(
              day, signal, eligible, eligible_count, start_ms, end_ms,
              therapy_only && signal == TOUCH_HISTORY_SIGNAL_SPO2,
              scratch, operation);
    if (result == ESP_OK) {
        history_stats_finalize(scratch, stats);
        stats->source_raw = signal == TOUCH_HISTORY_SIGNAL_FLOW;
        stats->loaded = true;
        history_operation_progress(operation, 1000);
    } else if (result == ESP_ERR_NOT_FOUND) {
        stats->loaded = true;
    }
    heap_caps_free(scratch);
    free(eligible);
    free(sessions);
    return result;
}

esp_err_t touch_history_load_stats(
    const char *day, touch_history_signal_t signal,
    int64_t start_ms, int64_t end_ms, bool therapy_only,
    touch_history_stats_t *stats)
{
    return touch_history_load_stats_ex(
        day, signal, start_ms, end_ms, therapy_only, stats, NULL);
}

esp_err_t touch_history_load_stats_ex(
    const char *day, touch_history_signal_t signal,
    int64_t start_ms, int64_t end_ms, bool therapy_only,
    touch_history_stats_t *stats,
    const touch_history_operation_t *operation)
{
    if (!valid_day(day) || signal < TOUCH_HISTORY_SIGNAL_FLOW ||
        signal >= TOUCH_HISTORY_SIGNAL_COUNT || start_ms <= 0 ||
        end_ms <= start_ms || end_ms - start_ms > HISTORY_AXIS_MAX_MS ||
        !stats) return ESP_ERR_INVALID_ARG;
    history_stats_prepare(stats, signal, start_ms, end_ms,
                          therapy_only && signal == TOUCH_HISTORY_SIGNAL_SPO2);
    history_operation_progress(operation, 0);
    esp_err_t lease = history_lease_acquire_operation(operation);
    if (lease != ESP_OK) return lease;
    history_cache_key_t key = history_cache_key(day, HISTORY_MEM_STATS,
        signal, start_ms, end_ms, therapy_only);
    esp_err_t result = ESP_OK;
    if (!history_cache_get(&key, stats, sizeof(*stats))) {
        result = history_load_stats_leased(day, signal, start_ms, end_ms,
            therapy_only && signal == TOUCH_HISTORY_SIGNAL_SPO2, stats, operation);
        if (result == ESP_OK && !history_operation_cancelled(operation))
            history_cache_put(&key, stats, sizeof(*stats));
    }
    if (history_operation_cancelled(operation)) result = TOUCH_HISTORY_ERR_CANCELLED;
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    return result;
}

static esp_err_t history_collect_events_uncached(
    const char *day, const history_session_info_t *sessions,
    size_t session_count, history_event_vector_t *events,
    touch_history_event_totals_t *totals,
    const touch_history_operation_t *operation,
    uint16_t progress_start, uint16_t progress_end)
{
    if (!day || (session_count && !sessions) || !events || !totals)
        return ESP_ERR_INVALID_ARG;
    if (!session_count) return ESP_ERR_NOT_FOUND;
    if (session_count > SIZE_MAX / sizeof(history_therapy_interval_t))
        return ESP_ERR_NO_MEM;
    history_therapy_interval_t *intervals = history_alloc(
        session_count * sizeof(*intervals), true);
    if (!intervals) return ESP_ERR_NO_MEM;
    size_t interval_count = 0;
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < session_count; ++i) {
        if (history_operation_cancelled(operation)) {
            result = TOUCH_HISTORY_ERR_CANCELLED;
            break;
        }
        trace_candidate_t pld = {0};
        esp_err_t inspect = history_session_candidate(
            day, &sessions[i], TOUCH_HISTORY_SIGNAL_PRESSURE, &pld);
        if (inspect == ESP_ERR_NOT_FOUND) {
            /* <60 s sessions without a PLD record are non-therapy fragments.
             * A longer breathing capture with no denominator is incomplete. */
            if (sessions[i].brp_samples >= 1500U) {
                result = ESP_ERR_NOT_FOUND;
                break;
            }
            continue;
        }
        if (inspect != ESP_OK) {
            result = (inspect == ESP_ERR_NO_MEM ||
                      inspect == TOUCH_HISTORY_ERR_CANCELLED)
                         ? inspect : ESP_ERR_NOT_FOUND;
            break;
        }
        if (pld.records < pld.sample_hz_x10 * 6U) continue;

        char path[384];
        if (snprintf(path, sizeof(path), "%s/%s/%s_events.snt",
                     SD_STREAMS_DIR, day, sessions[i].id) >=
            (int)sizeof(path)) {
            result = ESP_FAIL;
            break;
        }
        size_t saved_count = events->count;
        uint32_t saved_counts[TOUCH_HISTORY_EVENT_TYPE_COUNT];
        memcpy(saved_counts, events->counts, sizeof(saved_counts));
        history_gate_t gate = {0};
        esp_err_t parse = history_parse_event_file(
            path, &sessions[i], i, &gate, events, operation);
        if (parse != ESP_OK) {
            if (parse != ESP_ERR_NO_MEM &&
                parse != TOUCH_HISTORY_ERR_CANCELLED)
                ESP_LOGW(TAG, "event source unavailable day=%s session=%s: %s",
                         day, sessions[i].id, esp_err_to_name(parse));
            result = (parse == ESP_ERR_NO_MEM ||
                      parse == TOUCH_HISTORY_ERR_CANCELLED)
                         ? parse : ESP_ERR_NOT_FOUND;
            break;
        }
        int64_t gate_start = gate.rise_ms > 0 ? gate.rise_ms : gate.mask_on_ms;
        int64_t gate_end = gate.fall_ms > 0 ? gate.fall_ms : gate.mask_off_ms;
        if (gate_start > 0 && gate_end > 0 && gate_end <= gate_start) {
            result = ESP_FAIL;
            break;
        }
        history_therapy_interval_t interval = {0};
        if (!history_eligible_session_interval(&pld, &gate, &interval)) {
            events->count = saved_count;
            memcpy(events->counts, saved_counts, sizeof(saved_counts));
            continue;
        }

        /* Respiratory notifications outside the same gate used for the
         * denominator must not inflate ST AHI. Keep the event whose report/end
         * time falls inside eligible therapy. */
        size_t write = saved_count;
        memcpy(events->counts, saved_counts, sizeof(saved_counts));
        for (size_t read = saved_count; read < events->count; ++read) {
            touch_history_event_t event = events->items[read];
            if (event.end_ms < interval.start_ms ||
                event.end_ms > interval.end_ms) continue;
            events->items[write++] = event;
            if (event.type >= 0 && event.type < TOUCH_HISTORY_EVENT_TYPE_COUNT &&
                events->counts[event.type] < UINT32_MAX)
                events->counts[event.type]++;
        }
        events->count = write;

        if (interval_count &&
            interval.start_ms <= intervals[interval_count - 1].end_ms) {
            if (interval.end_ms > intervals[interval_count - 1].end_ms)
                intervals[interval_count - 1].end_ms = interval.end_ms;
        } else {
            intervals[interval_count++] = interval;
        }
        history_operation_progress(operation, history_progress_fraction(
            progress_start, progress_end, i + 1, session_count));
    }
    uint64_t eligible_ms = 0;
    if (result == ESP_OK) {
        if (!interval_count) {
            result = ESP_ERR_NOT_FOUND;
        } else {
            for (size_t i = 0; i < interval_count; ++i) {
                uint64_t duration = (uint64_t)(intervals[i].end_ms -
                                               intervals[i].start_ms);
                if (eligible_ms > UINT64_MAX - duration) {
                    result = ESP_FAIL;
                    break;
                }
                eligible_ms += duration;
            }
        }
    }
    if (result == ESP_OK) {
        size_t before_dedup = events->count;
        events->count = touch_history_deduplicate_events(
            events->items, events->count);
        if (events->count < before_dedup)
            ESP_LOGI(TAG, "day=%s deduplicated %u replayed respiratory events",
                     day, (unsigned)(before_dedup - events->count));
        memset(events->counts, 0, sizeof(events->counts));
        for (size_t i = 0; i < events->count; ++i) {
            touch_history_event_type_t type = events->items[i].type;
            if (type >= 0 && type < TOUCH_HISTORY_EVENT_TYPE_COUNT &&
                events->counts[type] < UINT32_MAX)
                events->counts[type]++;
        }
        (void)touch_history_compute_event_indices(
            events->counts, eligible_ms, totals);
        for (size_t i = 0; i < session_count; ++i) {
            if (!sessions[i].event_dropped) continue;
            totals->complete = false;
            totals->has_indices = false;
            ESP_LOGW(TAG,
                     "day=%s session=%s dropped %u event notifications; ST AHI unavailable",
                     day, sessions[i].id,
                     (unsigned)sessions[i].event_dropped);
        }
    }
    free(intervals);
    return result;
}

static esp_err_t history_collect_events_leased(
    const char *day, const history_session_info_t *sessions,
    size_t session_count, history_event_vector_t *events,
    touch_history_event_totals_t *totals,
    const touch_history_operation_t *operation,
    uint16_t progress_start, uint16_t progress_end)
{
    if (history_operation_cancelled(operation)) return TOUCH_HISTORY_ERR_CANCELLED;
    history_cache_key_t key = history_cache_key(day, HISTORY_MEM_EVENTS, 0, 0, 0, false);
    typedef struct { touch_history_event_totals_t totals; size_t count;
                     touch_history_event_t items[]; } cached_t;
    size_t bytes = history_cache_get(&key, NULL, 0);
    cached_t *cached = bytes ? history_alloc(bytes, true) : NULL;
    if (cached && history_cache_get(&key, cached, bytes)) {
        touch_history_event_t *items = cached->count
            ? history_alloc(cached->count * sizeof(*items), true) : NULL;
        if (items || !cached->count) {
            if (items) memcpy(items, cached->items, cached->count * sizeof(*items));
            events->items = items; events->count = events->capacity = cached->count;
            memcpy(events->counts, cached->totals.count, sizeof(events->counts));
            *totals = cached->totals;
            free(cached);
            return ESP_OK;
        }
    }
    free(cached);
    esp_err_t result = history_collect_events_uncached(day, sessions, session_count,
        events, totals, operation, progress_start, progress_end);
    if (result == ESP_OK && events->count <= 1024 && !history_operation_cancelled(operation)) {
        bytes = sizeof(cached_t) + events->count * sizeof(*events->items);
        cached = history_alloc(bytes, true);
        if (cached) {
            cached->totals = *totals; cached->count = events->count;
            if (events->count) memcpy(cached->items, events->items,
                                     events->count * sizeof(*events->items));
            history_cache_put(&key, cached, bytes);
            free(cached);
        }
    }
    return result;
}

static esp_err_t history_load_events_leased(
    const char *day, size_t offset, touch_history_event_t *out,
    size_t capacity, touch_history_event_page_t *page,
    const touch_history_operation_t *operation,
    uint16_t progress_start, uint16_t progress_end)
{
    history_operation_progress(operation, progress_start);
    history_session_info_t *sessions = NULL;
    size_t session_count = 0;
    size_t skipped_sessions = 0;
    esp_err_t result = history_load_sessions_leased(
        day, &sessions, &session_count, &skipped_sessions, operation);
    history_event_vector_t vector = {0};
    touch_history_event_totals_t totals = {0};
    if (result == ESP_OK)
        result = history_collect_events_leased(
            day, sessions, session_count, &vector, &totals, operation,
            history_progress_fraction(progress_start, progress_end, 1, 10),
            progress_end);
    if (result == ESP_OK) {
        size_t available = offset < vector.count ? vector.count - offset : 0;
        size_t returned = available < capacity ? available : capacity;
        if (returned) memcpy(out, &vector.items[offset],
                             returned * sizeof(*out));
        page->offset = offset;
        page->returned = returned;
        page->total_count = vector.count;
        page->has_more = offset + returned < vector.count;
        page->totals = totals;
        if (skipped_sessions) {
            page->totals.complete = false;
            page->totals.has_indices = false;
        }
    }
    free(vector.items);
    free(sessions);
    if (result == ESP_OK) history_operation_progress(operation, progress_end);
    return result;
}

esp_err_t touch_history_load_events(const char *day, size_t offset,
                                    touch_history_event_t *events,
                                    size_t capacity,
                                    touch_history_event_page_t *page)
{
    return touch_history_load_events_ex(
        day, offset, events, capacity, page, NULL);
}

esp_err_t touch_history_load_events_ex(
    const char *day, size_t offset, touch_history_event_t *events,
    size_t capacity, touch_history_event_page_t *page,
    const touch_history_operation_t *operation)
{
    if (!valid_day(day) || !page || (capacity && !events) ||
        capacity > SIZE_MAX / sizeof(*events))
        return ESP_ERR_INVALID_ARG;
    memset(page, 0, sizeof(*page));
    history_operation_progress(operation, 0);
    esp_err_t lease = history_lease_acquire_operation(operation);
    if (lease != ESP_OK) return lease;
    esp_err_t result = history_load_events_leased(
        day, offset, events, capacity, page, operation, 50, 1000);
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    return result;
}

esp_err_t touch_history_load_window_events_ex(const char *day,
    int64_t start_ms, int64_t end_ms, touch_history_event_t *events,
    size_t capacity, touch_history_event_page_t *page,
    const touch_history_operation_t *operation)
{
    if (!valid_day(day) || !page || (capacity && !events) || end_ms <= start_ms)
        return ESP_ERR_INVALID_ARG;
    memset(page, 0, sizeof(*page));
    esp_err_t result = history_lease_acquire_operation(operation);
    if (result != ESP_OK) return result;
    history_session_info_t *sessions = NULL;
    size_t count = 0, skipped = 0;
    history_event_vector_t vector = {0};
    result = history_load_sessions_leased(day, &sessions, &count, &skipped, operation);
    if (result == ESP_OK)
        result = history_collect_events_leased(day, sessions, count, &vector,
                                               &page->totals, operation, 0, 1000);
    if (result == ESP_OK) {
        page->total_count = vector.count;
        if (skipped) page->totals.complete = page->totals.has_indices = false;
        for (size_t i = 0; i < vector.count; ++i) {
            if (history_operation_cancelled(operation)) { result = TOUCH_HISTORY_ERR_CANCELLED; break; }
            const touch_history_event_t *event = &vector.items[i];
            if (event->end_ms < start_ms || event->start_ms >= end_ms) continue;
            if (page->returned < capacity) events[page->returned++] = *event;
            else page->has_more = true;
        }
    }
    free(vector.items);
    free(sessions);
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    return result;
}

static int history_day_newest_first(const void *left, const void *right)
{
    const history_day_index_t *a = left;
    const history_day_index_t *b = right;
    return strcmp(b->day, a->day);
}

static esp_err_t history_day_index_get(
    history_day_index_t **days, size_t *count, size_t *capacity,
    const char *day, history_day_index_t **entry_out)
{
    if (!days || !count || !capacity || !valid_day(day) || !entry_out)
        return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < *count; ++i) {
        if (!strcmp((*days)[i].day, day)) {
            *entry_out = &(*days)[i];
            return ESP_OK;
        }
    }
    if (*count == *capacity) {
        size_t next_capacity = *capacity ? *capacity * 2
                                         : HISTORY_INDEX_INITIAL_CAPACITY;
        if (next_capacity <= *capacity) return ESP_ERR_NO_MEM;
        history_day_index_t *grown = history_grow_array(
            *days, *capacity, next_capacity, sizeof(**days));
        if (!grown) return ESP_ERR_NO_MEM;
        *days = grown;
        *capacity = next_capacity;
    }
    history_day_index_t *entry = &(*days)[(*count)++];
    memset(entry, 0, sizeof(*entry));
    strlcpy(entry->day, day, sizeof(entry->day));
    *entry_out = entry;
    return ESP_OK;
}

static esp_err_t history_collect_days_leased(
    history_day_index_t **days_out, size_t *count_out,
    const touch_history_operation_t *operation, const char *month_prefix)
{
    if (!days_out || !count_out) return ESP_ERR_INVALID_ARG;
    *days_out = NULL;
    *count_out = 0;
    history_day_index_t *days = NULL;
    size_t count = 0;
    size_t capacity = 0;
    esp_err_t result = ESP_OK;
    DIR *dir = opendir(SD_STREAMS_DIR);
    if (!dir && errno != ENOENT && errno != ENOTDIR) result = ESP_FAIL;
    while (dir && result == ESP_OK) {
        if (history_operation_cancelled(operation)) {
            result = TOUCH_HISTORY_ERR_CANCELLED;
            break;
        }
        errno = 0;
        struct dirent *directory = readdir(dir);
        if (!directory) {
            if (errno != 0) result = ESP_FAIL;
            break;
        }
        if (!valid_day(directory->d_name)) continue;
        if (month_prefix && strncmp(directory->d_name, month_prefix, 6))
            continue;
        history_session_info_t *sessions = NULL;
        size_t session_count_value = 0;
        size_t skipped_sessions = 0;
        esp_err_t load = history_load_sessions_leased(
            directory->d_name, &sessions, &session_count_value,
            &skipped_sessions, operation);
        free(sessions);
        if (load == ESP_ERR_NOT_FOUND) continue;
        if (load != ESP_OK) {
            result = load;
            break;
        }
        history_day_index_t *entry = NULL;
        result = history_day_index_get(
            &days, &count, &capacity, directory->d_name, &entry);
        if (result != ESP_OK) break;
        entry->sessions = session_count_value > INT_MAX
            ? INT_MAX : (int)session_count_value;
        entry->skipped_sessions = skipped_sessions > UINT16_MAX
            ? UINT16_MAX : (uint16_t)skipped_sessions;
        entry->has_therapy = true;
    }
    if (dir && closedir(dir) != 0 && result == ESP_OK) result = ESP_FAIL;

    char ox_root[OXIMETRY_CANONICAL_MAX_PATH];
    if (snprintf(ox_root, sizeof(ox_root), SD_OXYMETRY_DIR "/recordings") >=
        (int)sizeof(ox_root)) result = ESP_FAIL;
    DIR *ox_dir = result == ESP_OK ? opendir(ox_root) : NULL;
    if (!ox_dir && result == ESP_OK && errno != ENOENT && errno != ENOTDIR)
        result = ESP_FAIL;
    while (ox_dir && result == ESP_OK) {
        if (history_operation_cancelled(operation)) {
            result = TOUCH_HISTORY_ERR_CANCELLED;
            break;
        }
        errno = 0;
        struct dirent *directory = readdir(ox_dir);
        if (!directory) {
            if (errno != 0) result = ESP_FAIL;
            break;
        }
        if (!valid_day(directory->d_name)) continue;
        if (month_prefix && strncmp(directory->d_name, month_prefix, 6))
            continue;
        esp_err_t found = history_ox_metadata(
            directory->d_name, NULL, NULL, NULL, operation);
        if (found == ESP_ERR_NOT_FOUND) continue;
        if (found != ESP_OK) {
            result = found;
            break;
        }
        history_day_index_t *entry = NULL;
        result = history_day_index_get(
            &days, &count, &capacity, directory->d_name, &entry);
        if (result != ESP_OK) break;
        entry->has_oximetry = true;
    }
    if (ox_dir && closedir(ox_dir) != 0 && result == ESP_OK)
        result = ESP_FAIL;
    if (result != ESP_OK) {
        free(days);
        return result;
    }
    if (!count) {
        free(days);
        return ESP_ERR_NOT_FOUND;
    }
    qsort(days, count, sizeof(*days), history_day_newest_first);
    *days_out = days;
    *count_out = count;
    return ESP_OK;
}

static esp_err_t history_fill_day_summary(touch_history_day_t *day)
{
    if (!day || !valid_day(day->day)) return ESP_ERR_INVALID_ARG;
    char path[320];
    if (snprintf(path, sizeof(path), "%s/%s.spool", SD_SUMMARIES_DIR,
                 day->day) >= (int)sizeof(path)) return ESP_FAIL;
    struct stat st;
    if (stat(path, &st) != 0)
        return errno == ENOENT || errno == ENOTDIR ? ESP_OK : ESP_FAIL;
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > HISTORY_SUMMARY_MAX_BYTES) return ESP_FAIL;
    FILE *file = fopen(path, "rb");
    if (!file) return ESP_FAIL;
    uint8_t *record = history_alloc((size_t)st.st_size, false);
    if (!record) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(record, 1, (size_t)st.st_size, file);
    bool read_ok = got == (size_t)st.st_size && !ferror(file);
    if (fclose(file) != 0) read_ok = false;
    bool decoded = read_ok &&
                   touch_history_decode_summary_record(record, got, day);
    free(record);
    return decoded ? ESP_OK : ESP_FAIL;
}

static esp_err_t history_load_page_leased(
    size_t offset, touch_history_day_t *days, size_t capacity,
    touch_history_index_page_t *page,
    const touch_history_operation_t *operation)
{
    history_day_index_t *index = NULL;
    size_t total = 0;
    esp_err_t result = history_collect_days_leased(
        &index, &total, operation, NULL);
    if (result == ESP_ERR_NOT_FOUND) {
        page->offset = offset;
        return result;
    }
    if (result != ESP_OK) return result;
    size_t available = offset < total ? total - offset : 0;
    size_t returned = available < capacity ? available : capacity;
    for (size_t i = 0; i < returned; ++i) {
        if (history_operation_cancelled(operation)) {
            result = TOUCH_HISTORY_ERR_CANCELLED;
            break;
        }
        touch_history_day_t *day = &days[i];
        memset(day, 0, sizeof(*day));
        strlcpy(day->day, index[offset + i].day, sizeof(day->day));
        day->sessions = index[offset + i].sessions;
        day->skipped_sessions = index[offset + i].skipped_sessions;
        day->has_incomplete_sessions = day->skipped_sessions > 0;
        day->has_therapy = index[offset + i].has_therapy;
        day->has_oximetry = index[offset + i].has_oximetry;
        result = history_fill_day_summary(day);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "summary unavailable day=%s: %s", day->day,
                     esp_err_to_name(result));
            result = ESP_OK;
        }
    }
    if (result == ESP_OK) {
        page->offset = offset;
        page->returned = returned;
        page->total_days = total;
        page->has_more = offset + returned < total;
    }
    free(index);
    return result;
}

esp_err_t touch_history_load_page(size_t offset, touch_history_day_t *days,
                                  size_t capacity,
                                  touch_history_index_page_t *page)
{
    return touch_history_load_page_ex(
        offset, days, capacity, page, NULL);
}

esp_err_t touch_history_load_page_ex(
    size_t offset, touch_history_day_t *days, size_t capacity,
    touch_history_index_page_t *page,
    const touch_history_operation_t *operation)
{
    if (!days || capacity == 0 || !page ||
        capacity > SIZE_MAX / sizeof(*days)) return ESP_ERR_INVALID_ARG;
    memset(days, 0, capacity * sizeof(*days));
    memset(page, 0, sizeof(*page));
    esp_err_t lease = history_lease_acquire_operation(operation);
    if (lease != ESP_OK) return lease;
    esp_err_t result = history_load_page_leased(
        offset, days, capacity, page, operation);
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    return result;
}

esp_err_t touch_history_find_day_index(const char *day, size_t *index_out,
                                       size_t *total_days_out)
{
    return touch_history_find_day_index_ex(
        day, index_out, total_days_out, NULL);
}

esp_err_t touch_history_find_day_index_ex(
    const char *day, size_t *index_out, size_t *total_days_out,
    const touch_history_operation_t *operation)
{
    if (!valid_day(day) || !index_out) return ESP_ERR_INVALID_ARG;
    *index_out = SIZE_MAX;
    if (total_days_out) *total_days_out = 0;
    esp_err_t lease = history_lease_acquire_operation(operation);
    if (lease != ESP_OK) return lease;
    history_day_index_t *index = NULL;
    size_t total = 0;
    esp_err_t result = history_collect_days_leased(
        &index, &total, operation, NULL);
    if (result == ESP_OK) {
        for (size_t i = 0; i < total; ++i) {
            if (history_operation_cancelled(operation)) {
                result = TOUCH_HISTORY_ERR_CANCELLED;
                break;
            }
            if (strcmp(index[i].day, day) != 0) continue;
            *index_out = i;
            break;
        }
        if (result == ESP_OK && *index_out == SIZE_MAX)
            result = ESP_ERR_NOT_FOUND;
    }
    if (total_days_out) *total_days_out = total;
    free(index);
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    return result;
}

esp_err_t touch_history_load(touch_history_day_t *days, size_t capacity,
                             size_t *count)
{
    if (!days || !count || capacity == 0) return ESP_ERR_INVALID_ARG;
    *count = 0;
    touch_history_index_page_t page = {0};
    esp_err_t result = touch_history_load_page(0, days, capacity, &page);
    if (result == ESP_OK) *count = page.returned;
    return result;
}

esp_err_t touch_history_load_month(uint16_t year, uint8_t month_number,
                                   touch_history_month_t *month_out)
{
    return touch_history_load_month_ex(
        year, month_number, month_out, NULL);
}

esp_err_t touch_history_load_month_ex(
    uint16_t year, uint8_t month_number, touch_history_month_t *month_out,
    const touch_history_operation_t *operation)
{
    if (!month_out || year < 2000 || year > 2200 ||
        month_number < 1 || month_number > 12) return ESP_ERR_INVALID_ARG;
    memset(month_out, 0, sizeof(*month_out));
    month_out->year = year;
    month_out->month = month_number;
    month_out->days_in_month = history_days_in_month(year, month_number);
    char prefix[7];
    snprintf(prefix, sizeof(prefix), "%04u%02u", (unsigned)year,
             (unsigned)month_number);
    esp_err_t lease = history_lease_acquire_operation(operation);
    if (lease != ESP_OK) return lease;
    history_day_index_t *index = NULL;
    size_t count = 0;
    esp_err_t result = history_collect_days_leased(
        &index, &count, operation, prefix);
    if (result == ESP_ERR_NOT_FOUND) result = ESP_OK;
    if (result == ESP_OK) {
        for (size_t i = 0; i < count; ++i) {
            if (history_operation_cancelled(operation)) {
                result = TOUCH_HISTORY_ERR_CANCELLED;
                break;
            }
            if (strncmp(index[i].day, prefix, 6)) continue;
            unsigned day_number = (unsigned)(index[i].day[6] - '0') * 10U +
                                  (unsigned)(index[i].day[7] - '0');
            if (day_number < 1 || day_number > month_out->days_in_month)
                continue;
            uint32_t bit = 1U << (day_number - 1);
            if (index[i].has_therapy && !(month_out->therapy_days & bit))
                month_out->therapy_night_count++;
            if (index[i].has_therapy) month_out->therapy_days |= bit;
            if (index[i].has_oximetry && !(month_out->oximetry_days & bit))
                month_out->oximetry_night_count++;
            if (index[i].has_oximetry) month_out->oximetry_days |= bit;
        }
    }
    free(index);
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    return result;
}

esp_err_t touch_history_load_night(const char *day,
                                   touch_history_night_t *night,
                                   touch_history_session_t *session_out,
                                   size_t session_capacity)
{
    return touch_history_load_night_ex(
        day, night, session_out, session_capacity, NULL);
}

static esp_err_t history_load_night_uncached(
    const char *day, touch_history_night_t *night,
    touch_history_session_t *session_out, size_t session_capacity,
    const touch_history_operation_t *operation)
{
    if (!valid_day(day) || !night || (session_capacity && !session_out) ||
        session_capacity > SIZE_MAX / sizeof(*session_out))
        return ESP_ERR_INVALID_ARG;
    memset(night, 0, sizeof(*night));
    if (session_out && session_capacity)
        memset(session_out, 0, session_capacity * sizeof(*session_out));
    strlcpy(night->day, day, sizeof(night->day));
    night->events_result = ESP_ERR_NOT_FOUND;
    history_operation_progress(operation, 0);
    esp_err_t lease = history_lease_acquire_operation(operation);
    if (lease != ESP_OK) return lease;
    history_operation_progress(operation, 40);

    history_session_info_t *sessions = NULL;
    size_t session_count_value = 0;
    size_t skipped_sessions = 0;
    esp_err_t result = history_load_sessions_leased(
        day, &sessions, &session_count_value, &skipped_sessions, operation);
    if (result == ESP_ERR_NOT_FOUND) result = ESP_OK;
    history_operation_progress(operation, 100);
    if (result == ESP_OK && session_count_value)
        result = history_resolve_session_ends(day, sessions,
                                              session_count_value);
    uint16_t oximetry_signals = 0;
    if (result == ESP_OK)
        result = history_unified_axis(
            day, sessions, session_count_value, &night->axis_start_ms,
            &night->axis_end_ms, &oximetry_signals,
            &night->has_oximetry_error, operation);

    if (result == ESP_OK) {
        night->skipped_sessions = skipped_sessions > UINT16_MAX
            ? UINT16_MAX : (uint16_t)skipped_sessions;
        night->has_session_errors = skipped_sessions > 0;
        night->available_signals = oximetry_signals;
        night->session_count = session_count_value;
        night->sessions_returned = session_count_value < session_capacity
            ? session_count_value : session_capacity;
        night->sessions_truncated = night->sessions_returned <
                                    session_count_value;
        for (size_t i = 0; i < session_count_value; ++i) {
            if (history_operation_cancelled(operation)) {
                result = TOUCH_HISTORY_ERR_CANCELLED;
                break;
            }
            if (sessions[i].end_ms <= sessions[i].start_ms) continue;
            esp_err_t probe_result = history_probe_session(day, &sessions[i], operation);
            if (probe_result == ESP_ERR_NO_MEM ||
                probe_result == TOUCH_HISTORY_ERR_CANCELLED) {
                result = probe_result;
                break;
            }
            if (probe_result != ESP_OK)
                ESP_LOGW(TAG,
                         "partial night day=%s session=%s probe failed: %s",
                         day, sessions[i].id, esp_err_to_name(probe_result));
            if (probe_result != ESP_OK) {
                if (night->probe_failed_sessions < UINT16_MAX)
                    night->probe_failed_sessions++;
                night->has_session_errors = true;
            }
            if (sessions[i].event_dropped)
                night->has_event_loss = true;
            night->available_signals |= sessions[i].available_signals;
            if (i < night->sessions_returned) {
                touch_history_session_t *out = &session_out[i];
                strlcpy(out->id, sessions[i].id, sizeof(out->id));
                out->start_ms = sessions[i].start_ms;
                out->end_ms = sessions[i].end_ms;
                out->available_signals = sessions[i].available_signals;
                out->partial = sessions[i].partial;
                out->end_estimated = sessions[i].end_estimated;
                out->has_epr_companion = sessions[i].has_epr_companion;
            }
            history_operation_progress(operation, history_progress_fraction(
                100, 350, i + 1, session_count_value));
        }
    }

    if (result == ESP_OK) history_operation_progress(operation, 425);

    if (result == ESP_OK &&
        (night->available_signals &
         TOUCH_HISTORY_SIGNAL_BIT(TOUCH_HISTORY_SIGNAL_SPO2))) {
        touch_history_overview_t *overview = history_alloc(
            sizeof(*overview), true);
        history_overview_aggregate_t *aggregate = history_alloc(
            sizeof(*aggregate), true);
        if (!overview || !aggregate) {
            result = ESP_ERR_NO_MEM;
        } else {
            history_overview_prepare(
                overview, TOUCH_HISTORY_SIGNAL_SPO2, night->axis_start_ms,
                night->axis_end_ms, sessions, session_count_value,
                TOUCH_HISTORY_OVERVIEW_POINTS, aggregate);
            result = history_accumulate_oximetry(
                day, TOUCH_HISTORY_SIGNAL_SPO2, sessions,
                session_count_value, night->axis_start_ms,
                night->axis_end_ms, aggregate, overview, false, operation,
                425, 600);
            if (result == ESP_OK)
                night->o2_coverage_per_mille =
                    overview->therapy_coverage_per_mille;
            if (result == ESP_OK)
                night->has_o2_coverage = overview->has_therapy_coverage;
            if (result != ESP_OK && result != ESP_ERR_NO_MEM &&
                result != TOUCH_HISTORY_ERR_CANCELLED) {
                ESP_LOGW(TAG, "omit unreadable O2 coverage day=%s: %s", day,
                         esp_err_to_name(result));
                night->available_signals &= (uint16_t)~(
                    TOUCH_HISTORY_SIGNAL_BIT(TOUCH_HISTORY_SIGNAL_SPO2) |
                    TOUCH_HISTORY_SIGNAL_BIT(TOUCH_HISTORY_SIGNAL_PULSE) |
                    TOUCH_HISTORY_SIGNAL_BIT(TOUCH_HISTORY_SIGNAL_MOTION));
                night->has_oximetry_error = true;
                result = ESP_OK;
            }
        }
        free(aggregate);
        free(overview);
    }

    if (result == ESP_OK) {
        history_operation_progress(operation, 620);
        touch_history_day_t summary = {0};
        strlcpy(summary.day, day, sizeof(summary.day));
        esp_err_t summary_result = history_fill_day_summary(&summary);
        if (summary_result == ESP_OK && summary.has_device_ahi) {
            night->device_ahi = summary.device_ahi;
            night->has_device_ahi = true;
        } else if (summary_result != ESP_OK) {
            /* Summary spools are optional metadata written by an independent
             * ingest path.  An old/truncated spool must not hide otherwise
             * readable raw traces, sessions, or respiratory events. */
            ESP_LOGW(TAG, "device summary unavailable day=%s: %s (0x%x)",
                     day, esp_err_to_name(summary_result),
                     (unsigned)summary_result);
            night->has_summary_error = true;
        }
    }

    if (result == ESP_OK && session_count_value) {
        history_event_vector_t vector = {0};
        night->events_result = history_collect_events_leased(
            day, sessions, session_count_value, &vector,
            &night->event_totals, operation, 700, 980);
        free(vector.items);
        if (night->has_session_errors || night->has_event_loss) {
            night->event_totals.complete = false;
            night->event_totals.has_indices = false;
        }
        if (night->events_result == ESP_OK &&
            night->event_totals.has_indices) {
            night->st_ahi = night->event_totals.ahi;
            night->has_st_ahi = true;
        }
    }
    free(sessions);
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    if (result == ESP_OK) history_operation_progress(operation, 1000);
    return result;
}

esp_err_t touch_history_load_night_ex(
    const char *day, touch_history_night_t *night,
    touch_history_session_t *sessions, size_t capacity,
    const touch_history_operation_t *operation)
{
    if (!valid_day(day) || !night || (capacity && !sessions)) return ESP_ERR_INVALID_ARG;
    esp_err_t result = history_lease_acquire_operation(operation);
    if (result != ESP_OK) return result;
    history_cache_select_day(day);
    history_cache_key_t key = history_cache_key(day, HISTORY_MEM_NIGHT, 0, 0, capacity, false);
    size_t bytes = capacity <= 64 ? sizeof(*night) + capacity * sizeof(*sessions) : 0;
    void *cached = bytes ? history_alloc(bytes, true) : NULL;
    if (cached && history_cache_get(&key, cached, bytes)) {
        memcpy(night, cached, sizeof(*night));
        if (capacity) memcpy(sessions, (char *)cached + sizeof(*night), capacity * sizeof(*sessions));
    } else {
        result = history_load_night_uncached(day, night, sessions, capacity, operation);
        if (result == ESP_OK && cached && !history_operation_cancelled(operation)) {
            memcpy(cached, night, sizeof(*night));
            if (capacity) memcpy((char *)cached + sizeof(*night), sessions, capacity * sizeof(*sessions));
            history_cache_put(&key, cached, bytes);
        }
    }
    free(cached);
    if (history_operation_cancelled(operation)) result = TOUCH_HISTORY_ERR_CANCELLED;
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    return result;
}

static bool history_flow_cancel(void *context)
{
    return history_operation_cancelled(context);
}

esp_err_t touch_history_prepare_day_ex(const char *day,
                                      const touch_history_operation_t *operation)
{
    if (!valid_day(day)) return ESP_ERR_INVALID_ARG;
    esp_err_t result = history_lease_acquire_operation(operation);
    if (result != ESP_OK) return result;
    history_cache_key_t key = history_cache_key(day, HISTORY_MEM_FLOW_PREPARED, 0, 0, 0, false);
    bool prepared = false;
    if (history_cache_get(&key, &prepared, sizeof(prepared))) goto done;
    history_session_info_t *sessions = NULL;
    size_t count = 0;
    result = history_load_sessions_leased(day, &sessions, &count, NULL, operation);
    for (size_t i = 0; result == ESP_OK && i < count; ++i) {
        trace_candidate_t candidate = {0};
        if (history_operation_cancelled(operation)) { result = TOUCH_HISTORY_ERR_CANCELLED; break; }
        if (sessions[i].fmt < 2 || history_session_candidate(day, &sessions[i],
            TOUCH_HISTORY_SIGNAL_FLOW, &candidate) != ESP_OK) continue;
        int built = history_flow_cache_build(candidate.path, history_flow_cancel, (void *)operation);
        if (built == -1) result = TOUCH_HISTORY_ERR_CANCELLED;
        else if (built) result = ESP_FAIL;
    }
    free(sessions);
    if (result == ESP_OK) {
        prepared = true;
        history_cache_put(&key, &prepared, sizeof(prepared));
    }
done:
    sd_storage_lease_release(SD_LEASE_UPLOAD);
    return result;
}

#endif /* TOUCH_HISTORY_MODEL_TEST */
