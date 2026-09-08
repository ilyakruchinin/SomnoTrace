/*
 * SomnoTrace - STR.edf summary generation from protobuf spool
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

#include "edf_summary.h"

static const char *TAG = "edf_sum";

/* ════════════════════════════════════════════════════════════════════
 *  Section 2: Minimal protobuf wire-format decoder
 * ════════════════════════════════════════════════════════════════════
 *
 * The AS11 spool payloads are protobuf-encoded.  We need a minimal decoder
 * that can extract varint and length-delimited fields without a .proto
 * schema.  The field number and wire type are packed into the first byte(s)
 * of each field tag as a varint: (field_number << 3) | wire_type.
 *
 * Wire types:
 *   0 = varint       (int64, uint64, bool, enum)
 *   1 = 64-bit       (fixed64, sfixed64, double)
 *   2 = length-delimited (string, bytes, embedded message, packed repeated)
 *   5 = 32-bit       (fixed32, sfixed32, float)
 */

typedef struct {
    int field;
    int wire;
    const uint8_t *data;
    size_t len;
} pb_field_t;

/* Decode a varint from buf, advancing *pos.  Returns the value. */
static uint64_t pb_decode_varint(const uint8_t *buf, size_t buf_len,
                                 size_t *pos)
{
    uint64_t val = 0;
    int shift = 0;
    while (*pos < buf_len) {
        uint8_t b = buf[(*pos)++];
        val |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift >= 64) break;
    }
    return val;
}

/* Iterate over top-level fields in a protobuf message.
 * Calls cb(field, wire, data, len, user_data) for each field.
 * Returns number of fields decoded. */
typedef void (*pb_iter_cb)(const pb_field_t *f, void *user_data);

static int pb_iter(const uint8_t *buf, size_t buf_len, pb_iter_cb cb, void *ud)
{
    size_t pos = 0;
    int count = 0;
    while (pos < buf_len) {
        size_t tag_start = pos;
        uint64_t tag = pb_decode_varint(buf, buf_len, &pos);
        int field = (int)(tag >> 3);
        int wire = (int)(tag & 0x07);
        if (field == 0) break;

        pb_field_t f = { .field = field, .wire = wire };

        switch (wire) {
        case 0: {  /* varint */
            size_t vpos = pos;
            f.data = buf + tag_start;
            f.len = 0;  /* varint value is decoded by caller if needed */
            /* Skip the varint value */
            pb_decode_varint(buf, buf_len, &vpos);
            f.len = vpos - pos;
            f.data = buf + pos;
            pos = vpos;
            break;
        }
        case 1: {  /* 64-bit */
            if (pos + 8 > buf_len) return count;
            f.data = buf + pos;
            f.len = 8;
            pos += 8;
            break;
        }
        case 2: {  /* length-delimited */
            size_t lpos = pos;
            uint64_t flen = pb_decode_varint(buf, buf_len, &lpos);
            pos = lpos;
            if (pos + flen > buf_len) return count;
            f.data = buf + pos;
            f.len = (size_t)flen;
            pos += flen;
            break;
        }
        case 5: {  /* 32-bit */
            if (pos + 4 > buf_len) return count;
            f.data = buf + pos;
            f.len = 4;
            pos += 4;
            break;
        }
        default:
            /* Unknown wire type — stop */
            return count;
        }

        cb(&f, ud);
        count++;
    }
    return count;
}

/* Helper: extract a varint value from a field's data. */
static int64_t pb_varint_val(const pb_field_t *f)
{
    if (!f || !f->data) return 0;
    size_t pos = 0;
    return (int64_t)pb_decode_varint(f->data, f->len > 0 ? f->len : 10, &pos);
}

static inline int16_t settings_x50(double v)
{
    return (int16_t)lrint(v * 50.0);
}

/* ════════════════════════════════════════════════════════════════════
 *  Section 6: STR.edf generation from Summary spool protobuf
 * ════════════════════════════════════════════════════════════════════
 *
 * The Summary spool contains one or more protobuf records (wrapped in
 * field-2 messages).  Each record contains session statistics and settings
 * that map to the 134-field STR.edf format.
 *
 * The STR.edf has 1 data record per 86400 seconds (1 day), containing
 * 134 int16 signal values + 1 Crc16 value.
 *
 * Field mapping is based on the _SUMMARY_FIELDS table in as11_spool.py
 * and the STR signal reference in edf_signals.md.
 */

/* Summary protobuf field numbers (from as11_spool.py _SUMMARY_FIELDS) */
#define SUM_F_PERIOD_START    2
#define SUM_F_PERIOD_END      3
#define SUM_F_TZ_OFFSET       4
#define SUM_F_DURATION_MIN    5
#define SUM_F_SESSION_MODE    6
#define SUM_F_AHI             7
#define SUM_F_AI              8
#define SUM_F_HI              9
#define SUM_F_OAI             10
#define SUM_F_CAI             11
#define SUM_F_UAI             12
#define SUM_F_RIN             13
#define SUM_F_LEAK            14
#define SUM_F_INSP_PRESS      15
#define SUM_F_CSR             16
#define SUM_F_SAU             17
#define SUM_F_SPONT_TRIG      18
#define SUM_F_SPONT_CYC       19
#define SUM_F_EXP_PRESS       20
#define SUM_F_MEAN_MASK_PRESS 21
#define SUM_F_TIDAL_VOL       22
#define SUM_F_MIN_VENT        23
#define SUM_F_TGT_VENT        24
#define SUM_F_RESP_RATE       25
#define SUM_F_INSP_DUR        26
#define SUM_F_IE_RATIO        27
#define SUM_F_SPO2            28
#define SUM_F_AMB_HUMID       29
#define SUM_F_HUM_TEMP        30
#define SUM_F_HTUBE_TEMP      31
#define SUM_F_HUM_POWER       32
#define SUM_F_HTUBE_POWER     33
#define SUM_F_HUM_CONNECTED   34
#define SUM_F_TUBE_CONNECTED  35
#define SUM_F_BLOWER_PRESS    36
#define SUM_F_RESP_FLOW       37
#define SUM_F_BLOWER_FLOW     38
#define SUM_F_SESSION_COUNT   39
#define SUM_F_CLOCK_B         40
#define SUM_F_HEART_RATE      41

/* Sub-field percentile indices for metric submessages.
 * Sub-field 2 = 50th percentile, 3 = 95th (or 70th for Leak), 4 = 100th (or 95th).
 * The exact mapping depends on the field — see _SUMMARY_SUBFIELDS in as11_spool.py. */

/* MaskOn/MaskOff declare phys_max 1440 (one day of minutes).  Anything
 * beyond that is rejected by consumers as a corrupt card, so it is treated
 * as an internal error rather than written out. */
#define STR_MASK_MINUTES_MAX  1440

/* STR.edf signal count — full 134-signal superset:
 * 133 data signals + 1 Crc16 = 134 total.
 * Includes VAuto/Spont/ST/Timed/ASV/ASVAuto settings and
 * SpontTrig/Cyc, TgtVent/IERatio/Ti stats — see edf_signals.md. */

/* STR enum export maps — some fields are remapped before writing to EDF.
 * From edf_signals.md "STR enum export maps" section. */
static const int MODE_MAP[] = {3, 1, 2, 4, 10, 16, 8, 6, 7, 5, 9};

/* Context for protobuf iteration: collects all field values from a
 * Summary record into a key-value store. */
typedef struct {
    int64_t scalars[64];       /* varint fields by field number */
    bool has_scalar[64];
    /* For metric submessages (length-delimited), store up to 4 sub-values */
    struct {
        int64_t val[8];
        int n;
    } metrics[64];
    bool has_metric[64];
    /* Session entries from Summary spool field 6 (SessionModeEntries).
     * Each entry has: sub-field 1 = MaskOn timestamp (epoch ms),
     * sub-field 2 = per-session duration in minutes (NOT therapy mode). */
    struct {
        int64_t ts;            /* sub-field 1: MaskOn timestamp (epoch ms) */
        int64_t duration_min;  /* sub-field 2: per-session duration (minutes) */
    } session_entries[20];
    int n_session_entries;
} summary_ctx_t;

/* Callback for pb_iter on a Summary record. */
static void summary_field_cb(const pb_field_t *f, void *ud)
{
    summary_ctx_t *ctx = (summary_ctx_t *)ud;
    if (f->field < 1 || f->field > 63) return;

    if (f->wire == 0) {
        /* Varint field — decode the value */
        ctx->scalars[f->field] = pb_varint_val(f);
        ctx->has_scalar[f->field] = true;
    } else if (f->field == SUM_F_SESSION_MODE && f->wire == 2 && f->data && f->len > 0) {
        /* SessionModeEntries (field 6): repeated wrapper submessages.
         * Each wrapper (sub-field 1, wire 2) contains:
         *   sub-sub-field 1 (varint) = MaskOn timestamp (epoch ms)
         *   sub-sub-field 2 (varint) = per-session duration (minutes) */
        size_t pos = 0;
        while (pos < f->len && ctx->n_session_entries < 20) {
            uint64_t tag = pb_decode_varint(f->data, f->len, &pos);
            int sf = (int)(tag >> 3);
            int sw = (int)(tag & 0x07);
            if (sf == 0) break;
            if (sw == 2 && sf == 1) {
                /* Wrapper submessage for one session entry */
                size_t lpos = pos;
                uint64_t flen = pb_decode_varint(f->data, f->len, &lpos);
                const uint8_t *inner = f->data + lpos;
                size_t inner_len = (size_t)flen;
                pos = lpos + flen;
                int64_t ts = 0, duration_min = 0;
                size_t ip = 0;
                while (ip < inner_len) {
                    uint64_t itag = pb_decode_varint(inner, inner_len, &ip);
                    int isf = (int)(itag >> 3);
                    int isw = (int)(itag & 0x07);
                    if (isf == 0) break;
                    if (isw == 0) {
                        int64_t val = (int64_t)pb_decode_varint(inner, inner_len, &ip);
                        if (isf == 1) ts = val;
                        else if (isf == 2) duration_min = val;
                    } else if (isw == 2) {
                        size_t ilpos = ip;
                        uint64_t ilen = pb_decode_varint(inner, inner_len, &ilpos);
                        ip = ilpos + ilen;
                    } else if (isw == 1) {
                        ip += 8;
                    } else if (isw == 5) {
                        ip += 4;
                    } else {
                        break;
                    }
                }
                ctx->session_entries[ctx->n_session_entries].ts = ts;
                ctx->session_entries[ctx->n_session_entries].duration_min = duration_min;
                ctx->n_session_entries++;
            } else if (sw == 2) {
                size_t lpos = pos;
                uint64_t flen = pb_decode_varint(f->data, f->len, &lpos);
                pos = lpos + flen;
            } else if (sw == 0) {
                (void)pb_decode_varint(f->data, f->len, &pos);
            } else if (sw == 1) {
                pos += 8;
            } else if (sw == 5) {
                pos += 4;
            } else {
                break;
            }
        }
    } else if (f->wire == 2 && f->data && f->len > 0) {
        /* Length-delimited — could be a metric submessage or raw bytes.
         * Try to decode as a protobuf submessage with varint sub-fields. */
        size_t pos = 0;
        int n = 0;
        while (pos < f->len && n < 8) {
            uint64_t tag = pb_decode_varint(f->data, f->len, &pos);
            int sub_field = (int)(tag >> 3);
            int sub_wire = (int)(tag & 0x07);
            if (sub_field == 0) break;
            if (sub_wire == 0) {
                ctx->metrics[f->field].val[sub_field] =
                    (int64_t)pb_decode_varint(f->data, f->len, &pos);
            } else if (sub_wire == 2) {
                size_t lpos = pos;
                uint64_t flen = pb_decode_varint(f->data, f->len, &lpos);
                pos = lpos + flen;
            } else if (sub_wire == 1) {
                pos += 8;
            } else if (sub_wire == 5) {
                pos += 4;
            } else {
                break;
            }
            if (sub_wire == 0) n++;
        }
        ctx->metrics[f->field].n = n;
        ctx->has_metric[f->field] = true;
    }
}

/* Get a metric sub-value (percentile) from the context.
 * sub_idx: 2=50th, 3=95th/70th, 4=100th/95th, 1=5th */
static int16_t get_metric(const summary_ctx_t *ctx, int field, int sub_idx,
                          int16_t default_val)
{
    if (field < 1 || field > 63 || !ctx->has_metric[field]) return default_val;
    if (sub_idx < 0 || sub_idx >= 8) return default_val;
    /* Check if the sub-field was populated (val array is sparse) */
    /* We use a simple heuristic: if n > 0 and sub_idx was seen */
    /* Actually, we need to track which sub-fields were seen.
     * For now, use the val directly — if it's 0 and n is small,
     * it might not have been set.  This is a simplification. */
    return (int16_t)ctx->metrics[field].val[sub_idx];
}

/* Get a scalar value from the context. */
static int16_t get_scalar(const summary_ctx_t *ctx, int field, int16_t default_val)
{
    if (field < 1 || field > 63 || !ctx->has_scalar[field]) return default_val;
    return (int16_t)ctx->scalars[field];
}

/* Map On/Off/Auto string from settings.json to AS11 EDF enum value.
 * AS11 EDF convention: raw enum + 1, so Off=1, On=2, Auto=3.
 * See spec/0002-edf-export.md §4.3.4. */
static int on_off_to_edf(const char *s)
{
    if (!s) return -1;
    if (strcmp(s, "On") == 0) return 2;
    if (strcmp(s, "Off") == 0) return 1;
    if (strcmp(s, "Auto") == 0) return 3;
    return -1;
}

/* Map trigger/cycle sensitivity to EDF value.
 * Export map [1,2,3,4,5,6,7] = raw_index + 1.
 * The AS11 Get RPC returns these as numbers (raw CONF indices 0-6).
 * See edf_signals.md "STR enum export maps". */
static int trigger_cycle_to_edf(int raw)
{
    if (raw < 0 || raw > 6) return -1;
    return raw + 1;
}

/* Map EasyBreathe enable to EDF value.
 * Export map [1,2] = Off=1, On=2. */
static int easy_breathe_to_edf(const char *s)
{
    if (!s) return -1;
    if (strcmp(s, "Off") == 0) return 1;
    if (strcmp(s, "On") == 0) return 2;
    return -1;
}

/* Map RespiratoryRateEnable to EDF value.
 * Export map [1,3]: raw 0→1 (Off), raw 1→3 (On).
 * See edf_signals.md "STR enum export maps". */
static int resp_rate_en_to_edf(const char *s)
{
    if (!s) return -1;
    if (strcmp(s, "Off") == 0) return 1;
    if (strcmp(s, "On") == 0) return 3;
    return -1;
}

/* Map ActiveTherapyProfile name from settings.json to the MOP enum index
 * used by MODE_MAP.  The AS11 STR.edf Mode field is sourced from the
 * ActiveTherapyProfile (MOP setting), not from SessionModeEntries in the
 * Summary spool (which contain unreliable values).  See edf_signals.md
 * "STR enum export maps" and resmed_config.py ENUM_OPTIONS['MOP']. */
static int profile_name_to_mop(const char *name)
{
    if (!name) return -1;
    if (strcmp(name, "CpapProfile") == 0)       return 0;  /* CPAP      */
    if (strcmp(name, "AutoSetProfile") == 0)    return 1;  /* AutoSet   */
    if (strcmp(name, "AutoSetForHerProfile") == 0) return 2;  /* APAP (Her) */
    if (strcmp(name, "SpontProfile") == 0)      return 3;  /* S         */
    if (strcmp(name, "STProfile") == 0)         return 4;  /* ST        */
    if (strcmp(name, "TimedProfile") == 0)      return 5;  /* T         */
    if (strcmp(name, "VAutoProfile") == 0)      return 6;  /* VAuto     */
    if (strcmp(name, "ASVProfile") == 0)        return 7;  /* ASV       */
    if (strcmp(name, "ASVAutoProfile") == 0)    return 8;  /* ASVAuto   */
    if (strcmp(name, "iVAPSProfile") == 0)      return 9;  /* iVAPS     */
    if (strcmp(name, "PACProfile") == 0)        return 10; /* PAC       */
    return -1;
}

/* Convert a raw spool value to the EDF digital value by dividing by the
 * field's "logical scale".  The AS11 stores summary metrics in the protobuf
 * spool as fixed-point integers; its firmware STR writer divides each by a
 * field-specific logical_scale before writing the EDF digital value.
 *
 * We replicate that here using integer arithmetic: the logical_scale is
 * expressed as a fraction (den / num), so the conversion is
 *   edf_digital = raw * num / den
 * Returns -1 (sentinel) unchanged when raw is -1.
 *
 * Logical scales (from as11_edf_superset.py STR_SUPERSET_METADATA):
 *   Pressure (cmH2O)     logical_scale = 2     → num=1, den=2
 *   Flow (L/s)           logical_scale = 0.2   → num=5, den=1
 *   Humidity/Temp/Power  logical_scale = 10    → num=1, den=10
 *   SpO2 (%)             logical_scale = 100   → num=1, den=100
 *   Minute Ventilation   logical_scale = 12.5  → num=2, den=25
 *   Respiratory Rate     logical_scale = 20    → num=1, den=20
 *   Tidal Volume         logical_scale = 2     → num=1, den=2
 *   Indices (AHI etc)    logical_scale = 10    → num=1, den=10
 *   Duration/enums/SAU   logical_scale = 1     → no conversion needed
 */
/* Build STR data values [4-132] from a summary context and settings JSON.
 * str_values must be pre-filled with 0xFF (sentinel for "no data").
 *
 * Spool-derived stat fields [78-132] are raw fixed-point integers from the
 * protobuf Summary record.  Each is divided by its logical_scale (via
 * spool_to_edf) to produce the EDF digital value that the AS11 firmware
 * would write.  Settings fields [6-77] come from the Get RPC response
 * (settings.json) and are already in EDF digital units (e.g. cmH2O × 50). */
static void build_str_data_values(summary_ctx_t *ctx, int16_t *str_values,
                                  const cJSON *settings_json)
{
    /* Session core [4-5] — logical_scale = 1, no conversion needed */
    str_values[4] = get_scalar(ctx, SUM_F_DURATION_MIN, 0);  /* Duration */

    /* Mode [5]: derived from ActiveTherapyProfile in settings.json.
     * The AS11's own export uses the MOP setting (ActiveTherapyProfile),
     * not the SessionModeEntries from the Summary spool, which contain
     * unreliable values.  See edf_signals.md "STR enum export maps". */
    int mode_raw = -1;
    if (settings_json) {
        cJSON *sp = cJSON_GetObjectItem(settings_json, "SettingProfiles");
        cJSON *ap = sp ? cJSON_GetObjectItem(sp, "ActiveProfiles") : NULL;
        cJSON *tp_name = ap ? cJSON_GetObjectItem(ap, "TherapyProfile") : NULL;
        if (tp_name && cJSON_IsString(tp_name))
            mode_raw = profile_name_to_mop(tp_name->valuestring);
    }
    if (mode_raw >= 0 && mode_raw < (int)(sizeof(MODE_MAP) / sizeof(MODE_MAP[0]))) {
        str_values[5] = MODE_MAP[mode_raw];
    } else {
        str_values[5] = -1;  /* unknown — leave sentinel */
    }

    /* CPAP/AutoSet settings [6-13], bi-level settings [14-58], and
     * common comfort/settings [59-77]: from settings.json (Get RPC response
     * captured during post-therapy).
     * Pressures are stored in cmH2O × 50, temperatures in °C × 10.
     * Ti fields use seconds × 50 (logical_scale=20, edf_output_scale=50).
     * Enum fields use AS11 EDF convention: raw enum + 1 (Off=1, On=2, etc.).
     * S.Mask is the exception: raw + 2.  See spec/0002-edf-export.md §4.3.4. */
    if (settings_json) {
        cJSON *sp = cJSON_GetObjectItem(settings_json, "SettingProfiles");
        cJSON *tp = sp ? cJSON_GetObjectItem(sp, "TherapyProfiles") : NULL;
        cJSON *fp = sp ? cJSON_GetObjectItem(sp, "FeatureProfiles") : NULL;

        /* Pressure fields [6-13]: cmH2O × 50 */
        if (tp) {
            cJSON *cpap = cJSON_GetObjectItem(tp, "CpapProfile");
            cJSON *autoset = cJSON_GetObjectItem(tp, "AutoSetProfile");
            cJSON *her = cJSON_GetObjectItem(tp, "AutoSetForHerProfile");
            cJSON *v;
            if (cpap) {
                if ((v = cJSON_GetObjectItem(cpap, "StartPressure")) && cJSON_IsNumber(v))
                    str_values[6] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(cpap, "SetPressure")) && cJSON_IsNumber(v))
                    str_values[7] = settings_x50(v->valuedouble);
            }
            if (autoset) {
                if ((v = cJSON_GetObjectItem(autoset, "StartPressure")) && cJSON_IsNumber(v))
                    str_values[8] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(autoset, "MaxPressure")) && cJSON_IsNumber(v))
                    str_values[9] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(autoset, "MinPressure")) && cJSON_IsNumber(v))
                    str_values[10] = settings_x50(v->valuedouble);
            }
            if (her) {
                if ((v = cJSON_GetObjectItem(her, "StartPressure")) && cJSON_IsNumber(v))
                    str_values[11] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(her, "MaxPressure")) && cJSON_IsNumber(v))
                    str_values[12] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(her, "MinPressure")) && cJSON_IsNumber(v))
                    str_values[13] = settings_x50(v->valuedouble);
            }

            /* VAuto settings [14-21]: pressures cmH2O × 50, Ti × 50,
             * trigger/cycle via trigger_cycle_to_edf (raw + 1). */
            cJSON *vauto = cJSON_GetObjectItem(tp, "VAutoProfile");
            if (vauto) {
                if ((v = cJSON_GetObjectItem(vauto, "StartPressure")) && cJSON_IsNumber(v))
                    str_values[14] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(vauto, "MaxInspiratoryPressure")) && cJSON_IsNumber(v))
                    str_values[15] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(vauto, "MinExpiratoryPressure")) && cJSON_IsNumber(v))
                    str_values[16] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(vauto, "SetPressureSupport")) && cJSON_IsNumber(v))
                    str_values[17] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(vauto, "SetMaxInspiratoryTime")) && cJSON_IsNumber(v))
                    str_values[18] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(vauto, "SetMinInspiratoryTime")) && cJSON_IsNumber(v))
                    str_values[19] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(vauto, "TriggerSensitivity")) && cJSON_IsNumber(v))
                    str_values[20] = (int16_t)trigger_cycle_to_edf(v->valueint);
                if ((v = cJSON_GetObjectItem(vauto, "CycleSensitivity")) && cJSON_IsNumber(v))
                    str_values[21] = (int16_t)trigger_cycle_to_edf(v->valueint);
            }

            /* Spont settings [22-32]: pressures cmH2O × 50, Ti × 50,
             * EasyBreathe/RespRateEn via helpers, RiseEnable via on_off,
             * RiseTime in msec, trigger/cycle via helper. */
            cJSON *spont = cJSON_GetObjectItem(tp, "SpontProfile");
            if (spont) {
                if ((v = cJSON_GetObjectItem(spont, "StartPressure")) && cJSON_IsNumber(v))
                    str_values[22] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(spont, "TargetInspiratoryPressure")) && cJSON_IsNumber(v))
                    str_values[23] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(spont, "TargetExpiratoryPressure")) && cJSON_IsNumber(v))
                    str_values[24] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(spont, "EasyBreatheEnable")) && cJSON_IsString(v))
                    str_values[25] = (int16_t)easy_breathe_to_edf(v->valuestring);
                if ((v = cJSON_GetObjectItem(spont, "RespiratoryRateEnable")) && cJSON_IsString(v))
                    str_values[26] = (int16_t)resp_rate_en_to_edf(v->valuestring);
                if ((v = cJSON_GetObjectItem(spont, "SetMaxInspiratoryTime")) && cJSON_IsNumber(v))
                    str_values[27] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(spont, "SetMinInspiratoryTime")) && cJSON_IsNumber(v))
                    str_values[28] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(spont, "RiseTimeEnable")) && cJSON_IsString(v))
                    str_values[29] = (int16_t)on_off_to_edf(v->valuestring);
                if ((v = cJSON_GetObjectItem(spont, "RiseTime")) && cJSON_IsNumber(v))
                    str_values[30] = (int16_t)v->valuedouble;
                if ((v = cJSON_GetObjectItem(spont, "TriggerSensitivity")) && cJSON_IsNumber(v))
                    str_values[31] = (int16_t)trigger_cycle_to_edf(v->valueint);
                if ((v = cJSON_GetObjectItem(spont, "CycleSensitivity")) && cJSON_IsNumber(v))
                    str_values[32] = (int16_t)trigger_cycle_to_edf(v->valueint);
            }

            /* ST settings [33-42]: pressures cmH2O × 50, RespRate × 5,
             * Ti × 50, RiseEnable/RiseTime, trigger via helper, cycle raw+1. */
            cJSON *st = cJSON_GetObjectItem(tp, "STProfile");
            if (st) {
                if ((v = cJSON_GetObjectItem(st, "StartPressure")) && cJSON_IsNumber(v))
                    str_values[33] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(st, "TargetInspiratoryPressure")) && cJSON_IsNumber(v))
                    str_values[34] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(st, "TargetExpiratoryPressure")) && cJSON_IsNumber(v))
                    str_values[35] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(st, "SetRespiratoryRate")) && cJSON_IsNumber(v))
                    str_values[36] = (int16_t)(v->valuedouble * 5);
                if ((v = cJSON_GetObjectItem(st, "SetMaxInspiratoryTime")) && cJSON_IsNumber(v))
                    str_values[37] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(st, "SetMinInspiratoryTime")) && cJSON_IsNumber(v))
                    str_values[38] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(st, "RiseTimeEnable")) && cJSON_IsString(v))
                    str_values[39] = (int16_t)on_off_to_edf(v->valuestring);
                if ((v = cJSON_GetObjectItem(st, "RiseTime")) && cJSON_IsNumber(v))
                    str_values[40] = (int16_t)v->valuedouble;
                if ((v = cJSON_GetObjectItem(st, "TriggerSensitivity")) && cJSON_IsNumber(v))
                    str_values[41] = (int16_t)trigger_cycle_to_edf(v->valueint);
                if ((v = cJSON_GetObjectItem(st, "CycleSensitivity")) && cJSON_IsNumber(v))
                    str_values[42] = (int16_t)(v->valueint + 1);
            }

            /* Timed settings [43-49]: pressures cmH2O × 50, RespRate × 5,
             * Ti × 50, RiseEnable/RiseTime. */
            cJSON *timed = cJSON_GetObjectItem(tp, "TimedProfile");
            if (timed) {
                if ((v = cJSON_GetObjectItem(timed, "StartPressure")) && cJSON_IsNumber(v))
                    str_values[43] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(timed, "TargetInspiratoryPressure")) && cJSON_IsNumber(v))
                    str_values[44] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(timed, "TargetExpiratoryPressure")) && cJSON_IsNumber(v))
                    str_values[45] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(timed, "SetRespiratoryRate")) && cJSON_IsNumber(v))
                    str_values[46] = (int16_t)(v->valuedouble * 5);
                if ((v = cJSON_GetObjectItem(timed, "SetInspiratoryTime")) && cJSON_IsNumber(v))
                    str_values[47] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(timed, "RiseTimeEnable")) && cJSON_IsString(v))
                    str_values[48] = (int16_t)on_off_to_edf(v->valuestring);
                if ((v = cJSON_GetObjectItem(timed, "RiseTime")) && cJSON_IsNumber(v))
                    str_values[49] = (int16_t)v->valuedouble;
            }

            /* ASV settings [50-53]: pressures cmH2O × 50 */
            cJSON *asv = cJSON_GetObjectItem(tp, "ASVProfile");
            if (asv) {
                if ((v = cJSON_GetObjectItem(asv, "StartPressure")) && cJSON_IsNumber(v))
                    str_values[50] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(asv, "TargetExpiratoryPressure")) && cJSON_IsNumber(v))
                    str_values[51] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(asv, "MaxPressureSupport")) && cJSON_IsNumber(v))
                    str_values[52] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(asv, "MinPressureSupport")) && cJSON_IsNumber(v))
                    str_values[53] = settings_x50(v->valuedouble);
            }

            /* ASVAuto settings [54-58]: pressures cmH2O × 50 */
            cJSON *asvauto = cJSON_GetObjectItem(tp, "ASVAutoProfile");
            if (asvauto) {
                if ((v = cJSON_GetObjectItem(asvauto, "StartPressure")) && cJSON_IsNumber(v))
                    str_values[54] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(asvauto, "MaxExpiratoryPressure")) && cJSON_IsNumber(v))
                    str_values[55] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(asvauto, "MinExpiratoryPressure")) && cJSON_IsNumber(v))
                    str_values[56] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(asvauto, "MaxPressureSupport")) && cJSON_IsNumber(v))
                    str_values[57] = settings_x50(v->valuedouble);
                if ((v = cJSON_GetObjectItem(asvauto, "MinPressureSupport")) && cJSON_IsNumber(v))
                    str_values[58] = settings_x50(v->valuedouble);
            }
        }

        /* Comfort/settings [59-77] */
        if (fp) {
            cJSON *comfort = cJSON_GetObjectItem(fp, "ComfortFeature");
            cJSON *epr = cJSON_GetObjectItem(fp, "EprFeature");
            cJSON *ramp = cJSON_GetObjectItem(fp, "AutoRampFeature");
            cJSON *smart = cJSON_GetObjectItem(fp, "SmartStartStopFeature");
            cJSON *circuit = cJSON_GetObjectItem(fp, "CircuitFeature");
            cJSON *climate = cJSON_GetObjectItem(fp, "ClimateFeature");
            cJSON *patview = cJSON_GetObjectItem(fp, "PatientViewFeature");
            cJSON *v;

            if (comfort) {
                v = cJSON_GetObjectItem(comfort, "AutoSetComfort");
                if (v && cJSON_IsString(v)) {
                    if (strcmp(v->valuestring, "On") == 0) str_values[59] = 2;
                    else if (strcmp(v->valuestring, "Off") == 0) str_values[59] = 1;
                }
            }

            if (ramp) {
                v = cJSON_GetObjectItem(ramp, "RampEnable");
                if (v && cJSON_IsString(v)) {
                    if (strcmp(v->valuestring, "Off") == 0) str_values[60] = 1;
                    else if (strcmp(v->valuestring, "On") == 0) str_values[60] = 2;
                    else if (strcmp(v->valuestring, "Auto") == 0) str_values[60] = 3;
                }
                v = cJSON_GetObjectItem(ramp, "RampTime");
                if (v && cJSON_IsNumber(v)) str_values[61] = (int16_t)v->valuedouble;
            }

            if (epr) {
                v = cJSON_GetObjectItem(epr, "EprEnablePatientAccess");
                if (v && cJSON_IsString(v)) str_values[62] = (int16_t)on_off_to_edf(v->valuestring);
                v = cJSON_GetObjectItem(epr, "EprEnable");
                if (v && cJSON_IsString(v)) str_values[63] = (int16_t)on_off_to_edf(v->valuestring);
                v = cJSON_GetObjectItem(epr, "EprPressure");
                if (v && cJSON_IsNumber(v)) str_values[64] = settings_x50(v->valuedouble);
                v = cJSON_GetObjectItem(epr, "EprType");
                if (v && cJSON_IsString(v)) {
                    if (strcmp(v->valuestring, "RampOnly") == 0) str_values[65] = 1;
                    else if (strcmp(v->valuestring, "FullTime") == 0) str_values[65] = 2;
                }
            }

            if (smart) {
                v = cJSON_GetObjectItem(smart, "SmartStart");
                if (v && cJSON_IsString(v)) str_values[66] = (int16_t)on_off_to_edf(v->valuestring);
            }

            if (patview) {
                v = cJSON_GetObjectItem(patview, "PatientView");
                if (v && cJSON_IsString(v)) {
                    if (strcmp(v->valuestring, "Advanced") == 0) str_values[67] = 1;
                    else if (strcmp(v->valuestring, "Full") == 0) str_values[67] = 1;
                    else if (strcmp(v->valuestring, "Basic") == 0) str_values[67] = 2;
                }
            }

            if (circuit) {
                v = cJSON_GetObjectItem(circuit, "AntiBacterialFilter");
                if (v && cJSON_IsString(v)) {
                    if (strcmp(v->valuestring, "No") == 0) str_values[68] = 1;
                    else if (strcmp(v->valuestring, "Yes") == 0) str_values[68] = 2;
                }
                v = cJSON_GetObjectItem(circuit, "MaskType");
                if (v && cJSON_IsString(v)) {
                    if (strcmp(v->valuestring, "Pillows") == 0) str_values[69] = 2;
                    else if (strcmp(v->valuestring, "FullFace") == 0 ||
                             strcmp(v->valuestring, "Full Face") == 0) str_values[69] = 3;
                    else if (strcmp(v->valuestring, "Nasal") == 0) str_values[69] = 4;
                    else if (strcmp(v->valuestring, "Pediatric") == 0) str_values[69] = 5;
                }
                v = cJSON_GetObjectItem(circuit, "TubeType");
                if (v && cJSON_IsString(v)) {
                    if (strcmp(v->valuestring, "SlimLine") == 0) str_values[70] = 1;
                    else if (strcmp(v->valuestring, "Standard") == 0) str_values[70] = 2;
                    else if (strcmp(v->valuestring, "15mmNonHeated") == 0) str_values[70] = 3;
                    else if (strcmp(v->valuestring, "19mmNonHeated") == 0) str_values[70] = 4;
                }
            }

            if (climate) {
                v = cJSON_GetObjectItem(climate, "ClimateControl");
                if (v && cJSON_IsString(v)) {
                    if (strcmp(v->valuestring, "Auto") == 0) str_values[71] = 1;
                    else if (strcmp(v->valuestring, "Manual") == 0) str_values[71] = 2;
                }
                v = cJSON_GetObjectItem(climate, "HumidifierSettingEnable");
                if (v && cJSON_IsString(v)) str_values[72] = (int16_t)on_off_to_edf(v->valuestring);
                v = cJSON_GetObjectItem(climate, "HumidifierLevel");
                if (v && cJSON_IsNumber(v)) str_values[73] = (int16_t)v->valuedouble;
                v = cJSON_GetObjectItem(climate, "HeatedTubeSettingEnable");
                if (v && cJSON_IsString(v)) str_values[74] = (int16_t)on_off_to_edf(v->valuestring);
                v = cJSON_GetObjectItem(climate, "HeatedTubeTemperature");
                if (v && cJSON_IsNumber(v)) str_values[75] = (int16_t)(v->valuedouble * 10);
            }
        }
    }

    /* [76] HeatedTube and [77] Humidifier: enum fields from Summary spool.
     * logical_scale = 1, no conversion needed. */
    str_values[76] = get_scalar(ctx, SUM_F_TUBE_CONNECTED, -1);
    str_values[77] = get_scalar(ctx, SUM_F_HUM_CONNECTED, -1);

    /* Environment and oximetry stats [78-91]
     * Spool values are converted to EDF digital via spool_to_edf().
     * Default -1 (sentinel) when no summary data available. */
    str_values[78] = spool_to_edf(get_metric(ctx, SUM_F_BLOWER_PRESS, 3, -1), 1, 2);   /* BlowPress.95  /2  */
    str_values[79] = spool_to_edf(get_metric(ctx, SUM_F_BLOWER_PRESS, 1, -1), 1, 2);   /* BlowPress.5   /2  */
    str_values[80] = spool_to_edf(get_metric(ctx, SUM_F_RESP_FLOW, 3, -1), 5, 1);     /* Flow.95       *5  */
    str_values[81] = spool_to_edf(get_metric(ctx, SUM_F_RESP_FLOW, 1, -1), 5, 1);     /* Flow.5        *5  */
    str_values[82] = spool_to_edf(get_metric(ctx, SUM_F_BLOWER_FLOW, 2, -1), 5, 1);   /* BlowFlow.50   *5  */
    str_values[83] = spool_to_edf(get_metric(ctx, SUM_F_AMB_HUMID, 2, -1), 1, 10);    /* AmbHumidity   /10 */
    str_values[84] = spool_to_edf(get_metric(ctx, SUM_F_HUM_TEMP, 2, -1), 1, 10);     /* HumTemp       /10 */
    str_values[85] = spool_to_edf(get_metric(ctx, SUM_F_HTUBE_TEMP, 2, -1), 1, 10);   /* HTubeTemp     /10 */
    str_values[86] = spool_to_edf(get_metric(ctx, SUM_F_HTUBE_POWER, 2, -1), 1, 10);  /* HTubePow      /10 */
    str_values[87] = spool_to_edf(get_metric(ctx, SUM_F_HUM_POWER, 2, -1), 1, 10);    /* HumPow        /10 */
    str_values[88] = spool_to_edf(get_metric(ctx, SUM_F_SPO2, 2, -1), 1, 100);        /* SpO2.50       /100*/
    str_values[89] = spool_to_edf(get_metric(ctx, SUM_F_SPO2, 3, -1), 1, 100);        /* SpO2.95       /100*/
    str_values[90] = spool_to_edf(get_metric(ctx, SUM_F_SPO2, 4, -1), 1, 100);        /* SpO2.Max      /100*/
    str_values[91] = get_scalar(ctx, SUM_F_SAU, -1);                                  /* SpO2Thresh    no scale */

    /* Spont trigger/cycle percentages [92-93] — scalars, logical_scale = 50. */
    str_values[92] = spool_to_edf(get_scalar(ctx, SUM_F_SPONT_TRIG, -1), 1, 50);  /* SpontTrig% /50 */
    str_values[93] = spool_to_edf(get_scalar(ctx, SUM_F_SPONT_CYC, -1), 1, 50);   /* SpontCyc%  /50 */

    /* Bilevel/ventilation summary stats [94-115] */
    str_values[94] = spool_to_edf(get_metric(ctx, SUM_F_MEAN_MASK_PRESS, 2, -1), 1, 2);  /* MaskPress.50 /2 */
    str_values[95] = spool_to_edf(get_metric(ctx, SUM_F_MEAN_MASK_PRESS, 3, -1), 1, 2);  /* MaskPress.95 /2 */
    str_values[96] = spool_to_edf(get_metric(ctx, SUM_F_MEAN_MASK_PRESS, 4, -1), 1, 2);  /* MaskPress.Max /2 */
    str_values[97] = spool_to_edf(get_metric(ctx, SUM_F_INSP_PRESS, 2, -1), 1, 2);       /* TgtIPAP.50   /2 */
    str_values[98] = spool_to_edf(get_metric(ctx, SUM_F_INSP_PRESS, 3, -1), 1, 2);       /* TgtIPAP.95   /2 */
    str_values[99] = spool_to_edf(get_metric(ctx, SUM_F_INSP_PRESS, 4, -1), 1, 2);       /* TgtIPAP.Max  /2 */
    str_values[100] = spool_to_edf(get_metric(ctx, SUM_F_EXP_PRESS, 2, -1), 1, 2);       /* TgtEPAP.50   /2 */
    str_values[101] = spool_to_edf(get_metric(ctx, SUM_F_EXP_PRESS, 3, -1), 1, 2);       /* TgtEPAP.95   /2 */
    str_values[102] = spool_to_edf(get_metric(ctx, SUM_F_EXP_PRESS, 4, -1), 1, 2);       /* TgtEPAP.Max  /2 */
    str_values[103] = spool_to_edf(get_metric(ctx, SUM_F_LEAK, 2, -1), 1, 2);            /* Leak.50      /2 */
    str_values[104] = spool_to_edf(get_metric(ctx, SUM_F_LEAK, 4, -1), 1, 2);            /* Leak.95      /2 */
    str_values[105] = spool_to_edf(get_metric(ctx, SUM_F_LEAK, 3, -1), 1, 2);            /* Leak.70      /2 */
    str_values[106] = spool_to_edf(get_metric(ctx, SUM_F_LEAK, 5, -1), 1, 2);            /* Leak.Max     /2 */
    str_values[107] = spool_to_edf(get_metric(ctx, SUM_F_MIN_VENT, 2, -1), 2, 25);       /* MinVent.50   /12.5 */
    str_values[108] = spool_to_edf(get_metric(ctx, SUM_F_MIN_VENT, 3, -1), 2, 25);       /* MinVent.95   /12.5 */
    str_values[109] = spool_to_edf(get_metric(ctx, SUM_F_MIN_VENT, 4, -1), 2, 25);       /* MinVent.Max  /12.5 */
    str_values[110] = spool_to_edf(get_metric(ctx, SUM_F_RESP_RATE, 2, -1), 1, 20);      /* RespRate.50  /20 */
    str_values[111] = spool_to_edf(get_metric(ctx, SUM_F_RESP_RATE, 3, -1), 1, 20);      /* RespRate.95  /20 */
    str_values[112] = spool_to_edf(get_metric(ctx, SUM_F_RESP_RATE, 4, -1), 1, 20);      /* RespRate.Max /20 */
    str_values[113] = spool_to_edf(get_metric(ctx, SUM_F_TIDAL_VOL, 2, -1), 1, 2);       /* TidVol.50    /2 */
    str_values[114] = spool_to_edf(get_metric(ctx, SUM_F_TIDAL_VOL, 3, -1), 1, 2);       /* TidVol.95    /2 */
    str_values[115] = spool_to_edf(get_metric(ctx, SUM_F_TIDAL_VOL, 4, -1), 1, 2);       /* TidVol.Max   /2 */

    /* Target minute ventilation [116-118] — logical_scale = 12.5. */
    str_values[116] = spool_to_edf(get_metric(ctx, SUM_F_TGT_VENT, 2, -1), 2, 25);  /* TgtVent.50  /12.5 */
    str_values[117] = spool_to_edf(get_metric(ctx, SUM_F_TGT_VENT, 3, -1), 2, 25);  /* TgtVent.95  /12.5 */
    str_values[118] = spool_to_edf(get_metric(ctx, SUM_F_TGT_VENT, 4, -1), 2, 25);  /* TgtVent.Max /12.5 */

    /* I:E ratio [119-121] — logical_scale = 100. */
    str_values[119] = spool_to_edf(get_metric(ctx, SUM_F_IE_RATIO, 2, -1), 1, 100);  /* IERatio.50  /100 */
    str_values[120] = spool_to_edf(get_metric(ctx, SUM_F_IE_RATIO, 3, -1), 1, 100);  /* IERatio.95  /100 */
    str_values[121] = spool_to_edf(get_metric(ctx, SUM_F_IE_RATIO, 4, -1), 1, 100);  /* IERatio.Max /100 */

    /* Inspiratory duration [122-124] — logical_scale = 20. */
    str_values[122] = spool_to_edf(get_metric(ctx, SUM_F_INSP_DUR, 2, -1), 1, 20);  /* Ti.50  /20 */
    str_values[123] = spool_to_edf(get_metric(ctx, SUM_F_INSP_DUR, 3, -1), 1, 20);  /* Ti.95  /20 */
    str_values[124] = spool_to_edf(get_metric(ctx, SUM_F_INSP_DUR, 4, -1), 1, 20);  /* Ti.Max /20 */

    /* Indices [125-131] — logical_scale = 10 (events/hr × 10 in spool).
     * [132] CSR — logical_scale = 1, no conversion. */
    str_values[125] = spool_to_edf(get_scalar(ctx, SUM_F_AHI, -1), 1, 10);
    str_values[126] = spool_to_edf(get_scalar(ctx, SUM_F_HI, -1), 1, 10);
    str_values[127] = spool_to_edf(get_scalar(ctx, SUM_F_AI, -1), 1, 10);
    str_values[128] = spool_to_edf(get_scalar(ctx, SUM_F_OAI, -1), 1, 10);
    str_values[129] = spool_to_edf(get_scalar(ctx, SUM_F_CAI, -1), 1, 10);
    str_values[130] = spool_to_edf(get_scalar(ctx, SUM_F_UAI, -1), 1, 10);
    str_values[131] = spool_to_edf(get_scalar(ctx, SUM_F_RIN, -1), 1, 10);
    str_values[132] = get_scalar(ctx, SUM_F_CSR, -1);
}

/* Build STR header values [0-3] from summary spool session entries.
 * Fills str_values[0-3] and mask_on_extra/mask_off_extra arrays.
 * period_start_ms is the PeriodStart from the summary spool (epoch ms).
 * Returns the MaskEvents count. */
static int build_str_mask_events(summary_ctx_t *ctx, int16_t *str_values,
                                 int16_t *mask_on_extra, int16_t *mask_off_extra,
                                 int64_t period_start_ms, int64_t clock_drift_ms)
{
    /* Summary timestamps are in AS11 time.  The MaskOn/MaskOff *values* are
     * deliberately exported in NTP time so their session intervals match the
     * NTP-based EDF headers, filenames, and event annotations (OSCAR matches
     * sessions to STR mask events within a ~1 minute window).
     *
     * Two different clocks are involved and both matter:
     *
     *  - The noon-day *bucket* belongs to the AS11.  The device defines its
     *    reporting day noon-to-noon in ITS OWN timezone and stamps
     *    PeriodStart at exactly its local noon.  Applying ESP local time to
     *    that instant is wrong whenever the zones differ: with the AS11 one
     *    hour east, its noon reads as 11:00 ESP-local, trips the "before
     *    noon" test and rolls every record back a whole day — producing
     *    Date-1 and MaskOn/MaskOff +1440.  Values above 1440 exceed the
     *    signal's declared maximum and OSCAR discards such pairs as card
     *    corruption, losing every STR session time (issue #75).
     *    as11_time_noon_day_for_period_start() resolves the bucket using the
     *    AS11's own offset, derived from the noon stamp itself.
     *
     *  - The mask minutes are measured from ESP-LOCAL noon of that bucket,
     *    because consumers reconstruct absolute times as (local noon of the
     *    record's Date) + minutes and compare them with DATALOG filenames,
     *    which are written in ESP local time. */
    char day_label[16];
    as11_time_noon_day_for_period_start(period_start_ms, day_label,
                                        sizeof(day_label));

    int64_t noon_secs = as11_time_local_noon_epoch(day_label);
    int day_number = as11_time_day_number(day_label);
    if (noon_secs == 0 || day_number < 0) {
        /* Should not happen — fall back to the raw instant so we still emit
         * something rather than a zeroed record. */
        ESP_LOGW(TAG, "STR.edf: unusable day label '%s' for PeriodStart %lld",
                 day_label, (long long)period_start_ms);
        noon_secs = period_start_ms / 1000;
        day_number = (int)(noon_secs / 86400);
    }

    /* [0] Date: days from the Unix epoch for the noon-day. */
    str_values[0] = (int16_t)day_number;

    int64_t noon_epoch_ms = noon_secs * 1000;

    /* [1-3] MaskOn/MaskOff/MaskEvents from session entries.
     * Each session entry has: ts = MaskOn timestamp, duration_min = session
     * duration in minutes. MaskOff = MaskOn + duration_min.
     * The AS11 writes MaskOff = MaskOn (not -1) for 0-duration sessions. */
    int mask_on_count = 0;
    int mask_off_count = 0;

    for (int i = 0; i < ctx->n_session_entries && mask_on_count < 20; i++) {
        int64_t event_ntp_ms = ctx->session_entries[i].ts + clock_drift_ms;
        int min_from_noon = (int)((event_ntp_ms - noon_epoch_ms) / 60000);

        /* A pair outside 0..1440 is what OSCAR reports as "Mask times are out
         * of range" and discards, taking the day's session times with it.
         * Sessions crossing the noon boundary or affected by minor clock drift
         * are clamped to [0, 1440], preserving continuous session data while
         * maintaining a valid STR.edf record. Entries with zero overlap are skipped. */
        int dur = (int)ctx->session_entries[i].duration_min;
        int off_min = min_from_noon + dur;

        /* Skip entries with zero overlap with this 24-hour day. */
        if (off_min <= 0 || min_from_noon >= STR_MASK_MINUTES_MAX) {
            ESP_LOGI(TAG, "STR.edf: %s session %d window %d..%d min is "
                     "outside the day — entry skipped", day_label, i,
                     min_from_noon, off_min);
            continue;
        }

        /* Clamp overlapping boundaries to [0, 1440].
         * Clamping happens AFTER off_min calculation so off_min reflects
         * the session's actual end time, not an artificially shifted one. */
        if (min_from_noon < 0) {
            ESP_LOGD(TAG, "STR.edf: %s session %d MaskOn %d clamped to 0",
                     day_label, i, min_from_noon);
            min_from_noon = 0;
        }
        if (off_min > STR_MASK_MINUTES_MAX) {
            ESP_LOGD(TAG, "STR.edf: %s session %d MaskOff %d clamped to %d",
                     day_label, i, off_min, STR_MASK_MINUTES_MAX);
            off_min = STR_MASK_MINUTES_MAX;
        }

        if (mask_on_count == 0)
            str_values[1] = (int16_t)min_from_noon;
        else
            mask_on_extra[mask_on_count - 1] = (int16_t)min_from_noon;
        mask_on_count++;

        if (mask_off_count == 0)
            str_values[2] = (int16_t)off_min;
        else
            mask_off_extra[mask_off_count - 1] = (int16_t)off_min;
        mask_off_count++;
    }

    /* Fallback: use PeriodStart/PeriodEnd if no session entries.
     * Evaluated as a pair to prevent orphaned half-pairs. */
    if (mask_on_count == 0 && mask_off_count == 0 &&
        ctx->has_scalar[SUM_F_PERIOD_START] && ctx->has_scalar[SUM_F_PERIOD_END]) {
        int64_t ps_ntp = ctx->scalars[SUM_F_PERIOD_START] + clock_drift_ms;
        int64_t pe_ntp = ctx->scalars[SUM_F_PERIOD_END] + clock_drift_ms;
        int on = (int)((ps_ntp - noon_epoch_ms) / 60000);
        int off = (int)((pe_ntp - noon_epoch_ms) / 60000);
        if (off > 0 && on < STR_MASK_MINUTES_MAX) {
            if (on < 0) on = 0;
            if (off > STR_MASK_MINUTES_MAX) off = STR_MASK_MINUTES_MAX;
            if (on < off) {
                str_values[1] = (int16_t)on;
                str_values[2] = (int16_t)off;
                mask_on_count = 1;
                mask_off_count = 1;
            }
        }
    }

    int n_pairs = mask_on_count > mask_off_count ? mask_on_count : mask_off_count;
    str_values[3] = (int16_t)(n_pairs * 2);  /* total mask events (on+off), not pair count */
    return str_values[3];
}

/* Per-day STR record for multi-record STR.edf generation. */
typedef struct {
    int16_t values[STR_DATA_COUNT];
    int16_t mask_on_extra[20];
    int16_t mask_off_extra[20];
    int64_t period_start;       /* NTP-corrected — used for data values and sorting */
    int64_t period_start_as11;  /* Raw AS11 clock — used ONLY for noon-day labelling.
                                 * Rationale (audit §5.9): AS11 sets PeriodStart to
                                 * exactly noon in its own clock.  After NTP drift
                                 * correction (~7–8 min) this lands just before noon,
                                 * causing noon_day_folder() to shift every record one
                                 * day back.  Using the raw AS11 value for day labels
                                 * keeps spool-day names aligned with STR records
                                 * while all data timestamps remain NTP-corrected. */
} str_day_record_t;

/* One mask on/off pair, in minutes from the record's local noon. */
typedef struct {
    int on_min;
    int off_min;
} mask_pair_t;

/* Parse one session's events.snt and append its mask window(s).
 *
 * Event reportTime is UTC in the AS11 clock domain, so it is converted with
 * as11_time_parse_iso8601_ms() and shifted by that session's own measured drift.
 * MaskOn/MaskOff are preferred; TherapyStart/TherapyStop are the fallback for
 * very short sessions where the AS11 emits no mask events. */
static int collect_session_mask_pairs(const char *session_dir,
                                     const char *session_id,
                                     int64_t noon_epoch_ms,
                                     int64_t session_drift_ms,
                                     int64_t start_epoch_ms,
                                     int64_t end_epoch_ms,
                                     mask_pair_t *out, int max_out)
{
    if (max_out <= 0) return 0;

    char events_path[400];
    snprintf(events_path, sizeof(events_path), "%s/%s_events.snt",
             session_dir, session_id);

    int n_on = 0, n_off = 0;
    int on_min[20], off_min[20];
    int ts_start_min = -1, ts_stop_min = -1;

    FILE *ef = fopen(events_path, "r");
    if (ef) {
        char line[640];
        while (fgets(line, sizeof(line), ef)) {
            cJSON *msg = cJSON_Parse(line);
            if (!msg) continue;
            cJSON *params = cJSON_GetObjectItem(msg, "params");
            cJSON *events = params ? cJSON_GetObjectItem(params, "events") : NULL;
            if (events && cJSON_IsArray(events)) {
                int n = cJSON_GetArraySize(events);
                for (int i = 0; i < n; i++) {
                    cJSON *ev = cJSON_GetArrayItem(events, i);
                    if (!ev) continue;
                    cJSON *label = cJSON_GetObjectItem(ev, "event");
                    cJSON *rt = cJSON_GetObjectItem(ev, "reportTime");
                    if (!label || !cJSON_IsString(label)) continue;
                    if (!rt || !cJSON_IsString(rt)) continue;

                    int64_t as11_ms = as11_time_parse_iso8601_ms(rt->valuestring);
                    if (as11_ms <= 0) continue;
                    int64_t ntp_ms = as11_ms + session_drift_ms;
                    int min_from_noon = (int)((ntp_ms - noon_epoch_ms) / 60000);

                    const char *l = label->valuestring;
                    if (strcmp(l, "MaskOn") == 0) {
                        if (n_on < 20) on_min[n_on++] = min_from_noon;
                    } else if (strcmp(l, "MaskOff") == 0) {
                        if (n_off < 20) off_min[n_off++] = min_from_noon;
                    } else if (strcmp(l, "TherapyStart") == 0) {
                        if (ts_start_min < 0) ts_start_min = min_from_noon;
                    } else if (strcmp(l, "TherapyStop") == 0) {
                        ts_stop_min = min_from_noon;
                    }
                }
            }
            cJSON_Delete(msg);
        }
        fclose(ef);
    }

    /* Fall back to therapy events, then to the session's own span, so a
     * session always contributes a window rather than disappearing. */
    if (n_on == 0) {
        if (ts_start_min >= 0) {
            on_min[n_on++] = ts_start_min;
        } else {
            on_min[n_on++] = (int)((start_epoch_ms - noon_epoch_ms) / 60000);
        }
    }
    if (n_off == 0) {
        if (ts_stop_min >= 0) {
            off_min[n_off++] = ts_stop_min;
        } else if (end_epoch_ms > start_epoch_ms) {
            off_min[n_off++] = (int)((end_epoch_ms - noon_epoch_ms) / 60000);
        } else {
            off_min[n_off++] = on_min[0];
        }
    }

    /* Pair them up positionally; the AS11 writes MaskOff == MaskOn for a
     * zero-length session rather than omitting it. */
    int pairs = n_on > n_off ? n_on : n_off;
    int written = 0;
    for (int i = 0; i < pairs && written < max_out; i++) {
        int on = (i < n_on) ? on_min[i] : on_min[n_on - 1];
        int off = (i < n_off) ? off_min[i] : on;
        if (off < on) off = on;

        /* Skip entries with zero overlap with this 24-hour day. */
        if (off <= 0 || on >= STR_MASK_MINUTES_MAX) {
            ESP_LOGI(TAG, "STR.edf: %s window %d..%d min is outside the day "
                     "— entry skipped", session_id, on, off);
            continue;
        }
        /* Clamp overlapping boundaries to [0, 1440]. */
        if (on < 0) on = 0;
        if (off > STR_MASK_MINUTES_MAX) off = STR_MASK_MINUTES_MAX;
        out[written].on_min = on;
        out[written].off_min = off;
        written++;
    }
    return written;
}

/* Build a STR record for a noon-day directly from session data (events.snt,
 * start/end times, settings).  Used when the AS11's Summary spool does not
 * yet cover that day — normally only the current, still-incomplete day.
 *
 * Every session belonging to the day is included, not just the one being
 * exported: a day with several sessions previously reported only the last
 * one, so MaskEvents and the mask windows under-reported the night.
 *
 * Fills rec->values, mask_on_extra, mask_off_extra, period_start. */
static void build_current_day_record(str_day_record_t *rec,
                                     const char *session_dir,
                                     const char *session_id,
                                     int64_t start_epoch_ms,
                                     int64_t end_epoch_ms,
                                     int64_t clock_drift_ms,
                                     const cJSON *settings_json)
{
    memset(rec->values, 0xFF, STR_DATA_COUNT * sizeof(int16_t));
    memset(rec->mask_on_extra, 0xFF, sizeof(rec->mask_on_extra));
    memset(rec->mask_off_extra, 0xFF, sizeof(rec->mask_off_extra));
    rec->period_start = start_epoch_ms;
    rec->period_start_as11 = start_epoch_ms - clock_drift_ms;

    /* Settings first.  build_str_data_values() writes Duration from its
     * (here empty) Summary context, so it must not run after the values
     * below or it silently overwrites Duration with 0 — which is what made
     * the synthesised day report zero usage. */
    summary_ctx_t *empty_ctx = calloc(1, sizeof(summary_ctx_t));
    if (!empty_ctx) {
        ESP_LOGE(TAG, "STR.edf: calloc failed for current-day ctx");
        return;
    }
    build_str_data_values(empty_ctx, rec->values, settings_json);
    free(empty_ctx);

    /* The day bucket follows the AS11's own noon boundary when its timezone
     * is known, so this record lands on the same day as the spool records
     * around it.  Mask minutes are then measured from ESP-LOCAL noon of that
     * day, because consumers add them to local noon and compare the result
     * with DATALOG filenames (written in ESP local time). */
    char day_label[16];
    as11_time_noon_day(start_epoch_ms - clock_drift_ms, day_label,
                       sizeof(day_label));

    int64_t noon_secs = as11_time_local_noon_epoch(day_label);
    int day_number = as11_time_day_number(day_label);
    if (noon_secs == 0 || day_number < 0) {
        ESP_LOGW(TAG, "STR.edf: unusable day label '%s' for session %s",
                 day_label, session_id);
        return;
    }
    int64_t noon_epoch_ms = noon_secs * 1000;

    /* [0] Date */
    rec->values[0] = (int16_t)day_number;

    /* [1-3] Mask windows, aggregated over every session of this noon-day. */
    mask_pair_t pairs[21];
    int n_pairs = 0;
    int64_t usage_ms = 0;

    DIR *d = opendir(session_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && n_pairs < 20) {
            const char *suffix = "_session.json";
            size_t slen = strlen(suffix), flen = strlen(ent->d_name);
            if (flen <= slen || strcmp(ent->d_name + flen - slen, suffix) != 0)
                continue;

            char sid[48];
            size_t plen = flen - slen;
            if (plen == 0 || plen >= sizeof(sid)) continue;
            memcpy(sid, ent->d_name, plen);
            sid[plen] = '\0';

            char json_path[420];
            snprintf(json_path, sizeof(json_path), "%s/%s", session_dir, ent->d_name);
            cJSON *j = edf_read_json_file(json_path);
            if (!j) continue;

            cJSON *js = cJSON_GetObjectItem(j, "start_epoch_ms");
            cJSON *je = cJSON_GetObjectItem(j, "end_epoch_ms");
            cJSON *jd = cJSON_GetObjectItem(j, "clock_drift_ms");
            int64_t s_start = (js && cJSON_IsNumber(js)) ? (int64_t)js->valuedouble : 0;
            int64_t s_end = (je && cJSON_IsNumber(je)) ? (int64_t)je->valuedouble : 0;
            int64_t s_drift = (jd && cJSON_IsNumber(jd)) ? (int64_t)jd->valuedouble
                                                         : clock_drift_ms;
            cJSON_Delete(j);
            if (s_start <= 0) continue;

            /* Only sessions belonging to this noon-day. */
            char sess_day[16];
            as11_time_noon_day(s_start - s_drift, sess_day, sizeof(sess_day));
            if (strcmp(sess_day, day_label) != 0) continue;

            int got = collect_session_mask_pairs(session_dir, sid, noon_epoch_ms,
                                                 s_drift, s_start, s_end,
                                                 &pairs[n_pairs], 20 - n_pairs);
            n_pairs += got;
            if (s_end > s_start) usage_ms += (s_end - s_start);
        }
        closedir(d);
    }

    /* The exported session must be represented even if its manifest is not on
     * the card yet (it is written after this runs on the very first export). */
    bool have_self = false;
    {
        char self_day[16];
        as11_time_noon_day(start_epoch_ms - clock_drift_ms, self_day, sizeof(self_day));
        if (strcmp(self_day, day_label) == 0) {
            char self_json[420];
            snprintf(self_json, sizeof(self_json), "%s/%s_session.json",
                     session_dir, session_id);
            struct stat st;
            have_self = (stat(self_json, &st) == 0);
        } else {
            have_self = true;   /* not part of this day at all */
        }
    }
    if (!have_self && n_pairs < 20) {
        int got = collect_session_mask_pairs(session_dir, session_id, noon_epoch_ms,
                                             clock_drift_ms, start_epoch_ms,
                                             end_epoch_ms, &pairs[n_pairs],
                                             20 - n_pairs);
        n_pairs += got;
        if (end_epoch_ms > start_epoch_ms) usage_ms += (end_epoch_ms - start_epoch_ms);
    }

    /* Chronological order, as the AS11 writes them. */
    for (int i = 1; i < n_pairs; i++) {
        mask_pair_t tmp = pairs[i];
        int k = i - 1;
        while (k >= 0 && pairs[k].on_min > tmp.on_min) {
            pairs[k + 1] = pairs[k];
            k--;
        }
        pairs[k + 1] = tmp;
    }

    for (int i = 0; i < n_pairs; i++) {
        if (i == 0) {
            rec->values[1] = (int16_t)pairs[i].on_min;
            rec->values[2] = (int16_t)pairs[i].off_min;
        } else {
            rec->mask_on_extra[i - 1] = (int16_t)pairs[i].on_min;
            rec->mask_off_extra[i - 1] = (int16_t)pairs[i].off_min;
        }
    }
    rec->values[3] = (int16_t)(n_pairs * 2);   /* on + off events */

    /* [4] Duration: total therapy minutes for the day. */
    int64_t usage_min = usage_ms / 60000;
    if (usage_min > STR_MASK_MINUTES_MAX) usage_min = STR_MASK_MINUTES_MAX;
    rec->values[4] = (int16_t)usage_min;

    ESP_LOGI(TAG, "STR.edf: synthesized record for %s "
             "(%d session(s), MaskOn=%d MaskOff=%d Duration=%d min)",
             day_label, n_pairs, (int)rec->values[1], (int)rec->values[2],
             (int)rec->values[4]);
}

/* Resolve the drift for one AS11 Summary noon-day.  Session metadata stores
 * the drift measured when that session stopped; choose the session closest to
 * the Summary PeriodStart.  This lets cumulative STR.edf keep historical days
 * NTP-corrected even after the AS11 clock changes later. */
typedef struct {
    int64_t start_as11_ms;
    int64_t drift_ms;
    char as11_day[16];
} session_drift_entry_t;

/* Build an in-memory index of all session drift entries by scanning
 * .somnotrace/sessions/streams/ once.  Returns a malloc'd array (caller frees) or
 * NULL on failure.  *out_count receives the number of entries. */
static session_drift_entry_t *build_session_drift_index(int *out_count)
{
    *out_count = 0;
    DIR *streams_dir = opendir(SD_STREAMS_DIR);
    if (!streams_dir) return NULL;

    int cap = 0, n = 0;
    session_drift_entry_t *entries = NULL;

    struct dirent *day_ent;
    while ((day_ent = readdir(streams_dir)) != NULL) {
        if (strlen(day_ent->d_name) != 8) continue;

        char stream_day_path[300];
        snprintf(stream_day_path, sizeof(stream_day_path), "%s/%s",
                 SD_STREAMS_DIR, day_ent->d_name);
        DIR *stream_day_dir = opendir(stream_day_path);
        if (!stream_day_dir) continue;

        struct dirent *session_ent;
        while ((session_ent = readdir(stream_day_dir)) != NULL) {
            size_t name_len = strlen(session_ent->d_name);
            if (name_len < 13 ||
                strcmp(session_ent->d_name + name_len - 13, "_session.json") != 0) {
                continue;
            }

            char session_path[600];
            snprintf(session_path, sizeof(session_path), "%s/%s",
                     stream_day_path, session_ent->d_name);
            cJSON *session = edf_read_json_file(session_path);
            if (!session) continue;

            cJSON *start_j = cJSON_GetObjectItem(session, "start_epoch_ms");
            cJSON *drift_j = cJSON_GetObjectItem(session, "clock_drift_ms");
            if (cJSON_IsNumber(start_j) && cJSON_IsNumber(drift_j)) {
                if (n >= cap) {
                    int new_cap = cap ? cap * 2 : 16;
                    session_drift_entry_t *tmp = realloc(entries,
                        new_cap * sizeof(session_drift_entry_t));
                    if (!tmp) {
                        cJSON_Delete(session);
                        continue;
                    }
                    entries = tmp;
                    cap = new_cap;
                }
                int64_t start_ntp_ms = (int64_t)start_j->valuedouble;
                int64_t drift_ms = (int64_t)drift_j->valuedouble;
                entries[n].start_as11_ms = start_ntp_ms - drift_ms;
                entries[n].drift_ms = drift_ms;
                /* Same AS11 noon boundary the spool records are keyed on, so
                 * a day's drift lookup cannot miss by one day. */
                as11_time_noon_day(entries[n].start_as11_ms, entries[n].as11_day,
                                   sizeof(entries[n].as11_day));
                n++;
            }
            cJSON_Delete(session);
        }
        closedir(stream_day_dir);
    }
    closedir(streams_dir);

    *out_count = n;
    return entries;
}

/* Look up the best-matching drift for a given AS11 noon-day and PeriodStart
 * from the in-memory index.  Falls back to fallback_drift_ms if no match. */
static int64_t lookup_drift(const session_drift_entry_t *entries, int n_entries,
                            const char *as11_day_label,
                            int64_t period_start_ms,
                            int64_t fallback_drift_ms)
{
    int64_t resolved_drift_ms = fallback_drift_ms;
    int64_t best_distance_ms = INT64_MAX;

    for (int i = 0; i < n_entries; i++) {
        if (strcmp(entries[i].as11_day, as11_day_label) != 0) continue;
        int64_t distance_ms = llabs(entries[i].start_as11_ms - period_start_ms);
        if (distance_ms < best_distance_ms) {
            best_distance_ms = distance_ms;
            resolved_drift_ms = entries[i].drift_ms;
        }
    }
    return resolved_drift_ms;
}

/* Build a JSON summary for one noon-day from its Summary spool, for the
 * dashboard. Reuses the same protobuf parser and physical-unit scaling as the
 * STR.edf export, so values match OSCAR/SleepHQ. Returns:
 *   - ESP_OK and *out_json (caller frees) on success
 *   - ESP_ERR_NOT_FOUND if the day has no spool yet
 *
 * Physical conversion (raw spool int = physical × 100 for these fields):
 *   indices/pressure/resp-rate  → raw × 0.01
 *   leak (stored L/s × 100)     → raw × 0.6  (= L/min)
 * Metric percentile sub-indices: pressure/resp median=2 p95=3 max=4;
 *   leak median=2 p70=3 p95=4 max=5. */
esp_err_t edf_gen_summary_json(const char *noon_day, char **out_json)
{
    if (!noon_day || !out_json) return ESP_ERR_INVALID_ARG;
    *out_json = NULL;

    char spool_path[300];
    snprintf(spool_path, sizeof(spool_path), "%s/%s.spool", SD_SUMMARIES_DIR, noon_day);
    size_t spool_len = 0;
    uint8_t *spool_data = edf_read_bin_file(spool_path, &spool_len);
    if (!spool_data || spool_len == 0) {
        free(spool_data);
        return ESP_ERR_NOT_FOUND;
    }

    summary_ctx_t *ctx = calloc(1, sizeof(summary_ctx_t));
    if (!ctx) { free(spool_data); return ESP_ERR_NO_MEM; }
    pb_iter(spool_data, spool_len, summary_field_cb, ctx);
    free(spool_data);

    /* Total usage (mask-on) minutes and session count from SessionModeEntries. */
    int64_t usage_min = 0;
    for (int i = 0; i < ctx->n_session_entries; i++) {
        usage_min += ctx->session_entries[i].duration_min;
    }
    if (usage_min == 0 && ctx->has_scalar[SUM_F_DURATION_MIN]) {
        usage_min = ctx->scalars[SUM_F_DURATION_MIN];
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) { free(ctx); return ESP_ERR_NO_MEM; }

    cJSON_AddStringToObject(root, "day", noon_day);
    cJSON_AddNumberToObject(root, "sessions", ctx->n_session_entries);
    cJSON_AddNumberToObject(root, "usage_min", (double)usage_min);

    /* Indices (events/hr): raw × 0.01. -1 sentinel → null. */
    #define ADD_INDEX(key, field) do { \
        int16_t r = get_scalar(ctx, field, -1); \
        if (r < 0) cJSON_AddNullToObject(root, key); \
        else cJSON_AddNumberToObject(root, key, r * 0.01); \
    } while (0)
    ADD_INDEX("ahi",  SUM_F_AHI);
    ADD_INDEX("ai",   SUM_F_AI);
    ADD_INDEX("hi",   SUM_F_HI);
    ADD_INDEX("oai",  SUM_F_OAI);
    ADD_INDEX("cai",  SUM_F_CAI);
    ADD_INDEX("uai",  SUM_F_UAI);
    ADD_INDEX("rera", SUM_F_RIN);
    #undef ADD_INDEX

    /* Percentile groups. factor: pressure/resp = 0.01 cmH2O/bpm; leak = 0.6 L/min. */
    #define ADD_STAT3(key, field, s_med, s_p95, s_max, factor) do { \
        cJSON *o = cJSON_CreateObject(); \
        int16_t m = get_metric(ctx, field, s_med, -1); \
        int16_t p = get_metric(ctx, field, s_p95, -1); \
        int16_t x = get_metric(ctx, field, s_max, -1); \
        if (m < 0) cJSON_AddNullToObject(o, "median"); else cJSON_AddNumberToObject(o, "median", m * (factor)); \
        if (p < 0) cJSON_AddNullToObject(o, "p95");    else cJSON_AddNumberToObject(o, "p95",    p * (factor)); \
        if (x < 0) cJSON_AddNullToObject(o, "max");    else cJSON_AddNumberToObject(o, "max",    x * (factor)); \
        cJSON_AddItemToObject(root, key, o); \
    } while (0)
    ADD_STAT3("pressure",  SUM_F_MEAN_MASK_PRESS, 2, 3, 4, 0.01);
    ADD_STAT3("epap",      SUM_F_EXP_PRESS,       2, 3, 4, 0.01);
    ADD_STAT3("resp_rate", SUM_F_RESP_RATE,       2, 3, 4, 0.01);
    /* Leak percentiles: median=2, p95=4, max=5. */
    ADD_STAT3("leak",      SUM_F_LEAK,            2, 4, 5, 0.6);
    #undef ADD_STAT3

    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(ctx);
    if (!js) return ESP_ERR_NO_MEM;
    *out_json = js;
    return ESP_OK;
}

/* Generate multi-record STR.edf from per-day summary spool files.
 * Scans .somnotrace/sessions/summaries/ for *.spool files, parses each into a
 * daily STR record, and writes them all into a single STR.edf at
 * <sdcard_dir>/STR.edf with one EDF data record per day.
 * If the current day is not covered by any spool, synthesizes a record
 * from the current session's data (events.snt, start/end times). */
esp_err_t edf_generate_str_edf(const char *sdcard_dir,
                                  const char *patient_id, const char *recording_id,
                                  const char *start_date,
                                  const cJSON *settings_json,
                                  const char *session_dir,
                                  const char *session_id,
                                  int64_t start_epoch_ms, int64_t end_epoch_ms,
                                  int64_t clock_drift_ms)
{
    (void)start_date;  /* computed internally from oldest record */
    /* ── Scan .somnotrace/sessions/summaries/ for per-day .spool files ── */
    DIR *dir = opendir(SD_SUMMARIES_DIR);
    if (!dir) {
        ESP_LOGE(TAG, "STR.edf: cannot open %s: %s", SD_SUMMARIES_DIR, strerror(errno));
        return ESP_FAIL;
    }

    /* First pass: collect all spool filenames, sort, keep newest 30.
     * readdir order is non-deterministic, so we must collect all names
     * and sort to ensure deterministic day selection. */
    char (*spool_names)[15] = NULL;
    int n_spool = 0, spool_cap = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *nm = ent->d_name;
        size_t nlen = strlen(nm);
        if (nlen != 14 || strcmp(nm + 8, ".spool") != 0) continue;
        bool digits_ok = true;
        for (int i = 0; i < 8; i++) {
            if (nm[i] < '0' || nm[i] > '9') { digits_ok = false; break; }
        }
        if (!digits_ok) continue;

        if (n_spool >= spool_cap) {
            int new_cap = spool_cap ? spool_cap * 2 : 16;
            char (*tmp)[15] = realloc(spool_names, new_cap * sizeof(*spool_names));
            if (!tmp) {
                ESP_LOGE(TAG, "STR.edf: spool name array realloc failed");
                free(spool_names);
                closedir(dir);
                return ESP_ERR_NO_MEM;
            }
            spool_names = tmp;
            spool_cap = new_cap;
        }
        strncpy(spool_names[n_spool], nm, 14);
        spool_names[n_spool][14] = '\0';
        n_spool++;
    }
    closedir(dir);

    /* Sort filenames lexicographically — YYYYMMDD.spool sorts chronologically */
    for (int i = 0; i < n_spool - 1; i++) {
        for (int j = i + 1; j < n_spool; j++) {
            if (strcmp(spool_names[j], spool_names[i]) < 0) {
                char tmp[15];
                strcpy(tmp, spool_names[i]);
                strcpy(spool_names[i], spool_names[j]);
                strcpy(spool_names[j], tmp);
            }
        }
    }

    /* Walk newest → oldest and stop once 30 *distinct* days have been
     * collected, rather than simply taking the newest 30 filenames.
     *
     * Spool files written before the day-labelling fix carry a name that is
     * one day off, so the same night can be present under two names.  Taking
     * the newest 30 names would let those duplicates crowd out genuinely
     * older days; selecting by distinct period keeps the window honest while
     * the stale names age out. */
    #define STR_MAX_DAYS  365

    str_day_record_t *records = heap_caps_calloc(STR_MAX_DAYS + 1, sizeof(str_day_record_t), MALLOC_CAP_SPIRAM);
    if (!records) records = calloc(STR_MAX_DAYS + 1, sizeof(str_day_record_t));
    summary_ctx_t *ctx = heap_caps_calloc(1, sizeof(summary_ctx_t), MALLOC_CAP_SPIRAM);
    if (!ctx) ctx = calloc(1, sizeof(summary_ctx_t));
    edf_signal_def_t *str_sigs = heap_caps_calloc(STR_DATA_COUNT, sizeof(edf_signal_def_t), MALLOC_CAP_SPIRAM);
    if (!str_sigs) str_sigs = calloc(STR_DATA_COUNT, sizeof(edf_signal_def_t));
    if (!records || !ctx || !str_sigs) {
        ESP_LOGE(TAG, "STR.edf: malloc failed");
        free(records); free(ctx); free(str_sigs);
        free(spool_names);
        return ESP_ERR_NO_MEM;
    }

    /* Build session drift index once (avoids O(days × sessions) dir scans). */
    int n_drift_entries = 0;
    session_drift_entry_t *drift_index = build_session_drift_index(&n_drift_entries);
    ESP_LOGI(TAG, "STR.edf: drift index: %d session entries", n_drift_entries);

    /* Second pass: parse spool files, newest first, until STR_MAX_DAYS
     * distinct days are held.  Records are sorted chronologically below. */
    int n_records = 0;
    for (int si = n_spool - 1; si >= 0 && n_records < STR_MAX_DAYS; si--) {
        const char *nm = spool_names[si];

        /* Read the spool file */
        char spool_path[300];
        snprintf(spool_path, sizeof(spool_path), "%s/%s", SD_SUMMARIES_DIR, nm);
        size_t spool_len = 0;
        uint8_t *spool_data = edf_read_bin_file(spool_path, &spool_len);
        if (!spool_data || spool_len == 0) {
            ESP_LOGW(TAG, "STR.edf: skipping %s (empty or unreadable)", nm);
            free(spool_data);
            continue;
        }

        /* Parse the spool file directly as a Summary record.
         * collect_summary_spool writes the inner record content (not
         * wrapped in a field-2 tag), so we iterate it directly. */
        memset(ctx, 0, sizeof(*ctx));
        pb_iter(spool_data, spool_len, summary_field_cb, ctx);

        int64_t period_start = 0;
        bool found_ps = false;
        if (ctx->has_scalar[SUM_F_PERIOD_START]) {
            period_start = ctx->scalars[SUM_F_PERIOD_START];
            found_ps = true;
        }
        free(spool_data);

        if (!found_ps) {
            ESP_LOGW(TAG, "STR.edf: skipping %s (no PeriodStart)", nm);
            continue;
        }

        /* Build the record */
        str_day_record_t *rec = &records[n_records];
        memset(rec->values, 0xFF, STR_DATA_COUNT * sizeof(int16_t));
        memset(rec->mask_on_extra, 0xFF, sizeof(rec->mask_on_extra));
        memset(rec->mask_off_extra, 0xFF, sizeof(rec->mask_off_extra));

        /* Label from the AS11's own noon boundary, not the ESP's (issue #75).
         * The filename is not authoritative: files written before this fix
         * carry the shifted name, so the day is always re-derived from the
         * PeriodStart inside the record. */
        char as11_day_label[16];
        as11_time_noon_day_for_period_start(period_start, as11_day_label,
                                            sizeof(as11_day_label));

        /* A legacy filename and a correctly-named one can describe the same
         * period.  Keep the first and skip the duplicate, so the same night
         * cannot appear twice in STR.edf. */
        bool dup = false;
        for (int k = 0; k < n_records; k++) {
            if (records[k].period_start_as11 == period_start) { dup = true; break; }
        }
        if (dup) {
            ESP_LOGD(TAG, "STR.edf: %s duplicates an already-parsed period "
                     "(day %s) — skipping", nm, as11_day_label);
            continue;
        }

        int64_t record_drift_ms = lookup_drift(drift_index, n_drift_entries,
                                                as11_day_label,
                                                period_start,
                                                clock_drift_ms);
        build_str_mask_events(ctx, rec->values, rec->mask_on_extra,
                              rec->mask_off_extra, period_start, record_drift_ms);

        /* Resolve settings for this specific day:
         * 1. Check SD_SUMMARIES_DIR/<day>.settings.json.
         * 2. Check SD_STREAMS_DIR/<day>/ settings files (and backfill).
         * 3. Fallback to settings_json passed in (current session). */
        cJSON *day_settings = NULL;
        char sum_settings_path[300];
        snprintf(sum_settings_path, sizeof(sum_settings_path), "%s/%s.settings.json",
                 SD_SUMMARIES_DIR, as11_day_label);
        day_settings = edf_read_json_file(sum_settings_path);
        if (!day_settings) {
            char stream_day_dir[300];
            snprintf(stream_day_dir, sizeof(stream_day_dir), "%s/%s",
                     SD_STREAMS_DIR, as11_day_label);
            DIR *sdd = opendir(stream_day_dir);
            if (sdd) {
                struct dirent *se;
                while ((se = readdir(sdd)) != NULL) {
                    size_t sel = strlen(se->d_name);
                    if (sel > 14 && strcmp(se->d_name + sel - 14, "_settings.json") == 0) {
                        char stream_set_path[600];
                        snprintf(stream_set_path, sizeof(stream_set_path), "%s/%s",
                                 stream_day_dir, se->d_name);
                        day_settings = edf_read_json_file(stream_set_path);
                        if (day_settings) {
                            edf_write_json_file(sum_settings_path, day_settings);
                            break;
                        }
                    }
                }
                closedir(sdd);
            }
        }

        const cJSON *settings_to_use = day_settings ? day_settings : settings_json;
        build_str_data_values(ctx, rec->values, settings_to_use);
        if (day_settings) {
            cJSON_Delete(day_settings);
        }

        rec->period_start = period_start + record_drift_ms;
        rec->period_start_as11 = period_start;  /* raw AS11 for day labelling */
        n_records++;

        ESP_LOGD(TAG, "STR.edf: parsed %s (PeriodStart=%lld, drift=%lld, Duration=%d)",
                 nm, (long long)period_start, (long long)record_drift_ms,
                 rec->values[4]);
    }
    free(spool_names);
    free(drift_index);

    /* Check whether the exported session's day is already covered by a spool
     * record; if not, synthesise one from session data.
     *
     * Both sides of the comparison use the AS11's own noon boundary, so a
     * timezone difference between device and ESP can no longer make the day
     * look uncovered and force a synthesised record every single time
     * (issue #75). */
    char current_day_label[16];
    as11_time_noon_day(start_epoch_ms - clock_drift_ms, current_day_label,
                       sizeof(current_day_label));
    ESP_LOGI(TAG, "STR.edf: current day label=%s (start_epoch_ms=%lld)",
             current_day_label, (long long)start_epoch_ms);
    bool current_day_found = false;
    for (int i = 0; i < n_records; i++) {
        char rec_day[16];
        as11_time_noon_day_for_period_start(records[i].period_start_as11,
                                            rec_day, sizeof(rec_day));
        ESP_LOGD(TAG, "STR.edf: record[%d] day=%s (period_start=%lld)",
                 i, rec_day, (long long)records[i].period_start);
        if (strcmp(rec_day, current_day_label) == 0) {
            current_day_found = true;
            break;
        }
    }
    ESP_LOGI(TAG, "STR.edf: current_day_found=%d n_records=%d",
             current_day_found, n_records);
    if (!current_day_found && n_records < STR_MAX_DAYS + 1) {
        ESP_LOGI(TAG, "STR.edf: synthesizing current day record (day=%s)",
                 current_day_label);
        build_current_day_record(&records[n_records], session_dir,
                                 session_id,
                                 start_epoch_ms, end_epoch_ms, clock_drift_ms,
                                 settings_json);
        n_records++;
    }

    if (n_records == 0) {
        ESP_LOGW(TAG, "STR.edf: no valid summary spool files found in %s", SD_SUMMARIES_DIR);
        free(records); free(ctx); free(str_sigs);
        return ESP_FAIL;
    }

    /* Sort records by period_start (chronological) */
    for (int i = 0; i < n_records - 1; i++) {
        for (int j = i + 1; j < n_records; j++) {
            if (records[j].period_start < records[i].period_start) {
                str_day_record_t tmp = records[i];
                records[i] = records[j];
                records[j] = tmp;
            }
        }
    }

    /* STR.edf signal definitions */
    memcpy(str_sigs, g_str_signals, STR_DATA_COUNT * sizeof(edf_signal_def_t));

    /* Create STR.edf at SDCARD root */
    char path[300];
    snprintf(path, sizeof(path), "%s/STR.edf", sdcard_dir);

    char tmp_path[380];
    FILE *edf = edf_open_atomic_file(path, tmp_path, sizeof(tmp_path));
    if (!edf) {
        ESP_LOGE(TAG, "cannot create %s: %s", path, strerror(errno));
        free(records); free(ctx); free(str_sigs);
        return ESP_FAIL;
    }

    /* STR.edf uses 12.00.00 noon as start time.
     * The start date must correspond to the oldest record (records[0] after
     * sorting), not the current session — otherwise SleepHQ misaligns every
     * record by the offset between the header date and the actual first day.
     *
     * The header date must name the same day as the first record's Date
     * signal, so it is derived from the identical AS11 noon-day label rather
     * than re-deriving it from the timestamp with different rules. */
    const char *str_start_time = "12.00.00";
    char first_day_label[16];
    if (records[0].period_start_as11 > 0) {
        as11_time_noon_day_for_period_start(records[0].period_start_as11,
                                            first_day_label,
                                            sizeof(first_day_label));
    } else {
        as11_time_noon_day(records[0].period_start, first_day_label,
                           sizeof(first_day_label));
    }
    int fd_y = 0, fd_m = 0, fd_d = 0;
    if (sscanf(first_day_label, "%4d%2d%2d", &fd_y, &fd_m, &fd_d) != 3) {
        fd_y = 2000; fd_m = 1; fd_d = 1;
    }

    char str_date[32];
    snprintf(str_date, sizeof(str_date), "%02d.%02d.%02d",
             fd_d, fd_m, fd_y % 100);
    ESP_LOGI(TAG, "STR.edf: header date=%s (from oldest record PeriodStart=%lld)",
             str_date, (long long)records[0].period_start);

    /* Rewrite the recording_id Startdate to match the header start_date
     * (oldest record), not the current session's noon day.  The recording_id
     * format is "Startdate DD-MMM-YYYY X X X SRN=...". */
    char fixed_recording_id[128];
    {
        /* Same day label as the header date above. */
        static const char *mon_names[] = {
            "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
            "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
        };
        /* Find the tail after "Startdate DD-MMM-YYYY" in the original */
        const char *tail = recording_id;
        if (strncmp(tail, "Startdate ", 10) == 0) {
            /* Skip "Startdate " + date (11 chars for "DD-MMM-YYYY") */
            tail += 10;
            while (*tail && *tail != ' ') tail++;
        }
        snprintf(fixed_recording_id, sizeof(fixed_recording_id),
                 "Startdate %02d-%.3s-%04d%.100s",
                 fd_d, mon_names[(fd_m - 1) % 12], fd_y, tail);
    }

    int header_bytes = edf_write_header(edf, patient_id, fixed_recording_id,
                                        str_date, str_start_time,
                                        n_records,
                                        "86400.00",
                                        "EDF", str_sigs, STR_DATA_COUNT);
    if (header_bytes < 0) {
        ESP_LOGE(TAG, "STR.edf: edf_write_header failed");
        edf_discard_atomic_file(edf, tmp_path);
        free(records); free(ctx); free(str_sigs);
        return ESP_FAIL;
    }

    /* Write one data record per day.
     * Each record: 171 data int16 + 1 CRC int16 = 344 bytes. */
    for (int r = 0; r < n_records; r++) {
        int16_t rec_buf[171];
        int rec_pos = 0;
        for (int i = 0; i < STR_DATA_COUNT; i++) {
            int spr = g_str_signals[i].samples_per_record;
            rec_buf[rec_pos++] = records[r].values[i];
            for (int s = 1; s < spr; s++) {
                if (i == 1 && s - 1 < 20)
                    rec_buf[rec_pos++] = records[r].mask_on_extra[s - 1];
                else if (i == 2 && s - 1 < 20)
                    rec_buf[rec_pos++] = records[r].mask_off_extra[s - 1];
                else
                    rec_buf[rec_pos++] = -1;
            }
        }
        if (rec_pos != 171) {
            ESP_LOGE(TAG, "STR.edf: internal error: rec_pos=%d != 171", rec_pos);
            edf_discard_atomic_file(edf, tmp_path);
            free(records); free(ctx); free(str_sigs);
            return ESP_FAIL;
        }

        uint16_t crc = edf_crc16_ccitt((uint8_t *)rec_buf, 171 * sizeof(int16_t));
        if (!edf_write_all(edf, rec_buf, 171 * sizeof(int16_t))) {
            ESP_LOGE(TAG, "STR.edf: write failed at record %d", r);
            edf_discard_atomic_file(edf, tmp_path);
            free(records); free(ctx); free(str_sigs);
            return ESP_FAIL;
        }
        int16_t crc_val = (int16_t)crc;
        if (!edf_write_all(edf, &crc_val, sizeof(crc_val))) {
            ESP_LOGE(TAG, "STR.edf: CRC write failed at record %d", r);
            edf_discard_atomic_file(edf, tmp_path);
            free(records); free(ctx); free(str_sigs);
            return ESP_FAIL;
        }
    }

    if (edf_finalize_atomic_file(edf, tmp_path, path) != ESP_OK) {
        ESP_LOGE(TAG, "cannot finalize %s: %s", path, strerror(errno));
        free(records); free(ctx); free(str_sigs);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "STR.edf generated: %s (%d records)", path, n_records);

    free(records);
    free(ctx);
    free(str_sigs);
    return ESP_OK;
}

