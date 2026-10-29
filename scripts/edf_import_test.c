/*
 * SomnoTrace - Host unit tests for the EDF → SNT import engine
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3, or (at your option) any later version.
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

/* Black-box suite for main/edf_import.c (compiled with -DEDF_IMPORT_HOST so
 * the FreeRTOS worker + edf_gen/uploader hooks stay out).  All timestamps
 * run under TZ=UTC so device-civil == UTC epoch is exact.
 *
 *   phase a  full DATALOG/<day>/ session (BRP+SA2+PLD+EVE+CSL): every .snt
 *            header/value, flow_mm envelope, events.json, manifest fields,
 *            source deletion, file_import drift provenance
 *   phase b  AS10-style day dir (IMPORT/<day>/) + Identification.tgt +
 *            missing SA2 → all-missing fill; AS10 .tgt → flat ident
 *   phase c  torn BRP tail → record clamp; header lying n_records → clamp
 *   phase d  EVE/CSL TAL annotations → canonical event names, onsets,
 *            unknown labels skipped, CSR pair
 *   phase e  _bridge_drift.json → somnobridge drift fields in the manifest
 *   phase f  re-import = duplicate (skipped, sources consumed); different
 *            session at same second → _2 suffix
 *   phase g  STR.edf → per-day .spool protobuf + <day>.settings.json;
 *            AS11 Identification.json → flat ident
 *   phase h  crash residue: .importing marker + staged .part files removed
 *            on the next scan; committed marker left alone
 *   phase i  annotation-only session (EVE+CSL, no waveforms) → empty
 *            streams + events + manifest
 *   phase k  AS10 PLD aliases, STR label boundary, S.* settings overlay
 *   phase l  real ResMed EVE/CSL layout (0.00 s records, one per
 *            annotation): events kept, mask-less span, 0 s waveform → .bad
 *   phase m  drift sidecar size budget: verbatim embed when small, compact
 *            projection + relocated copy when large, manifest always under
 *            the 4096 B cap of the pre-existing readers
 *   phase n  quiescence window: files modified moments ago (group, root,
 *            drift sidecar) defer instead of importing; future-dated files
 *            and window 0 never defer
 *   phase o  AS10 *.crc sidecars consumed with their EDF; a leftover-only
 *            or quarantined day folder is skipped, never aborting the scan
 *            for later days (issue #338)
 *
 * Fixtures are synthetic — no patient data. */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "cJSON.h"
#include "edf_import.h"
#include "snt_format.h"
#include "sd_storage.h"

/* ── Micro test framework ──────────────────────────────────────────── */
static int g_checks, g_fail;
#define CHECK(cond, msg) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("  FAILED %s:%d %s\n", __func__, __LINE__, msg); } \
} while (0)

/* ── FS helpers ────────────────────────────────────────────────────── */
static void rmtree(const char *p)
{
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", p);
    if (system(cmd) != 0) printf("  warn: rmtree %s failed\n", p);
}
static void mkpath(const char *p)
{
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s", p);
    for (char *q = tmp + 1; *q; q++) {
        if (*q != '/') continue;
        *q = '\0'; mkdir(tmp, 0775); *q = '/';
    }
    mkdir(tmp, 0775);
}
static bool file_exists(const char *p) { struct stat s; return stat(p, &s) == 0; }

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t rd = fread(b, 1, n, f); fclose(f); b[rd] = 0;
    return b;
}

/* ── EDF fixture writer ────────────────────────────────────────────── */

typedef struct {
    const char *label;
    const char *unit;
    double      pmin, pmax;
    int         dmin, dmax;
    int         spr;
    const int16_t *samples;   /* nrec*spr values; NULL → zeros */
    const char *tal;          /* annotation-channel record bytes */
    int         tal_len;
    const char *const *tals;  /* optional: one annotation block per record */
    const int  *tal_lens;     /*           (overrides tal/tal_len)          */
} fx_sig_t;

static void fx_field(char *dst, int w, const char *s)
{
    memset(dst, ' ', (size_t)w);
    size_t n = strlen(s);
    if ((int)n > w) n = (size_t)w;
    memcpy(dst, s, n);
}

static uint16_t fx_crc16(const uint8_t *b, size_t n)
{
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)b[i] << 8;
        for (int k = 0; k < 8; k++)
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

/* Writes <sigs> signals + a trailing Crc16 signal; every record ends with
 * the 2-byte CRC16-CCITT trailer.  tail_bytes appends junk after the last
 * complete record (torn-tail fixtures). */
static void fx_write_edf(const char *path, const char *rec_id,
                         int y, int mo, int d, int h, int mi, int s,
                         const char *reserved,
                         const char *rdur_s, int decl_recs, int nrec,
                         const fx_sig_t *sigs, int nsig, int tail_bytes)
{
    int total = nsig + 1;
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  FAILED fixture fopen %s\n", path); exit(1); }

    char hdr[256];
    memset(hdr, ' ', sizeof(hdr));
    fx_field(hdr + 0, 8, "0");
    fx_field(hdr + 8, 80, "TEST SUBJECT");
    fx_field(hdr + 88, 80, rec_id);
    char t[32];
    snprintf(t, sizeof(t), "%02d.%02d.%02d", d, mo, y % 100);
    fx_field(hdr + 168, 8, t);
    snprintf(t, sizeof(t), "%02d.%02d.%02d", h, mi, s);
    fx_field(hdr + 176, 8, t);
    snprintf(t, sizeof(t), "%d", 256 + 256 * total);
    fx_field(hdr + 184, 8, t);
    fx_field(hdr + 192, 44, reserved);
    snprintf(t, sizeof(t), "%d", decl_recs);
    fx_field(hdr + 236, 8, t);
    fx_field(hdr + 244, 8, rdur_s);
    snprintf(t, sizeof(t), "%d", total);
    fx_field(hdr + 252, 4, t);
    fwrite(hdr, 1, 256, f);

    char *blk = malloc(256 * (size_t)total);
    memset(blk, ' ', 256 * (size_t)total);
    for (int i = 0; i < nsig; i++) {
        const fx_sig_t *sg = &sigs[i];
        char b[24];
        fx_field(blk + i * 16, 16, sg->label);
        fx_field(blk + 96 * total + i * 8, 8, sg->unit);
        snprintf(b, sizeof(b), "%.2f", sg->pmin); fx_field(blk + 104 * total + i * 8, 8, b);
        snprintf(b, sizeof(b), "%.2f", sg->pmax); fx_field(blk + 112 * total + i * 8, 8, b);
        snprintf(b, sizeof(b), "%d", sg->dmin);  fx_field(blk + 120 * total + i * 8, 8, b);
        snprintf(b, sizeof(b), "%d", sg->dmax);  fx_field(blk + 128 * total + i * 8, 8, b);
        snprintf(b, sizeof(b), "%d", sg->spr);   fx_field(blk + 216 * total + i * 8, 8, b);
    }
    /* Crc16 signal entry (spr=1) */
    fx_field(blk + nsig * 16, 16, "Crc16");
    fx_field(blk + 104 * total + nsig * 8, 8, "-32768.0");
    fx_field(blk + 112 * total + nsig * 8, 8, "32767.00");
    fx_field(blk + 120 * total + nsig * 8, 8, "-32768");
    fx_field(blk + 128 * total + nsig * 8, 8, "32767");
    fx_field(blk + 216 * total + nsig * 8, 8, "1");
    fwrite(blk, 1, 256 * (size_t)total, f);
    free(blk);

    long stride = 2;
    for (int i = 0; i < nsig; i++) stride += sigs[i].spr * 2;
    uint8_t *rec = malloc((size_t)stride);
    for (int r = 0; r < nrec; r++) {
        memset(rec, 0, (size_t)stride);
        long off = 0;
        for (int i = 0; i < nsig; i++) {
            const fx_sig_t *sg = &sigs[i];
            if (sg->tals)
                memcpy(rec + off, sg->tals[r],
                       (size_t)sg->tal_lens[r] < (size_t)sg->spr * 2
                           ? (size_t)sg->tal_lens[r] : (size_t)sg->spr * 2);
            else if (sg->tal)
                memcpy(rec + off, sg->tal,
                       (size_t)sg->tal_len < (size_t)sg->spr * 2
                           ? (size_t)sg->tal_len : (size_t)sg->spr * 2);
            else if (sg->samples)
                memcpy(rec + off, sg->samples + r * sg->spr,
                       (size_t)sg->spr * 2);
            off += sg->spr * 2;
        }
        uint16_t c = fx_crc16(rec, (size_t)stride - 2);
        rec[stride - 2] = (uint8_t)(c & 0xFF);
        rec[stride - 1] = (uint8_t)(c >> 8);
        fwrite(rec, 1, (size_t)stride, f);
    }
    if (tail_bytes > 0) {
        uint8_t *junk = malloc((size_t)tail_bytes);
        memset(junk, 0xAB, (size_t)tail_bytes);
        fwrite(junk, 1, (size_t)tail_bytes, f);
        free(junk);
    }
    free(rec);
    fclose(f);
}

/* Real ResMed EVE/CSL layout: EDF+D, data-record duration 0.00, one 62-byte
 * annotation record (31 samples) per annotation, each led by its own
 * time-keeping TAL "+0\x14\x14\x00".  anns[0] is "Recording starts" on real
 * cards.  Every annotation block carries its trailing NUL; lens[] are the
 * block lengths. */
static void fx_write_real_ann(const char *path, int y, int mo, int d,
                              int h, int mi, int s,
                              const char *const *anns, const int *lens, int n)
{
    static const char tk[] = "+0\x14\x14\x00";
    char bufs[16][64];
    const char *ptr[16];
    int ln[16];
    if (n > 16) n = 16;
    for (int r = 0; r < n; r++) {
        memset(bufs[r], 0, sizeof(bufs[r]));
        memcpy(bufs[r], tk, sizeof(tk) - 1);
        memcpy(bufs[r] + sizeof(tk) - 1, anns[r], (size_t)lens[r]);
        ptr[r] = bufs[r];
        ln[r] = (int)sizeof(tk) - 1 + lens[r];
    }
    const fx_sig_t sig[] = {
        { "EDF Annotations", "", -32768.0, 32767.0, -32768, 32767, 31,
          NULL, NULL, 0, ptr, ln },
    };
    fx_write_edf(path, "SRN=22199999999 MID=46 VID=1", y, mo, d, h, mi, s,
                 "EDF+D", "0.00", n, n, sig, 1, 0);
}

/* ── SNT read-back ─────────────────────────────────────────────────── */

static bool snt_read(const char *path, snt_header_t *h, int16_t **data)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fread(h, 1, sizeof(*h), f) != sizeof(*h)) { fclose(f); return false; }
    size_t n = (size_t)h->sample_count * h->n_channels;
    *data = malloc(n * 2 + 2);
    if (n && fread(*data, 2, n, f) != n) { fclose(f); free(*data); return false; }
    fclose(f);
    return true;
}

/* cJSON_Parse(NULL input) would crash — read+parse+free in one helper. */
static cJSON *parse_json_file(const char *path)
{
    char *t = read_text(path);
    cJSON *j = t ? cJSON_Parse(t) : NULL;
    free(t);
    return j;
}

static char p_buf[700];

/* ── Fixture builders ──────────────────────────────────────────────── */

/* Session fixtures.  Filenames carry the session's device-civil prefix;
 * the fixture header startdate/starttime is derived from (y,mo,d) +
 * the time args so header and filename agree like a real card.
 * noon-day label for 2024-01-15 22:10 UTC → 20240115. */
static const char *DAY = "20240115";
static const char *IMPORT_DIR = SD_IMPORT_DIR;
static char DATALOG_DAY[128];

static int16_t  brp_flow[3 * 25];   /* rdur=1s, spr=25, 3 records */
static int16_t  brp_press[3 * 25];
static int16_t  sa2_pulse[3], sa2_spo2[3];
static int16_t  pld_mask[3], pld_leak[3];

static void build_session_dir(const char *dir, const char *ts_wave,
                              const char *ts_eve, bool with_sa2,
                              int y, int mo, int d)
{
    mkpath(dir);
    /* Header clock = filename clock (ts_* are "YYYYMMDD_HHMMSS") so the
     * fixtures stay self-consistent for non-evening start times. */
    int wh = 22, wm = 10, ws = 7, eh = 22, em = 10, es = 0;
    sscanf(ts_wave + 9, "%2d%2d%2d", &wh, &wm, &ws);
    sscanf(ts_eve + 9, "%2d%2d%2d", &eh, &em, &es);
    for (int i = 0; i < 75; i++) brp_flow[i] = (int16_t)(i * 10);
    for (int i = 0; i < 75; i++) brp_press[i] = (int16_t)(100 + i);
    sa2_pulse[0] = 62; sa2_pulse[1] = -1; sa2_pulse[2] = 64;
    sa2_spo2[0] = 97;  sa2_spo2[1] = 96; sa2_spo2[2] = -1;
    for (int i = 0; i < 3; i++) { pld_mask[i] = 110 + i; pld_leak[i] = 20 + i; }

    const fx_sig_t brp_s[] = {
        { "Flow.40ms", "L/s", -2.0, 3.0, -1000, 1500, 25, brp_flow, NULL, 0 },
        { "Press.40ms", "cmH2O", 0.0, 40.0, 0, 2000, 25, brp_press, NULL, 0 },
    };
    char p[640];
    snprintf(p, sizeof(p), "%s/%s_BRP.edf", dir, ts_wave);
    fx_write_edf(p, "SRN=22199999999 MID=46 VID=1",
                 y, mo, d, wh, wm, ws, "", "1", 3, 3, brp_s, 2, 0);

    if (with_sa2) {
        const fx_sig_t sa2_s[] = {
            { "Pulse.1s", "bpm", 0.0, 300.0, 0, 300, 1, sa2_pulse, NULL, 0 },
            { "SpO2.1s", "%", 0.0, 100.0, 0, 100, 1, sa2_spo2, NULL, 0 },
        };
        snprintf(p, sizeof(p), "%s/%s_SA2.edf", dir, ts_wave);
        fx_write_edf(p, "SRN=22199999999 MID=46 VID=1",
                     y, mo, d, wh, wm, ws, "", "1", 3, 3, sa2_s, 2, 0);
    }

    const fx_sig_t pld_s[] = {
        { "MaskPress.2s", "cmH2O", 0.0, 40.0, 0, 2000, 1, pld_mask, NULL, 0 },
        { "Press.2s", "cmH2O", 0.0, 50.0, 0, 2500, 1, pld_mask, NULL, 0 },
        { "EprPress.2s", "cmH2O", 0.0, 30.0, 0, 1500, 1, pld_mask, NULL, 0 },
        { "Leak.2s", "L/s", 0.0, 2.0, 0, 100, 1, pld_leak, NULL, 0 },
        { "RespRate.2s", "bpm", 0.0, 90.0, 0, 450, 1, pld_mask, NULL, 0 },
        { "TidVol.2s", "L", 0.0, 4.0, 0, 200, 1, pld_mask, NULL, 0 },
        { "MinVent.2s", "L/min", 0.0, 30.0, 0, 240, 1, pld_mask, NULL, 0 },
        { "Snore.2s", "", 0.0, 5.0, 0, 250, 1, pld_mask, NULL, 0 },
        { "FlowLim.2s", "", 0.0, 1.0, 0, 100, 1, pld_leak, NULL, 0 },
    };
    snprintf(p, sizeof(p), "%s/%s_PLD.edf", dir, ts_wave);
    fx_write_edf(p, "SRN=22199999999 MID=46 VID=1",
                 y, mo, d, wh, wm, ws, "", "1", 3, 3, pld_s, 9, 0);

    /* EVE.edf — real ResMed structure (record duration 0.00, one record per
     * annotation): "Recording starts", +2.1s Obstructive Apnea 9.4s,
     * +5.0 Hypopnea 10s.  (Adjacent-literal splits after \x15/\x14 — a
     * digit or hex char right after the escape would extend it.) */
    static const char e0[] = "+0\x15" "0\x14" "Recording starts\x14\x00";
    static const char e1[] = "+2.1\x15" "9.4\x14" "Obstructive Apnea\x14\x00";
    static const char e2[] = "+5.0\x15" "10\x14" "Hypopnea\x14\x00";
    const char *const eve_a[] = { e0, e1, e2 };
    const int eve_l[] = { (int)sizeof(e0) - 1, (int)sizeof(e1) - 1,
                          (int)sizeof(e2) - 1 };
    snprintf(p, sizeof(p), "%s/%s_EVE.edf", dir, ts_eve);
    fx_write_real_ann(p, y, mo, d, eh, em, es, eve_a, eve_l, 3);

    snprintf(p, sizeof(p), "%s/%s_CSL.edf", dir, ts_eve);
    /* CSL.edf — one CSR window, same real structure */
    static const char c1[] = "+1.0\x14" "CSR Start\x14\x00";
    static const char c2[] = "+2.0\x14" "CSR End\x14\x00";
    const char *const csl_a[] = { e0, c1, c2 };
    const int csl_l[] = { (int)sizeof(e0) - 1, (int)sizeof(c1) - 1,
                          (int)sizeof(c2) - 1 };
    fx_write_real_ann(p, y, mo, d, eh, em, es, csl_a, csl_l, 3);
}

static void fresh_tree(void)
{
    rmtree(SD_MOUNT_POINT);
    mkpath(SD_IMPORT_DIR);
    mkpath(SD_STREAMS_DIR);
    mkpath(SD_SUMMARIES_DIR);
    mkpath(DATALOG_DAY);
}

/* Runs process_next until NOT_FOUND; returns last unit. */
static edf_import_unit_t drain(edf_import_ctx_t *c, int *units)
{
    edf_import_unit_t u, tot = {0};
    *units = 0;
    while (edf_import_process_next(c, &u) == ESP_OK) {
        tot.sessions_imported += u.sessions_imported;
        tot.sessions_skipped  += u.sessions_skipped;
        tot.sessions_failed   += u.sessions_failed;
        tot.files_consumed    += u.files_consumed;
        tot.files_rejected    += u.files_rejected;
        tot.spools_written    += u.spools_written;
        if (u.day_label[0]) memcpy(tot.day_label, u.day_label, 9);
        if (u.day2_label[0]) memcpy(tot.day2_label, u.day2_label, 9);
        (*units)++;
        if (*units > 40) break;   /* safety: no infinite inbox */
    }
    return tot;
}

/* ── phase a: full session, AS11 layout ────────────────────────────── */
static void phase_a(void)
{
    fresh_tree();
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%s", DATALOG_DAY, DAY);
    build_session_dir(dir, "20240115_221007", "20240115_221000", true,
                      2024, 1, 15);

    edf_import_ctx_t *c = edf_import_ctx_create();
    CHECK(c != NULL, "ctx create");
    CHECK(edf_import_pending(), "inbox should be pending");

    int units;
    edf_import_unit_t t = drain(c, &units);
    CHECK(units == 1, "one unit expected");
    CHECK(t.sessions_imported == 1, "1 session imported");
    CHECK(t.sessions_failed == 0, "no failures");
    CHECK(t.files_consumed == 5, "5 sources consumed");
    CHECK(!edf_import_pending(), "inbox drained");

    /* Session id = eve start (2024-01-15 22:10:00 UTC) → 1705356600000 */
    const char *id = "20240115_221000";
    char p[700];

    /* manifest */
    snprintf(p, sizeof(p), "%s/%s/%s_session.json", SD_STREAMS_DIR, DAY, id);
    cJSON *m = parse_json_file(p);
    CHECK(m != NULL, "manifest parses");
    if (m) {
        cJSON *v;
        CHECK((v = cJSON_GetObjectItem(m, "start_epoch_ms")) &&
              (int64_t)v->valuedouble == 1705356600000LL, "start_epoch_ms");
        /* wav files span 22:10:07 + 3 s → session ends 22:10:10 */
        CHECK((v = cJSON_GetObjectItem(m, "end_epoch_ms")) &&
              (int64_t)v->valuedouble == 1705356600000LL + 10000,
              "end_epoch_ms");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(m, "pre_gated")), "pre_gated");
        CHECK((v = cJSON_GetObjectItem(m, "waveform_offset_ms")) &&
              (int64_t)v->valuedouble == 7000, "waveform_offset_ms 7000");
        CHECK((v = cJSON_GetObjectItem(m, "clock_drift_ms")) &&
              (int64_t)v->valuedouble == 0, "drift 0");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(m, "clock_drift_valid")),
              "drift not valid");
        v = cJSON_GetObjectItem(m, "clock_drift_source");
        CHECK(v && !strcmp(v->valuestring, "file_import"), "drift source");
        v = cJSON_GetObjectItem(m, "import_source");
        CHECK(v && !strcmp(v->valuestring, "file_import"), "import source");
        CHECK((v = cJSON_GetObjectItem(m, "brp_samples")) &&
              (int)v->valuedouble == 75, "brp_samples=75");
        /* start_iso / end_iso are LOCAL civil strings (localtime_r) —
         * "YYYY-MM-DDTHH:MM:SS" (19 chars, no Z, no millis).  The old
         * imp_dev_to_iso format was "...SS.mmmZ" — catch a regression
         * by shape and by exact value against localtime_r. */
        v = cJSON_GetObjectItem(m, "start_iso");
        CHECK(v && cJSON_IsString(v) && strlen(v->valuestring) == 19 &&
              v->valuestring[10] == 'T' && v->valuestring[16] == ':',
              "start_iso is local civil (no Z, no millis)");
        if (v && cJSON_IsString(v)) {
            time_t tt = (time_t)(1705356600000LL / 1000);
            struct tm lt;
            char want[32];
            localtime_r(&tt, &lt);
            strftime(want, sizeof(want), "%Y-%m-%dT%H:%M:%S", &lt);
            CHECK(!strcmp(v->valuestring, want), "start_iso localtime value");
        }
        v = cJSON_GetObjectItem(m, "end_iso");
        if (v && cJSON_IsString(v)) {
            time_t tt = (time_t)((1705356600000LL + 10000) / 1000);
            struct tm lt;
            char want[32];
            localtime_r(&tt, &lt);
            strftime(want, sizeof(want), "%Y-%m-%dT%H:%M:%S", &lt);
            CHECK(!strcmp(v->valuestring, want), "end_iso localtime value");
        } else {
            CHECK(false, "end_iso present");
        }
        cJSON_Delete(m);
    }

    /* flow.snt — 1ch 25Hz, 75 samples, phys×100 from L/s.
     * dig 0 → phys 0.00 → 0; dig 10 → 0.02 → 2. */
    snt_header_t h; int16_t *d;
    snprintf(p, sizeof(p), "%s/%s/%s_flow.snt", SD_STREAMS_DIR, DAY, id);
    CHECK(snt_read(p, &h, &d), "flow.snt readable");
    if (h.magic == SNT_MAGIC) {
        CHECK(h.version == 2 && h.n_channels == 1 && h.sample_hz_x10 == 250,
              "flow header");
        CHECK(h.sample_count == 75, "flow count");
        CHECK(d[0] == 0 && d[1] == 2, "flow values");
        CHECK(h.start_epoch_ms == 1705356600000LL + 7000, "flow start=wav");
    }
    free(d);

    /* press.snt — phys 0-40, dig 0-2000: dig=100 → 2.0 → 200 */
    snprintf(p, sizeof(p), "%s/%s/%s_press.snt", SD_STREAMS_DIR, DAY, id);
    CHECK(snt_read(p, &h, &d), "press.snt readable");
    CHECK(h.sample_count == 75 && d[0] == 200 && d[1] == 202, "press values");
    free(d);

    /* flow_mm.snt — 2ch 1Hz minmax over each 25-sample second.
     * second 0: dig 0..240 → phys 0.00..0.48 → mm [0, 48] */
    snprintf(p, sizeof(p), "%s/%s/%s_flow_mm.snt", SD_STREAMS_DIR, DAY, id);
    CHECK(snt_read(p, &h, &d), "flow_mm.snt readable");
    CHECK(h.n_channels == 2 && h.sample_hz_x10 == 10 && h.sample_count == 3,
          "flow_mm header");
    CHECK(d[0] == 0 && d[1] == 48, "flow_mm sec0");
    free(d);

    /* sa2.snt — stored = phys×100; dig -1 → INT16_MIN */
    snprintf(p, sizeof(p), "%s/%s/%s_sa2.snt", SD_STREAMS_DIR, DAY, id);
    CHECK(snt_read(p, &h, &d), "sa2.snt readable");
    CHECK(h.n_channels == 2 && h.sample_count == 3, "sa2 header");
    CHECK(d[0] == 6200 && d[1] == 9700, "sa2 frame0");
    CHECK(d[2] == INT16_MIN && d[5] == INT16_MIN, "sa2 missing → sentinel");
    free(d);

    /* pld.snt — 12ch, MaskPress dig 110 → phys 2.20 → 220;
     * Leak dig 20 → 0.4 → 40; FlowLim (ch10) dig 20 → 0.2 → 20;
     * channels 7,8,11 absent from a 9-signal AS11 PLD → 0 */
    snprintf(p, sizeof(p), "%s/%s/%s_pld.snt", SD_STREAMS_DIR, DAY, id);
    CHECK(snt_read(p, &h, &d), "pld.snt readable");
    CHECK(h.n_channels == 12 && h.sample_hz_x10 == 5 && h.sample_count == 3,
          "pld header");
    CHECK(d[0] == 220 && d[3] == 40 && d[10] == 20, "pld mapped channels");
    CHECK(d[7] == 0 && d[8] == 0 && d[11] == 0, "pld absent channels zero");
    free(d);

    /* events.snt — synthetic therapy + mask window + 2 respiratory + 2 CSR */
    snprintf(p, sizeof(p), "%s/%s/%s_events.snt", SD_STREAMS_DIR, DAY, id);
    char *ev = read_text(p);
    CHECK(ev != NULL, "events.snt readable");
    if (ev) {
        CHECK(strstr(ev, "\"TherapyStart\""), "TherapyStart present");
        CHECK(strstr(ev, "\"MaskOn\"") && strstr(ev, "\"MaskOff\""),
              "mask window");
        CHECK(strstr(ev, "\"ObstructiveApneaEnd\""), "OSA mapped");
        CHECK(strstr(ev, "\"HypopneaEnd\""), "Hypopnea mapped");
        CHECK(strstr(ev, "\"CsrStart\"") && strstr(ev, "\"CsrEnd\""), "CSR");
        /* onset 22:10:00+2.1s → reportTime 22:10:02.100 */
        CHECK(strstr(ev, "2024-01-15T22:10:02.100Z"), "onset timestamp");
        free(ev);
    }

    /* all 5 sources deleted */
    snprintf(p, sizeof(p), "%s/%s/20240115_221007_BRP.edf", DATALOG_DAY, DAY);
    CHECK(!file_exists(p), "BRP source deleted");
    edf_import_ctx_destroy(c);
    printf("phase a OK (%d checks)\n", g_checks);
}

/* ── phase b: AS10 path — day dir, .tgt ident, no SA2 ──────────────── */
static void phase_b(void)
{
    fresh_tree();
    /* AS10-style: IMPORT/<day>/ with BRP+PLD+SAD(no oximeter sigs? — no,
     * simply absent SA2/SAD entirely) */
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%s", IMPORT_DIR, DAY);
    mkpath(dir);

    /* Identification.tgt — AS10 hex-coded fields */
    char tp[700];
    snprintf(tp, sizeof(tp), "%s/Identification.tgt", IMPORT_DIR);
    FILE *f = fopen(tp, "w");
    fputs("#SRN 22161122334\n#PNA AirSense_10_AutoSet\n#PCD 37028\n"
          "#MID 0018\n#VID 0001\n#CID 000F\n#SID SX474-0601\n#RGN E\n", f);
    fclose(f);

    build_session_dir(dir, "20240115_221007", "20240115_221000", false,
                      2024, 1, 15);

    edf_import_ctx_t *c = edf_import_ctx_create();
    int units;
    edf_import_unit_t t = drain(c, &units);
    CHECK(units == 2, "root pass + day unit");
    CHECK(t.sessions_imported == 1, "1 session");
    /* BRP+PLD+EVE+CSL consumed in the day unit; the .tgt went in the
     * root pass which does not count toward files_consumed. */
    CHECK(t.files_consumed == 4, "4 sources consumed");

    const char *id = "20240115_221000";
    char p[700];

    /* AS10 ident → flat _ident.json, family recorded in manifest */
    snprintf(p, sizeof(p), "%s/%s/%s_ident.json", SD_STREAMS_DIR, DAY, id);
    cJSON *ij = parse_json_file(p);
    CHECK(ij != NULL, "ident.json written");
    if (ij) {
        cJSON *s = cJSON_GetObjectItem(ij, "SerialNumber");
        CHECK(s && !strcmp(s->valuestring, "22161122334"), "SRN from .tgt");
        cJSON *m = cJSON_GetObjectItem(ij, "PlatformIdentifier");
        CHECK(m && (int)m->valuedouble == 0x18, "MID hex 0x18 → 24");
        s = cJSON_GetObjectItem(ij, "ProductName");
        CHECK(s && strstr(s->valuestring, "AirSense 10"), "PNA underscores");
        cJSON_Delete(ij);
    }
    snprintf(p, sizeof(p), "%s/%s/%s_session.json", SD_STREAMS_DIR, DAY, id);
    cJSON *mj = parse_json_file(p);
    if (mj) {
        cJSON *v = cJSON_GetObjectItem(mj, "source_device_family");
        CHECK(v && !strcmp(v->valuestring, "AS10"), "family AS10");
        cJSON_Delete(mj);
    }

    /* missing SA2 → all-missing 1 Hz stream covering the 3 s session */
    snt_header_t h; int16_t *d;
    snprintf(p, sizeof(p), "%s/%s/%s_sa2.snt", SD_STREAMS_DIR, DAY, id);
    CHECK(snt_read(p, &h, &d), "sa2.snt exists despite missing SA2.edf");
    CHECK(h.sample_count == 3 && d[0] == INT16_MIN && d[1] == INT16_MIN &&
          d[3] == INT16_MIN, "sa2 all-missing fill");
    free(d);

    /* .tgt consumed */
    CHECK(!file_exists(tp), "Identification.tgt deleted");
    edf_import_ctx_destroy(c);
    printf("phase b OK (%d checks)\n", g_checks);
}

/* ── phase c: torn tail — clamp + CRC gating ───────────────────────── */
static void phase_c(void)
{
    fresh_tree();
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%s", DATALOG_DAY, DAY);
    mkpath(dir);

    int16_t flow[3 * 25];
    for (int i = 0; i < 75; i++) flow[i] = (int16_t)(i);
    const fx_sig_t s[] = {
        { "Flow.40ms", "L/s", -2.0, 3.0, -1000, 1500, 25, flow, NULL, 0 },
    };
    char p[700];
    snprintf(p, sizeof(p), "%s/20240115_221000_BRP.edf", dir);
    /* header declares 99 records; 3 valid records + 733 junk bytes on disk.
     * stride = 25*2+2 = 52 → disk holds 17 record slots; records 3..16
     * fail CRC and must import as SNT_MISSING, not garbage. */
    fx_write_edf(p, "SRN=22199999999 MID=46 VID=1",
                 2024, 1, 15, 22, 10, 0, "", "1", 99, 3, s, 1, 733);

    edf_import_ctx_t *c = edf_import_ctx_create();
    int units;
    edf_import_unit_t t = drain(c, &units);
    CHECK(t.sessions_imported == 1, "torn session still imports");
    CHECK(t.sessions_failed == 0, "torn tail is not a failure");

    snprintf(p, sizeof(p), "%s/%s/20240115_221000_flow.snt",
             SD_STREAMS_DIR, DAY);
    snt_header_t h; int16_t *d;
    CHECK(snt_read(p, &h, &d), "flow.snt readable");
    CHECK(h.sample_count == 17 * 25, "17 record slots kept positional");
    /* dig 74 → phys = -2 + (74+1000)·5/2500 = 0.148 → stored 15 */
    CHECK(d[74] == 15, "last good sample");
    CHECK(d[75] == INT16_MIN && d[424] == INT16_MIN, "CRC junk → missing");
    free(d);
    edf_import_ctx_destroy(c);
    printf("phase c OK (%d checks)\n", g_checks);
}

/* ── phase d: TAL annotation fidelity ──────────────────────────────── */
static void phase_d(void)
{
    fresh_tree();
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%s", DATALOG_DAY, DAY);
    mkpath(dir);

    /* Events covering every mapped label + one unknown + "Recording starts" */
    static const char tal[] =
        "+0\x14\x14\x00"
        "+0.5\x15\x38\x14" "Obstructive Apnea\x14\x00"
        "+3.0\x15\x39\x14" "Central Apnea\x14\x00"
        "+6.0\x15\x31\x32\x14" "Hypopnea\x14\x00"
        "+8.0\x15\x35\x14" "Arousal\x14\x00"
        "+9.0\x15\x36\x14" "RERA\x14\x00"
        "+11.0\x15\x37\x14" "Unclassified Apnea\x14\x00"
        "+13.0\x14" "Recording starts\x14\x00"
        "+14.0\x14" "RubbishLabel\x14\x00";
    const fx_sig_t eve_s[] = {
        { "EDF Annotations", "", -32768.0, 32767.0, -32768, 32767, 200,
          NULL, tal, (int)sizeof(tal) - 1 },
    };
    char p[700];
    snprintf(p, sizeof(p), "%s/20240115_221000_EVE.edf", dir);
    fx_write_edf(p, "SRN=22199999999 MID=46 VID=1",
                 2024, 1, 15, 22, 10, 0, "EDF+D", "1", 1, 1, eve_s, 1, 0);

    edf_import_ctx_t *c = edf_import_ctx_create();
    int units;
    edf_import_unit_t t = drain(c, &units);
    CHECK(t.sessions_imported == 1, "eve-only session imports");

    snprintf(p, sizeof(p), "%s/%s/20240115_221000_events.snt",
             SD_STREAMS_DIR, DAY);
    char *ev = read_text(p);
    CHECK(ev != NULL, "events readable");
    if (ev) {
        CHECK(strstr(ev, "\"ObstructiveApneaEnd\""), "OSA");
        CHECK(strstr(ev, "\"CentralApneaEnd\""), "CA");
        CHECK(strstr(ev, "\"HypopneaEnd\""), "H");
        CHECK(strstr(ev, "\"ReraEnd\""), "Arousal→Rera");
        CHECK(strstr(ev, "\"ApneaEnd\""), "unclassified→Apnea");
        CHECK(!strstr(ev, "Recording starts"), "Recording starts skipped");
        CHECK(!strstr(ev, "RubbishLabel"), "unknown label skipped");
        /* RERA+Arousal both map to ReraEnd — count occurrences */
        int n = 0;
        for (char *q = ev; (q = strstr(q, "\"ReraEnd\"")); q++) n++;
        CHECK(n == 2, "two ReraEnd events");
        free(ev);
    }
    edf_import_ctx_destroy(c);
    printf("phase d OK (%d checks)\n", g_checks);
}

/* ── phase e: SomnoBridge drift sidecar ────────────────────────────── */
static void phase_e(void)
{
    fresh_tree();
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%s", DATALOG_DAY, DAY);
    build_session_dir(dir, "20240115_221007", "20240115_221000", true,
                      2024, 1, 15);

    /* drift = +214000 ms measured by the bridge */
    char dp[700];
    snprintf(dp, sizeof(dp), "%s/_bridge_drift.json", dir);
    FILE *f = fopen(dp, "w");
    fputs("{\"source_of_truth\":{\"clock_drift_ms\":\"214000\",\"half_ms\":300,"
          "\"quality\":\"qualified\",\"basis\":\"anchor\",\"generation\":4},"
          "\"estimate\":{\"has_drift\":true,\"clock_drift_ms\":\"214200\","
          "\"half_ms\":900}}", f);
    fclose(f);

    edf_import_ctx_t *c = edf_import_ctx_create();
    int units;
    edf_import_unit_t t = drain(c, &units);
    CHECK(t.sessions_imported == 1, "session imported");

    /* Session id is in the drifted (NTP) domain: 22:10:00 + 214 s → 221334 */
    char p[700];
    snprintf(p, sizeof(p), "%s/%s/20240115_221334_session.json",
             SD_STREAMS_DIR, DAY);
    cJSON *m = parse_json_file(p);
    CHECK(m != NULL, "manifest parses");
    if (m) {
        cJSON *v;
        CHECK((v = cJSON_GetObjectItem(m, "clock_drift_ms")) &&
              (int64_t)v->valuedouble == 214000, "drift = 214000");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(m, "clock_drift_valid")),
              "sot → valid");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(m, "clock_drift_usable")),
              "sot → usable");
        v = cJSON_GetObjectItem(m, "clock_drift_source");
        CHECK(v && !strncmp(v->valuestring, "somnobridge_", 12),
              "somnobridge source");
        v = cJSON_GetObjectItem(m, "import_source");
        CHECK(v && !strcmp(v->valuestring, "somnobridge"), "import source");
        v = cJSON_GetObjectItem(m, "somnobridge_drift");
        CHECK(v && cJSON_IsObject(v), "sidecar embedded");
        /* start_epoch_ms = eve_dev + drift → 1705356600000+214000 */
        v = cJSON_GetObjectItem(m, "start_epoch_ms");
        CHECK(v && (int64_t)v->valuedouble == 1705356600000LL + 214000,
              "drifted start");
        cJSON_Delete(m);
    }
    CHECK(!file_exists(dp), "sidecar consumed");
    edf_import_ctx_destroy(c);

    /* estimate-only sidecar → valid=false, usable iff half_ms ≤ 5000 */
    fresh_tree();
    build_session_dir(dir, "20240115_221007", "20240115_221000", false,
                      2024, 1, 15);
    f = fopen(dp, "w");
    fputs("{\"estimate\":{\"has_drift\":true,\"clock_drift_ms\":\"3000\","
          "\"half_ms\":800}}", f);
    fclose(f);
    c = edf_import_ctx_create();
    drain(c, &units);
    snprintf(p, sizeof(p), "%s/%s/20240115_221003_session.json",
             SD_STREAMS_DIR, DAY);
    cJSON *m2 = parse_json_file(p);
    if (m2) {
        cJSON *v = cJSON_GetObjectItem(m2, "clock_drift_ms");
        CHECK(v && (int64_t)v->valuedouble == 3000, "estimate drift used");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(m2, "clock_drift_valid")),
              "estimate → not valid");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(m2, "clock_drift_usable")),
              "estimate ±800ms → usable");
        cJSON_Delete(m2);
    }
    edf_import_ctx_destroy(c);

    /* estimate with half_ms > 5000 → NOT usable: drift stays 0, session id
     * and epochs are unshifted, but the sidecar is still preserved. */
    fresh_tree();
    build_session_dir(dir, "20240115_221007", "20240115_221000", false,
                      2024, 1, 15);
    f = fopen(dp, "w");
    fputs("{\"estimate\":{\"has_drift\":true,\"clock_drift_ms\":\"3000\","
          "\"half_ms\":15000}}", f);
    fclose(f);
    c = edf_import_ctx_create();
    drain(c, &units);
    snprintf(p, sizeof(p), "%s/%s/20240115_221000_session.json",
             SD_STREAMS_DIR, DAY);
    cJSON *m3 = parse_json_file(p);
    CHECK(m3 != NULL, "unusable-estimate manifest parses");
    if (m3) {
        cJSON *v = cJSON_GetObjectItem(m3, "clock_drift_ms");
        CHECK(v && (int64_t)v->valuedouble == 0,
              "unusable estimate → drift 0");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(m3, "clock_drift_valid")),
              "unusable estimate → not valid");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(m3, "clock_drift_usable")),
              "estimate ±15s → not usable");
        v = cJSON_GetObjectItem(m3, "start_epoch_ms");
        CHECK(v && (int64_t)v->valuedouble == 1705356600000LL,
              "unusable estimate → unshifted start");
        v = cJSON_GetObjectItem(m3, "somnobridge_drift");
        CHECK(v && cJSON_IsObject(v), "unusable sidecar still embedded");
        cJSON_Delete(m3);
    }
    edf_import_ctx_destroy(c);

    /* ── schema-5 envelope shapes (SomnoBridge update6) ── */

    /* sot as interval_ms (no scalar clock_drift_ms) → midpoint used */
    fresh_tree();
    build_session_dir(dir, "20240115_221007", "20240115_221000", false,
                      2024, 1, 15);
    f = fopen(dp, "w");
    fputs("{\"source_of_truth\":{\"selected_view\":\"current_session_estimate\","
          "\"quality\":\"qualified\",\"interval_ms\":[213500,214500],"
          "\"accuracy_half_ms\":500,\"reference_at_ms\":1705357000000}}", f);
    fclose(f);
    c = edf_import_ctx_create();
    drain(c, &units);
    snprintf(p, sizeof(p), "%s/%s/20240115_221334_session.json",
             SD_STREAMS_DIR, DAY);
    cJSON *m4 = parse_json_file(p);
    CHECK(m4 != NULL, "schema-5 sot manifest parses");
    if (m4) {
        cJSON *v = cJSON_GetObjectItem(m4, "clock_drift_ms");
        CHECK(v && (int64_t)v->valuedouble == 214000,
              "interval midpoint drift");
        v = cJSON_GetObjectItem(m4, "clock_drift_half_ms");
        CHECK(v && (int64_t)v->valuedouble == 500, "sot half recorded");
        v = cJSON_GetObjectItem(m4, "clock_drift_measured_at_ms");
        CHECK(v && (int64_t)v->valuedouble == 1705357000000LL,
              "sot reference_at");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(m4, "clock_drift_valid")),
              "sot valid");
        v = cJSON_GetObjectItem(m4, "clock_drift_source");
        CHECK(v && !strcmp(v->valuestring, "somnobridge_qualified"),
              "sot source = qualified");
        cJSON_Delete(m4);
    }
    edf_import_ctx_destroy(c);

    /* best_available_offset previous-session fallback → -480000 ms */
    fresh_tree();
    build_session_dir(dir, "20240115_221007", "20240115_221000", false,
                      2024, 1, 15);
    f = fopen(dp, "w");
    fputs("{\"best_available_offset\":{\"selected_view\":"
          "\"previous_session_fallback\",\"quality\":\"provisional\","
          "\"interval_ms\":[-480500,-479500],\"accuracy_half_ms\":500,"
          "\"reference_at_ms\":1705356400000}}", f);
    fclose(f);
    c = edf_import_ctx_create();
    drain(c, &units);
    snprintf(p, sizeof(p), "%s/%s/20240115_220200_session.json",
             SD_STREAMS_DIR, DAY);
    cJSON *m5 = parse_json_file(p);
    CHECK(m5 != NULL, "best-available manifest parses");
    if (m5) {
        cJSON *v = cJSON_GetObjectItem(m5, "clock_drift_ms");
        CHECK(v && (int64_t)v->valuedouble == -480000,
              "best_available drift -480s");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(m5, "clock_drift_valid")),
              "best_available valid");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(m5, "clock_drift_usable")),
              "best_available usable");
        v = cJSON_GetObjectItem(m5, "clock_drift_source");
        CHECK(v && !strcmp(v->valuestring, "somnobridge_fallback"),
              "fallback view → fallback source");
        cJSON_Delete(m5);
    }
    edf_import_ctx_destroy(c);

    /* completed_session_fallback eligible record → -481000 ms */
    fresh_tree();
    build_session_dir(dir, "20240115_221007", "20240115_221000", false,
                      2024, 1, 15);
    f = fopen(dp, "w");
    fputs("{\"completed_session_fallback\":{\"fallback_eligible\":true,"
          "\"adjusted_half_ms\":2300,\"record\":{\"clock_drift_ms\":"
          "\"-481000\",\"accuracy_half_ms\":1300,"
          "\"reference_at_ms\":1705356000000}}}", f);
    fclose(f);
    c = edf_import_ctx_create();
    drain(c, &units);
    snprintf(p, sizeof(p), "%s/%s/20240115_220159_session.json",
             SD_STREAMS_DIR, DAY);
    cJSON *m6 = parse_json_file(p);
    CHECK(m6 != NULL, "completed-fallback manifest parses");
    if (m6) {
        cJSON *v = cJSON_GetObjectItem(m6, "clock_drift_ms");
        CHECK(v && (int64_t)v->valuedouble == -481000,
              "completed fallback drift");
        v = cJSON_GetObjectItem(m6, "clock_drift_half_ms");
        CHECK(v && (int64_t)v->valuedouble == 2300,
              "adjusted half used");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(m6, "clock_drift_valid")),
              "completed fallback valid");
        cJSON_Delete(m6);
    }
    edf_import_ctx_destroy(c);

    /* estimate-only + confirmed device zone UTC-10 under a UTC host:
     * applied shift = -480000 + (0 - 36000)*1000 = -36480000 */
    fresh_tree();
    build_session_dir(dir, "20240115_221007", "20240115_221000", false,
                      2024, 1, 15);
    f = fopen(dp, "w");
    fputs("{\"estimate\":{\"estimate_state\":\"provisional\","
          "\"clock_drift_ms\":\"-480000\",\"accuracy_half_ms\":1191,"
          "\"timezone_rule\":\"UTC-10\",\"timezone_confirmed\":true}}", f);
    fclose(f);
    c = edf_import_ctx_create();
    drain(c, &units);
    snprintf(p, sizeof(p), "%s/%s/20240115_120200_session.json",
             SD_STREAMS_DIR, DAY);
    cJSON *m7 = parse_json_file(p);
    CHECK(m7 != NULL, "device-zone manifest parses");
    if (m7) {
        cJSON *v = cJSON_GetObjectItem(m7, "clock_drift_ms");
        CHECK(v && (int64_t)v->valuedouble == -36480000,
              "drift+zone applied");
        v = cJSON_GetObjectItem(m7, "device_tz_offset_s");
        CHECK(v && (int64_t)v->valuedouble == 36000,
              "device offset +10h");
        v = cJSON_GetObjectItem(m7, "device_tz_delta_ms");
        CHECK(v && (int64_t)v->valuedouble == -36000000,
              "zone delta -10h");
        v = cJSON_GetObjectItem(m7, "device_tz_rule");
        CHECK(v && !strcmp(v->valuestring, "UTC-10"),
              "device rule recorded");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(m7, "clock_drift_valid")),
              "estimate not valid");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(m7, "clock_drift_usable")),
              "estimate usable");
        cJSON_Delete(m7);
    }
    edf_import_ctx_destroy(c);

    /* unconfirmed device zone → zone delta NOT applied */
    fresh_tree();
    build_session_dir(dir, "20240115_221007", "20240115_221000", false,
                      2024, 1, 15);
    f = fopen(dp, "w");
    fputs("{\"estimate\":{\"estimate_state\":\"provisional\","
          "\"clock_drift_ms\":\"-480000\",\"accuracy_half_ms\":1191,"
          "\"timezone_rule\":\"UTC-10\",\"timezone_confirmed\":false}}", f);
    fclose(f);
    c = edf_import_ctx_create();
    drain(c, &units);
    snprintf(p, sizeof(p), "%s/%s/20240115_220200_session.json",
             SD_STREAMS_DIR, DAY);
    cJSON *m8 = parse_json_file(p);
    CHECK(m8 != NULL, "unconfirmed-zone manifest parses");
    if (m8) {
        cJSON *v = cJSON_GetObjectItem(m8, "clock_drift_ms");
        CHECK(v && (int64_t)v->valuedouble == -480000,
              "unconfirmed zone → drift only");
        CHECK(cJSON_GetObjectItem(m8, "device_tz_offset_s") == NULL,
              "no device offset field");
        CHECK(cJSON_GetObjectItem(m8, "device_tz_delta_ms") == NULL,
              "no zone delta field");
        cJSON_Delete(m8);
    }
    edf_import_ctx_destroy(c);

    /* unusable estimate (half 15000) but confirmed zone → zone delta
     * still applied, drift stays 0 */
    fresh_tree();
    build_session_dir(dir, "20240115_221007", "20240115_221000", false,
                      2024, 1, 15);
    f = fopen(dp, "w");
    fputs("{\"estimate\":{\"estimate_state\":\"provisional\","
          "\"clock_drift_ms\":\"-480000\",\"accuracy_half_ms\":15000,"
          "\"timezone_rule\":\"UTC-10\",\"timezone_confirmed\":true}}", f);
    fclose(f);
    c = edf_import_ctx_create();
    drain(c, &units);
    snprintf(p, sizeof(p), "%s/%s/20240115_121000_session.json",
             SD_STREAMS_DIR, DAY);
    cJSON *m9 = parse_json_file(p);
    CHECK(m9 != NULL, "zone-only manifest parses");
    if (m9) {
        cJSON *v = cJSON_GetObjectItem(m9, "clock_drift_ms");
        CHECK(v && (int64_t)v->valuedouble == -36000000,
              "zone-only shift");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(m9, "clock_drift_usable")),
              "estimate not usable");
        cJSON_Delete(m9);
    }
    edf_import_ctx_destroy(c);

    /* poisoned drift magnitude (>24 h) → ignored entirely (RC1 bug) */
    fresh_tree();
    build_session_dir(dir, "20240115_221007", "20240115_221000", false,
                      2024, 1, 15);
    f = fopen(dp, "w");
    fputs("{\"estimate\":{\"estimate_state\":\"provisional\","
          "\"clock_drift_ms\":\"-1793194135596\",\"accuracy_half_ms\":1191}}",
          f);
    fclose(f);
    c = edf_import_ctx_create();
    drain(c, &units);
    snprintf(p, sizeof(p), "%s/%s/20240115_221000_session.json",
             SD_STREAMS_DIR, DAY);
    cJSON *m10 = parse_json_file(p);
    CHECK(m10 != NULL, "garbage-drift manifest parses");
    if (m10) {
        cJSON *v = cJSON_GetObjectItem(m10, "clock_drift_ms");
        CHECK(v && (int64_t)v->valuedouble == 0,
              "garbage drift → 0");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(m10, "clock_drift_valid")),
              "garbage → not valid");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(m10, "clock_drift_usable")),
              "garbage → not usable");
        v = cJSON_GetObjectItem(m10, "clock_drift_source");
        CHECK(v && !strcmp(v->valuestring, "somnobridge_none"),
              "garbage → none source");
        v = cJSON_GetObjectItem(m10, "start_epoch_ms");
        CHECK(v && (int64_t)v->valuedouble == 1705356600000LL,
              "garbage → unshifted start");
        cJSON_Delete(m10);
    }
    edf_import_ctx_destroy(c);

    /* garbage sot must not shadow a valid lower tier (fallthrough) */
    fresh_tree();
    build_session_dir(dir, "20240115_221007", "20240115_221000", false,
                      2024, 1, 15);
    f = fopen(dp, "w");
    fputs("{\"source_of_truth\":{\"quality\":\"qualified\",\"interval_ms\":"
          "[-1793194135596,-1793194135000],\"accuracy_half_ms\":500},"
          "\"completed_session_fallback\":{\"fallback_eligible\":true,"
          "\"adjusted_half_ms\":1300,\"record\":{\"clock_drift_ms\":"
          "\"-480500\",\"accuracy_half_ms\":1300,"
          "\"reference_at_ms\":1705356000000}}}", f);
    fclose(f);
    c = edf_import_ctx_create();
    drain(c, &units);
    snprintf(p, sizeof(p), "%s/%s/20240115_220159_session.json",
             SD_STREAMS_DIR, DAY);
    cJSON *m11 = parse_json_file(p);
    CHECK(m11 != NULL, "fallthrough manifest parses");
    if (m11) {
        cJSON *v = cJSON_GetObjectItem(m11, "clock_drift_ms");
        CHECK(v && (int64_t)v->valuedouble == -480500,
              "garbage sot → fallback tier");
        v = cJSON_GetObjectItem(m11, "clock_drift_source");
        CHECK(v && !strcmp(v->valuestring, "somnobridge_fallback"),
              "fallback source");
        cJSON_Delete(m11);
    }
    edf_import_ctx_destroy(c);
    printf("phase e OK (%d checks)\n", g_checks);
}

/* ── phase f: duplicates + id collision ────────────────────────────── */
static void phase_f(void)
{
    fresh_tree();
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%s", DATALOG_DAY, DAY);
    build_session_dir(dir, "20240115_221007", "20240115_221000", true,
                      2024, 1, 15);

    edf_import_ctx_t *c = edf_import_ctx_create();
    int units;
    drain(c, &units);
    edf_import_ctx_destroy(c);

    /* Re-drop identical files → duplicate detection consumes them */
    build_session_dir(dir, "20240115_221007", "20240115_221000", true,
                      2024, 1, 15);
    c = edf_import_ctx_create();
    edf_import_unit_t t = drain(c, &units);
    CHECK(t.sessions_imported == 0, "no second import");
    CHECK(t.sessions_skipped == 1, "duplicate skipped");
    CHECK(t.files_consumed == 5, "duplicate sources consumed");
    edf_import_ctx_destroy(c);

    /* Same anchor, slightly different content → still the same session */
    build_session_dir(dir, "20240115_221007", "20240115_221000", false,
                      2024, 1, 15);
    c = edf_import_ctx_create();
    t = drain(c, &units);
    CHECK(t.sessions_skipped == 1, "same-start = duplicate");
    edf_import_ctx_destroy(c);

    /* Foreign session already occupying the id (a *different* session
     * whose start is >90 s away) → suffix _2 */
    fresh_tree();
    mkpath(DATALOG_DAY);
    char mpath[700];
    snprintf(mpath, sizeof(mpath), "%s/%s", SD_STREAMS_DIR, DAY);
    mkpath(mpath);
    snprintf(mpath, sizeof(mpath), "%s/%s/20240115_221000_session.json",
             SD_STREAMS_DIR, DAY);
    FILE *f = fopen(mpath, "w");
    /* existing session at 22:06:40 — 200 s away → not a dup */
    fputs("{\"id\":\"20240115_221000\",\"start_epoch_ms\":1705356400000,"
          "\"end_epoch_ms\":1705356410000}", f);
    fclose(f);

    build_session_dir(dir, "20240115_221007", "20240115_221000", true,
                      2024, 1, 15);
    c = edf_import_ctx_create();
    t = drain(c, &units);
    CHECK(t.sessions_imported == 1, "collision import proceeds");
    snprintf(p_buf, sizeof(p_buf), "%s/%s/20240115_221000_2_session.json",
             SD_STREAMS_DIR, DAY);
    CHECK(file_exists(p_buf), "suffixed _2 manifest written");
    edf_import_ctx_destroy(c);
    printf("phase f OK (%d checks)\n", g_checks);
}

/* Minimal protobuf walker: returns the varint value of field `want`
 * (wire 0), or -1 when absent.  Good enough to audit our own spool. */
static uint64_t pb_varint(const unsigned char *b, size_t n, size_t *i)
{
    uint64_t v = 0;
    int sh = 0;
    while (*i < n) {
        uint8_t c = b[(*i)++];
        v |= (uint64_t)(c & 0x7f) << sh;
        if (!(c & 0x80)) break;
        sh += 7;
    }
    return v;
}

static int64_t pb_find_varint(const unsigned char *b, size_t n, int want)
{
    size_t i = 0;
    while (i < n) {
        uint64_t tag = pb_varint(b, n, &i);
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        if (wire == 0) {
            uint64_t v = pb_varint(b, n, &i);
            if (field == want) return (int64_t)v;
        } else if (wire == 2) {
            i += (size_t)pb_varint(b, n, &i);
        } else if (wire == 5) {
            i += 4;
        } else if (wire == 1) {
            i += 8;
        } else {
            break;
        }
    }
    return -1;
}

/* Summary metrics are length-delimited submessages (outer field = metric
 * id, inner varints = percentile slots 1..5). */
static int64_t pb_find_nested_varint(const unsigned char *b, size_t n,
                                     int outer, int inner)
{
    size_t i = 0;
    while (i < n) {
        uint64_t tag = pb_varint(b, n, &i);
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        if (wire == 2) {
            uint64_t len = pb_varint(b, n, &i);
            if (field == outer)
                return pb_find_varint(b + i, (size_t)len, inner);
            i += (size_t)len;
        } else if (wire == 0) {
            (void)pb_varint(b, n, &i);
        } else if (wire == 5) {
            i += 4;
        } else if (wire == 1) {
            i += 8;
        } else {
            break;
        }
    }
    return -1;
}

static uint8_t *read_bin(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)len + 1);
    *n = fread(b, 1, (size_t)len, f);
    fclose(f);
    return b;
}

/* ── phase g: STR → spool + AS11 ident ─────────────────────────────── */
static void phase_g(void)
{
    fresh_tree();
    /* AS11 Identification.json */
    char ip[700];
    snprintf(ip, sizeof(ip), "%s/Identification.json", IMPORT_DIR);
    FILE *f = fopen(ip, "w");
    fputs("{\"FlowGenerator\":{\"IdentificationProfiles\":{"
          "\"Product\":{\"SerialNumber\":\"22199876543\","
          "\"ProductName\":\"AirSense 11 AutoSet\","
          "\"PlatformIdentifier\":46,\"VariantIdentifier\":1},"
          "\"Software\":{\"SoftwareVersion\":\"SX566-0302\"}}}}", f);
    fclose(f);

    /* STR.edf — two day records.  "Date" carries days-since-epoch:
     * 2024-01-15 = 19737, 2024-01-14 = 19736. */
    static int16_t str_date[2]  = { 19737, 19736 };
    static int16_t str_on[40]   = { 630, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                    620, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    static int16_t str_off[40]  = { 1050, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                    1040, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    static int16_t str_dur[2]   = { 420, 420 };
    static int16_t str_mode[2]  = { 1, 1 };     /* AutoSetProfile */
    static int16_t str_ahi[2]   = { 23, 18 };   /* spool raw = dig*10 */
    static int16_t str_oai[2]   = { 11, 9 };
    static int16_t str_mp[2]    = { 1080, 1100 };
    const fx_sig_t str_s[] = {
        { "Date",    "d",   0, 40000,   0, 40000, 1, str_date, NULL, 0 },
        { "MaskOn",  "min", -1, 1440,  -1, 1440, 20, str_on,   NULL, 0 },
        { "MaskOff", "min", -1, 1440,  -1, 1440, 20, str_off,  NULL, 0 },
        { "Duration","min", 0,  1440,   0, 1440,  1, str_dur,  NULL, 0 },
        { "Mode",    "",    0,  32,     0,   32,  1, str_mode, NULL, 0 },
        { "AHI",     "e/h", 0,  300,    0,  300,  1, str_ahi,  NULL, 0 },
        { "OAI",     "e/h", 0,  150,    0,  150,  1, str_oai,  NULL, 0 },
        { "MaskPress.50", "cmH2O", 0, 40, 0, 2000, 1, str_mp,  NULL, 0 },
    };
    char sp[700];
    snprintf(sp, sizeof(sp), "%s/STR.edf", IMPORT_DIR);
    /* real STR files use record_duration=86400 s (one day per record) */
    fx_write_edf(sp, "SRN=22199876543 MID=46 VID=1",
                 2024, 1, 16, 0, 0, 5, "", "86400", 2, 2, str_s, 8, 0);

    /* session on 2024-01-15 so the resolved AS11 ident gets attached */
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%s", DATALOG_DAY, DAY);
    build_session_dir(dir, "20240115_221007", "20240115_221000", true,
                      2024, 1, 15);

    edf_import_ctx_t *c = edf_import_ctx_create();
    int units;
    edf_import_unit_t t = drain(c, &units);
    CHECK(t.spools_written == 2, "two spools from STR");
    CHECK(t.sessions_imported == 1, "session imported");

    /* 20240115.spool — field walk: f2 noon_ms(0115), f3 +24h, f5=420,
     * f7 AHI raw=230, f10 OAI raw=110, f39 session count=1. */
    snprintf(sp, sizeof(sp), "%s/20240115.spool", SD_SUMMARIES_DIR);
    size_t n = 0;
    uint8_t *blob = read_bin(sp, &n);
    CHECK(blob && n > 20, "spool 20240115 non-empty");
    if (blob) {
        CHECK(pb_find_varint(blob, n, 2) == 1705320000000LL,
              "f2 noon 2024-01-15 UTC");
        CHECK(pb_find_varint(blob, n, 5) == 420, "f5 duration");
        CHECK(pb_find_varint(blob, n, 7) == 230, "f7 AHI raw");
        CHECK(pb_find_varint(blob, n, 10) == 110, "f10 OAI raw");
        CHECK(pb_find_varint(blob, n, 39) == 1, "f39 session count");
        free(blob);
    }
    snprintf(sp, sizeof(sp), "%s/20240114.spool", SD_SUMMARIES_DIR);
    blob = read_bin(sp, &n);
    CHECK(blob != NULL, "spool 20240114 exists");
    if (blob) {
        CHECK(pb_find_varint(blob, n, 7) == 180, "f7 AHI day2");
        free(blob);
    }

    /* <day>.settings.json — TherapyProfile from Mode dig=1 → AutoSet */
    snprintf(sp, sizeof(sp), "%s/20240115.settings.json", SD_SUMMARIES_DIR);
    cJSON *sj = parse_json_file(sp);
    CHECK(sj != NULL, "day settings written");
    if (sj) {
        cJSON *v = cJSON_GetObjectItem(
            cJSON_GetObjectItem(cJSON_GetObjectItem(sj, "SettingProfiles"),
                                "ActiveProfiles"), "TherapyProfile");
        CHECK(v && !strcmp(v->valuestring, "AutoSetProfile"), "AutoSet profile");
        cJSON_Delete(sj);
    }
    CHECK(!file_exists(ip), "Identification.json consumed");
    snprintf(sp, sizeof(sp), "%s/STR.edf", IMPORT_DIR);
    CHECK(!file_exists(sp), "STR.edf consumed");

    /* the imported session's _ident.json carries the resolved AS11 ident */
    snprintf(sp, sizeof(sp), "%s/%s/20240115_221000_ident.json",
             SD_STREAMS_DIR, DAY);
    cJSON *ij = parse_json_file(sp);
    CHECK(ij != NULL, "ident.json written");
    if (ij) {
        cJSON *v = cJSON_GetObjectItem(ij, "SerialNumber");
        CHECK(v && !strcmp(v->valuestring, "22199876543"),
              "serial from Identification.json");
        v = cJSON_GetObjectItem(ij, "ProductName");
        CHECK(v && !strcmp(v->valuestring, "AirSense 11 AutoSet"),
              "product from Identification.json");
        v = cJSON_GetObjectItem(ij, "SoftwareVersion");
        CHECK(v && !strcmp(v->valuestring, "SX566-0302"), "sw version");
        cJSON_Delete(ij);
    }
    edf_import_ctx_destroy(c);

    /* Re-importing the same STR: both spools exist already, so none counts
     * as new (the worker would otherwise rebuild + re-offer a day for no
     * reason) */
    snprintf(sp, sizeof(sp), "%s/STR.edf", IMPORT_DIR);
    fx_write_edf(sp, "SRN=22199876543 MID=46 VID=1",
                 2024, 1, 16, 0, 0, 5, "", "86400", 2, 2, str_s, 8, 0);
    c = edf_import_ctx_create();
    t = drain(c, &units);
    CHECK(t.spools_written == 0, "re-imported STR creates no new spools");
    edf_import_ctx_destroy(c);

    /* A fresh ctx with no root ident falls back to EDF recording_id */
    snprintf(dir, sizeof(dir), "%s/%s", DATALOG_DAY, "20240116");
    build_session_dir(dir, "20240116_221007", "20240116_221000", true,
                      2024, 1, 16);
    c = edf_import_ctx_create();
    drain(c, &units);
    snprintf(sp, sizeof(sp), "%s/%s/20240116_221000_ident.json",
             SD_STREAMS_DIR, "20240116");
    ij = parse_json_file(sp);
    CHECK(ij != NULL, "rec_id fallback ident written");
    if (ij) {
        cJSON *v = cJSON_GetObjectItem(ij, "SerialNumber");
        CHECK(v && !strcmp(v->valuestring, "22199999999"), "SRN from rec_id");
        v = cJSON_GetObjectItem(ij, "PlatformIdentifier");
        CHECK(v && (int)v->valuedouble == 46, "MID=46");
        v = cJSON_GetObjectItem(ij, "ProductName");
        CHECK(v && !strcmp(v->valuestring, "AirSense 11"), "MID>=40 → AS11");
        cJSON_Delete(ij);
    }
    edf_import_ctx_destroy(c);
    printf("phase g OK (%d checks)\n", g_checks);
}

/* ── phase h: crash residue cleanup ────────────────────────────────── */
static void phase_h(void)
{
    fresh_tree();
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%s", SD_STREAMS_DIR, DAY);
    mkpath(dir);

    /* Simulate an import killed mid-commit: marker + staged parts + one
     * renamed stream, NO manifest. */
    char p[700];
    snprintf(p, sizeof(p), "%s/20240115_221000_session.json.importing", dir);
    FILE *f = fopen(p, "w"); fclose(f);
    snprintf(p, sizeof(p), "%s/20240115_221000_flow.snt.part", dir);
    f = fopen(p, "wb"); fwrite("0123456789", 1, 10, f); fclose(f);
    snprintf(p, sizeof(p), "%s/20240115_221000_events.snt", dir);
    f = fopen(p, "wb"); fwrite("0123456789", 1, 10, f); fclose(f);
    /* A committed marker (manifest exists) must survive: */
    snprintf(p, sizeof(p), "%s/20240115_230000_session.json.importing", dir);
    f = fopen(p, "w"); fclose(f);
    snprintf(p, sizeof(p), "%s/20240115_230000_session.json", dir);
    f = fopen(p, "w");
    fputs("{\"start_epoch_ms\":1705360200000}", f); fclose(f);
    snprintf(p, sizeof(p), "%s/20240115_230000_flow.snt", dir);
    f = fopen(p, "wb"); fwrite("0123456789", 1, 10, f); fclose(f);

    edf_import_ctx_t *c = edf_import_ctx_create();
    int units;
    drain(c, &units);   /* nothing to import — cleanup still runs */
    edf_import_ctx_destroy(c);

    snprintf(p, sizeof(p), "%s/20240115_221000_session.json.importing", dir);
    CHECK(!file_exists(p), "dead marker removed");
    snprintf(p, sizeof(p), "%s/20240115_221000_flow.snt.part", dir);
    CHECK(!file_exists(p), "stale .part removed");
    snprintf(p, sizeof(p), "%s/20240115_221000_events.snt", dir);
    CHECK(!file_exists(p), "orphan events.snt removed");
    snprintf(p, sizeof(p), "%s/20240115_230000_session.json", dir);
    CHECK(file_exists(p), "committed manifest kept");
    snprintf(p, sizeof(p), "%s/20240115_230000_flow.snt", dir);
    CHECK(file_exists(p), "committed flow kept");
    snprintf(p, sizeof(p), "%s/20240115_230000_session.json.importing", dir);
    CHECK(!file_exists(p), "committed marker removed");
    printf("phase h OK (%d checks)\n", g_checks);
}

/* ── phase i: annotation-only session ──────────────────────────────── */
static void phase_i(void)
{
    fresh_tree();
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%s", DATALOG_DAY, DAY);
    mkpath(dir);
    static const char tal[] = "+0\x14\x14\x00+1.0\x14" "Mask On\x14\x00";
    const fx_sig_t eve_s[] = {
        { "EDF Annotations", "", -32768.0, 32767.0, -32768, 32767, 50,
          NULL, tal, (int)sizeof(tal) - 1 },
    };
    char p[700];
    snprintf(p, sizeof(p), "%s/20240115_221000_EVE.edf", dir);
    fx_write_edf(p, "SRN=22199999999 MID=46 VID=1",
                 2024, 1, 15, 22, 10, 0, "EDF+D", "1", 1, 1, eve_s, 1, 0);

    edf_import_ctx_t *c = edf_import_ctx_create();
    int units;
    edf_import_unit_t t = drain(c, &units);
    CHECK(t.sessions_imported == 1, "ann-only session imported");

    /* empty streams exist so edf_gen can still emit the session */
    static const char *const sfx[] = {
        "_flow.snt", "_press.snt", "_flow_mm.snt", "_sa2.snt", "_pld.snt"
    };
    for (int i = 0; i < 5; i++) {
        snprintf(p, sizeof(p), "%s/%s/20240115_221000%s",
                 SD_STREAMS_DIR, DAY, sfx[i]);
        snt_header_t h; int16_t *d;
        CHECK(snt_read(p, &h, &d), sfx[i]);
        CHECK(h.sample_count == 0, "header-only stream");
        free(d);
    }
    edf_import_ctx_destroy(c);
    printf("phase i OK (%d checks)\n", g_checks);
}

/* ── phase j: one day-dir, sessions split across the noon seam ──────── */
static void phase_j(void)
{
    fresh_tree();
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%s", DATALOG_DAY, DAY);   /* .../20240115 */
    /* 01:00 belongs to the previous noon-day; 22:00 stays on 0115. */
    build_session_dir(dir, "20240115_010007", "20240115_010000", false,
                      2024, 1, 15);
    build_session_dir(dir, "20240115_220007", "20240115_220000", false,
                      2024, 1, 15);

    edf_import_ctx_t *c = edf_import_ctx_create();
    int units;
    edf_import_unit_t t = drain(c, &units);
    CHECK(t.sessions_imported == 2, "both sessions imported");
    CHECK(!strcmp(t.day_label, "20240114"), "early session's day reported");
    CHECK(!strcmp(t.day2_label, "20240115"), "second streams day reported");

    char p[700];
    snprintf(p, sizeof(p), "%s/20240114/20240115_010000_session.json",
             SD_STREAMS_DIR);
    CHECK(file_exists(p), "early session under previous noon-day");
    snprintf(p, sizeof(p), "%s/20240115/20240115_220000_session.json",
             SD_STREAMS_DIR);
    CHECK(file_exists(p), "evening session under dir-name day");
    snprintf(p, sizeof(p), "%s/20240114.spool", SD_SUMMARIES_DIR);
    CHECK(file_exists(p), "spool synthesized for 20240114");
    snprintf(p, sizeof(p), "%s/20240115.spool", SD_SUMMARIES_DIR);
    CHECK(file_exists(p), "spool synthesized for 20240115");
    edf_import_ctx_destroy(c);
    printf("phase j OK (%d checks)\n", g_checks);
}

/* ── phase k: AS10 PLD aliases + label boundary + S.* settings overlay ── */
static void phase_k(void)
{
    fresh_tree();
    char dir[256], p[700];
    snprintf(dir, sizeof(dir), "%s/%s", DATALOG_DAY, DAY);
    build_session_dir(dir, "20240115_221007", "20240115_221000", false,
                      2024, 1, 15);

    /* Overwrite PLD.edf with the AS10 short-label variant — every channel
     * carries a distinct dig so a misrouted probe cannot pass silently.
     * phys = dig/10, stored native = phys*100 = dig*10. */
    static int16_t s_mp[3]={110,111,112}, s_pr[3]={220,221,222},
                   s_epr[3]={330,331,332}, s_lk[3]={440,441,442},
                   s_rr[3]={550,551,552}, s_vt[3]={660,661,662},
                   s_mv[3]={770,771,772}, s_tg[3]={880,881,882},
                   s_ie[3]={990,991,992}, s_sn[3]={120,240,360},
                   s_fl[3]={150,250,350}, s_ti[3]={260,360,460};
    const fx_sig_t pld_s[] = {
        {"MaskPress.2s","",0,100,0,1000,1,s_mp,NULL,0},
        {"Press.2s","",0,100,0,1000,1,s_pr,NULL,0},
        {"EprPress.2s","",0,100,0,1000,1,s_epr,NULL,0},
        {"Leak.2s","",0,100,0,1000,1,s_lk,NULL,0},
        {"RR.2s","",0,100,0,1000,1,s_rr,NULL,0},     /* AS10 RespRate */
        {"Vt.2s","",0,100,0,1000,1,s_vt,NULL,0},     /* AS10 TidVol   */
        {"MV.2s","",0,100,0,1000,1,s_mv,NULL,0},     /* AS10 MinVent  */
        {"TgMV.2s","",0,100,0,1000,1,s_tg,NULL,0},   /* AS10 TgtVent  */
        {"I:E.2s","",0,100,0,1000,1,s_ie,NULL,0},    /* AS10 IERatio  */
        {"Snore.2s","",0,100,0,1000,1,s_sn,NULL,0},
        {"FL.2s","",0,100,0,1000,1,s_fl,NULL,0},     /* AS10 FlowLim  */
        {"Ti.2s","",0,100,0,1000,1,s_ti,NULL,0},
    };
    snprintf(p, sizeof(p), "%s/20240115_221007_PLD.edf", dir);
    fx_write_edf(p, "SRN=22199876543 MID=38 VID=1",
                 2024, 1, 15, 22, 10, 7, "", "1", 3, 3, pld_s, 12, 0);

    /* CurrentSettings.json — carries a marker field the STR does not
     * cover; the S.* overlay must preserve it (not stub the tree). */
    snprintf(p, sizeof(p), "%s/SETTINGS", IMPORT_DIR);
    mkpath(p);
    snprintf(p, sizeof(p), "%s/SETTINGS/CurrentSettings.json", IMPORT_DIR);
    FILE *f = fopen(p, "w");
    fputs("{\"FlowGenerator\":{\"SettingProfiles\":{\"TherapyProfiles\":"
          "{\"CpapProfile\":{\"SetPressure\":4.0}},"
          "\"FeatureProfiles\":{\"PatientViewFeature\":"
          "{\"PatientView\":\"Basic\"}}}}}", f);
    fclose(f);

    /* STR.edf — one day, Mode=3 (CPAP), S.* settings channels, plus a
     * Flow.50 decoy ahead of Flow.5 so an unbounded prefix lookup would
     * read the median into the 5th-percentile slot. */
    static int16_t k_date[1]={19737}, k_dur[1]={420}, k_mode[1]={3},
                   k_cp[1]={500}, k_ramp[1]={2}, k_hum[1]={6},
                   k_mask[1]={3}, k_cc[1]={1},
                   k_f50[1]={300}, k_f5[1]={500};
    const fx_sig_t str_s[] = {
        {"Date","d",0,40000,0,40000,1,k_date,NULL,0},
        {"Duration","min",0,1440,0,1440,1,k_dur,NULL,0},
        {"Mode","",0,32,0,32,1,k_mode,NULL,0},
        {"S.C.Press","cmH2O",0,40,0,2000,1,k_cp,NULL,0},
        {"S.RampEnable","",0,4,0,4,1,k_ramp,NULL,0},
        {"S.HumLevel","",0,8,0,8,1,k_hum,NULL,0},
        {"S.Mask","",0,8,0,8,1,k_mask,NULL,0},
        {"S.ClimateControl","",0,4,0,4,1,k_cc,NULL,0},
        {"Flow.50","L/s",-2,3,-1000,1500,1,k_f50,NULL,0},  /* decoy */
        {"Flow.5","L/s",-2,3,-1000,1500,1,k_f5,NULL,0},
    };
    snprintf(p, sizeof(p), "%s/STR.edf", IMPORT_DIR);
    fx_write_edf(p, "SRN=22199876543 MID=46 VID=1",
                 2024, 1, 15, 0, 0, 5, "", "86400", 1, 1, str_s, 10, 0);

    /* Identification.json so the root pass resolves AS11 (1-indexed enums) */
    snprintf(p, sizeof(p), "%s/Identification.json", IMPORT_DIR);
    f = fopen(p, "w");
    fputs("{\"FlowGenerator\":{\"IdentificationProfiles\":{"
          "\"Product\":{\"SerialNumber\":\"22199876543\","
          "\"ProductName\":\"AirSense 11 AutoSet\","
          "\"PlatformIdentifier\":46,\"VariantIdentifier\":1},"
          "\"Software\":{\"SoftwareVersion\":\"SX566-0302\"}}}}", f);
    fclose(f);

    edf_import_ctx_t *c = edf_import_ctx_create();
    int units;
    edf_import_unit_t t = drain(c, &units);
    CHECK(t.sessions_imported == 1, "session imported");
    CHECK(t.spools_written == 1, "one spool from STR");

    /* AS10 aliases — all 12 channels decoded, none stuck at 0. */
    snprintf(p, sizeof(p), "%s/%s/20240115_221000_pld.snt",
             SD_STREAMS_DIR, DAY);
    snt_header_t h; int16_t *d;
    CHECK(snt_read(p, &h, &d), "as10 pld.snt readable");
    if (h.magic == SNT_MAGIC) {
        CHECK(h.n_channels == 12 && h.sample_count == 3, "pld header");
        CHECK(d[4] == 5500 && d[5] == 6600 && d[6] == 7700,
              "RR/Vt/MV aliases");
        CHECK(d[7] == 8800 && d[8] == 9900 && d[10] == 1500,
              "TgMV/I:E/FL aliases");
        CHECK(d[11] == 2600, "Ti channel");
    }
    free(d);

    /* Flow.5 must decode from the Flow.5 signal (dig 500 → 500*1/5 = 100),
     * not the Flow.50 decoy that precedes it (dig 300 → 60). */
    snprintf(p, sizeof(p), "%s/20240115.spool", SD_SUMMARIES_DIR);
    size_t n = 0;
    uint8_t *blob = read_bin(p, &n);
    CHECK(blob != NULL, "spool exists");
    if (blob) {
        CHECK(pb_find_nested_varint(blob, n, 37, 1) == 100,
              "Flow.5 decoded (not Flow.50)");
        free(blob);
    }

    /* <day>.settings.json = CurrentSettings overlaid with S.* channels:
     * SetPressure 10.0 (dig 500/50) replaces the stub's 4.0; PatientView
     * "Basic" survives because no S.* channel covers it. */
    snprintf(p, sizeof(p), "%s/20240115.settings.json", SD_SUMMARIES_DIR);
    cJSON *sj = parse_json_file(p);
    CHECK(sj != NULL, "day settings written");
    if (sj) {
        cJSON *sp = cJSON_GetObjectItem(sj, "SettingProfiles");
        cJSON *ap = cJSON_GetObjectItem(sp, "ActiveProfiles");
        cJSON *v = cJSON_GetObjectItem(ap, "TherapyProfile");
        CHECK(v && !strcmp(v->valuestring, "CpapProfile"),
              "Mode 3 → CpapProfile");
        cJSON *tp = cJSON_GetObjectItem(sp, "TherapyProfiles");
        cJSON *cpap = cJSON_GetObjectItem(tp, "CpapProfile");
        v = cJSON_GetObjectItem(cpap, "SetPressure");
        CHECK(v && cJSON_IsNumber(v) && v->valuedouble == 10.0,
              "S.C.Press 500 → SetPressure 10.0");
        cJSON *fp = cJSON_GetObjectItem(sp, "FeatureProfiles");
        v = cJSON_GetObjectItem(cJSON_GetObjectItem(fp, "AutoRampFeature"),
                                "RampEnable");
        CHECK(v && !strcmp(v->valuestring, "On"), "S.RampEnable 2 → On");
        v = cJSON_GetObjectItem(cJSON_GetObjectItem(fp, "ClimateFeature"),
                                "HumidifierLevel");
        CHECK(v && cJSON_IsNumber(v) && v->valuedouble == 6.0,
              "S.HumLevel 6 → 6.0");
        v = cJSON_GetObjectItem(cJSON_GetObjectItem(fp, "ClimateFeature"),
                                "ClimateControl");
        CHECK(v && !strcmp(v->valuestring, "Auto"),
              "S.ClimateControl 1 → Auto");
        v = cJSON_GetObjectItem(cJSON_GetObjectItem(fp, "CircuitFeature"),
                                "MaskType");
        CHECK(v && !strcmp(v->valuestring, "FullFace"),
              "S.Mask 3 → FullFace (1-idx)");
        v = cJSON_GetObjectItem(cJSON_GetObjectItem(fp, "PatientViewFeature"),
                                "PatientView");
        CHECK(v && !strcmp(v->valuestring, "Basic"),
              "unmapped CurrentSettings field preserved");
        cJSON_Delete(sj);
    }
    edf_import_ctx_destroy(c);
    printf("phase k OK (%d checks)\n", g_checks);
}

/* ── phase l: real ResMed annotation layout (record duration 0.00) ───── */
static void phase_l(void)
{
    char dir[256], p[700];
    static const char r0[] = "+0\x15" "0\x14" "Recording starts\x14\x00";
    snprintf(dir, sizeof(dir), "%s/%s", DATALOG_DAY, DAY);

    /* l1: annotation-only session, events out to +300 s.  The 0 s records
     * give no span, so the session must stretch to the last annotation. */
    fresh_tree();
    mkpath(dir);
    static const char h1[] = "+60.0\x15" "12\x14" "Hypopnea\x14\x00";
    static const char o1[] = "+300.0\x15" "10\x14" "Obstructive Apnea\x14\x00";
    static const char s1[] = "+120.0\x14" "CSR Start\x14\x00";
    static const char s2[] = "+180.0\x14" "CSR End\x14\x00";
    const char *const ea[] = { r0, h1, o1 };
    const int el[] = { (int)sizeof(r0) - 1, (int)sizeof(h1) - 1, (int)sizeof(o1) - 1 };
    const char *const ca[] = { r0, s1, s2 };
    const int cl_[] = { (int)sizeof(r0) - 1, (int)sizeof(s1) - 1, (int)sizeof(s2) - 1 };
    snprintf(p, sizeof(p), "%s/20240115_221000_EVE.edf", dir);
    fx_write_real_ann(p, 2024, 1, 15, 22, 10, 0, ea, el, 3);
    snprintf(p, sizeof(p), "%s/20240115_221000_CSL.edf", dir);
    fx_write_real_ann(p, 2024, 1, 15, 22, 10, 0, ca, cl_, 3);

    edf_import_ctx_t *c = edf_import_ctx_create();
    int units;
    edf_import_unit_t t = drain(c, &units);
    CHECK(t.sessions_imported == 1 && t.sessions_failed == 0,
          "real-layout annotation-only session imports");
    CHECK(t.files_consumed == 2, "EVE + CSL consumed");

    snprintf(p, sizeof(p), "%s/%s/20240115_221000_events.snt", SD_STREAMS_DIR, DAY);
    char *ev = read_text(p);
    CHECK(ev != NULL, "events readable");
    if (ev) {
        CHECK(strstr(ev, "\"HypopneaEnd\""), "0.00-duration EVE: hypopnea kept");
        CHECK(strstr(ev, "\"ObstructiveApneaEnd\""), "0.00-duration EVE: OSA kept");
        CHECK(strstr(ev, "\"CsrStart\"") && strstr(ev, "\"CsrEnd\""),
              "0.00-duration CSL: CSR pair kept");
        CHECK(!strstr(ev, "Recording starts"), "Recording starts skipped");
        free(ev);
    }
    snprintf(p, sizeof(p), "%s/%s/20240115_221000_session.json", SD_STREAMS_DIR, DAY);
    cJSON *m = parse_json_file(p);
    CHECK(m != NULL, "manifest parses");
    if (m) {
        cJSON *a = cJSON_GetObjectItem(m, "start_epoch_ms");
        cJSON *b = cJSON_GetObjectItem(m, "end_epoch_ms");
        CHECK(a && b && (int64_t)b->valuedouble - (int64_t)a->valuedouble == 310000,
              "session spans to the last annotation end (+300 s + 10 s)");
        cJSON_Delete(m);
    }
    /* the zero-wave session is counted in the synthesized day spool */
    snprintf(p, sizeof(p), "%s/20240115.spool", SD_SUMMARIES_DIR);
    size_t n = 0;
    uint8_t *blob = read_bin(p, &n);
    CHECK(blob != NULL, "day spool synthesized");
    if (blob) {
        CHECK(pb_find_varint(blob, n, 39) == 1, "session counted in spool");
        CHECK(pb_find_varint(blob, n, 5) == 5, "310 s → 5 whole minutes");
        free(blob);
    }
    edf_import_ctx_destroy(c);

    /* l2: only "Recording starts" (no events) — still a valid, positive-length
     * session so it isn't dropped from the day's SessionModeEntries. */
    fresh_tree();
    mkpath(dir);
    const char *const ra[] = { r0 };
    const int rl[] = { (int)sizeof(r0) - 1 };
    snprintf(p, sizeof(p), "%s/20240115_221000_EVE.edf", dir);
    fx_write_real_ann(p, 2024, 1, 15, 22, 10, 0, ra, rl, 1);
    c = edf_import_ctx_create();
    t = drain(c, &units);
    CHECK(t.sessions_imported == 1, "event-less real-layout session imports");
    snprintf(p, sizeof(p), "%s/%s/20240115_221000_session.json", SD_STREAMS_DIR, DAY);
    m = parse_json_file(p);
    if (m) {
        cJSON *a = cJSON_GetObjectItem(m, "start_epoch_ms");
        cJSON *b = cJSON_GetObjectItem(m, "end_epoch_ms");
        CHECK(a && b && (int64_t)b->valuedouble - (int64_t)a->valuedouble == 1000,
              "zero-length session floors at +1 s");
        cJSON_Delete(m);
    }
    snprintf(p, sizeof(p), "%s/20240115.spool", SD_SUMMARIES_DIR);
    blob = read_bin(p, &n);
    CHECK(blob && pb_find_varint(blob, n, 39) == 1, "floor keeps it in the spool");
    free(blob);
    edf_import_ctx_destroy(c);

    /* l3: the relaxation is for annotation files only — a waveform file
     * declaring a 0 s record is malformed and must be quarantined, never
     * converted. */
    fresh_tree();
    mkpath(dir);
    for (int i = 0; i < 75; i++) brp_flow[i] = (int16_t)(i * 10);
    for (int i = 0; i < 75; i++) brp_press[i] = (int16_t)(100 + i);
    const fx_sig_t brp_s[] = {
        { "Flow.40ms", "L/s", -2.0, 3.0, -1000, 1500, 25, brp_flow, NULL, 0 },
        { "Press.40ms", "cmH2O", 0.0, 40.0, 0, 2000, 25, brp_press, NULL, 0 },
    };
    snprintf(p, sizeof(p), "%s/20240115_221007_BRP.edf", dir);
    fx_write_edf(p, "SRN=22199999999 MID=46 VID=1",
                 2024, 1, 15, 22, 10, 7, "", "0", 3, 3, brp_s, 2, 0);
    c = edf_import_ctx_create();
    t = drain(c, &units);
    CHECK(t.sessions_imported == 0 && t.sessions_failed == 1,
          "waveform with 0 s records is rejected");
    CHECK(t.files_rejected == 1, "…and quarantined");
    char bad[760];
    snprintf(bad, sizeof(bad), "%s.bad", p);
    CHECK(file_exists(bad) && !file_exists(p), "BRP renamed to .bad");
    snprintf(p, sizeof(p), "%s/%s/20240115_221007_session.json", SD_STREAMS_DIR, DAY);
    CHECK(!file_exists(p), "no manifest committed");
    CHECK(!edf_import_pending(), ".bad does not keep the inbox pending");
    edf_import_ctx_destroy(c);

    printf("phase l OK (%d checks)\n", g_checks);
}

/* ── phase m: drift sidecar size budget ────────────────────────────── */

/* A GET_DRIFT-shaped sidecar: scalar tier records plus the bulky members
 * (file tuples, component lists, a free-text flags string) that real
 * records carry.  n_files tunes the size; flags_len adds one giant scalar. */
static cJSON *mk_sidecar(int n_files, int flags_len)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "schema", 5);
    cJSON *sot = cJSON_AddObjectToObject(r, "source_of_truth");
    cJSON_AddStringToObject(sot, "clock_drift_ms", "214000");
    cJSON_AddNumberToObject(sot, "half_ms", 300);
    cJSON_AddStringToObject(sot, "quality", "qualified");
    cJSON_AddStringToObject(sot, "basis", "anchor");
    cJSON_AddNumberToObject(sot, "reference_at_ms", 1705356700000.0);
    cJSON *files = cJSON_AddArrayToObject(sot, "files");
    for (int i = 0; i < n_files; i++) {
        cJSON *f = cJSON_CreateObject();
        char nm[96];
        snprintf(nm, sizeof(nm), "A:\\DATALOG\\20240115\\20240115_2210%02d_BRP.edf", i % 60);
        cJSON_AddStringToObject(f, "path", nm);
        cJSON_AddNumberToObject(f, "size", 123456 + i);
        cJSON_AddStringToObject(f, "raw_start", "2024-01-15T22:10:07");
        cJSON_AddItemToArray(files, f);
    }
    cJSON *sc = cJSON_AddArrayToObject(sot, "systematic_components");
    for (int i = 0; i < 4; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "name", "timezone_prior");
        cJSON_AddNumberToObject(c, "bound_ms", 1000 + i);
        cJSON_AddItemToArray(sc, c);
    }
    if (flags_len > 0) {
        char *big = malloc((size_t)flags_len + 1);
        memset(big, 'x', (size_t)flags_len);
        big[flags_len] = '\0';
        cJSON_AddStringToObject(sot, "flags", big);
        free(big);
    }
    return r;
}

typedef struct {
    bool compact_present;     /* somnobridge_drift in the manifest */
    bool truncated;           /* somnobridge_drift_truncated */
    bool file_ref;            /* somnobridge_drift_file + the file on disk */
    int  files_in_manifest;   /* sot.files length inside the manifest (-1 absent) */
    int  files_in_copy;       /* sot.files length in the relocated copy (-1 absent) */
    long manifest_bytes;
} drift_case_t;

static drift_case_t run_sidecar_case(const char *label, int n_files, int flags_len)
{
    drift_case_t out = { false, false, false, -1, -1, 0 };
    fresh_tree();
    char dir[256], dp[700], p[700];
    snprintf(dir, sizeof(dir), "%s/%s", DATALOG_DAY, DAY);
    build_session_dir(dir, "20240115_221007", "20240115_221000", false, 2024, 1, 15);
    cJSON *sc = mk_sidecar(n_files, flags_len);
    char *txt = cJSON_PrintUnformatted(sc);
    cJSON_Delete(sc);
    snprintf(dp, sizeof(dp), "%s/_bridge_drift.json", dir);
    FILE *f = fopen(dp, "w");
    fputs(txt, f);
    fclose(f);
    free(txt);

    edf_import_ctx_t *c = edf_import_ctx_create();
    int units;
    edf_import_unit_t t = drain(c, &units);
    char msg[120];
    snprintf(msg, sizeof(msg), "%s: session imported", label);
    CHECK(t.sessions_imported == 1 && t.sessions_failed == 0, msg);
    CHECK(!file_exists(dp), "sidecar consumed");

    /* id is in the drifted domain: 22:10:00 + 214 s */
    snprintf(p, sizeof(p), "%s/%s/20240115_221334_session.json", SD_STREAMS_DIR, DAY);
    struct stat st;
    if (stat(p, &st) == 0) out.manifest_bytes = (long)st.st_size;
    snprintf(msg, sizeof(msg), "%s: manifest within the 4096 B reader cap", label);
    CHECK(out.manifest_bytes > 0 && out.manifest_bytes < 4096, msg);
    snprintf(msg, sizeof(msg), "%s: manifest within the importer budget", label);
    CHECK(out.manifest_bytes > 0 && out.manifest_bytes <= 3584, msg);

    cJSON *m = parse_json_file(p);
    CHECK(m != NULL, "manifest parses");
    if (m) {
        cJSON *v = cJSON_GetObjectItem(m, "clock_drift_ms");
        CHECK(v && (int64_t)v->valuedouble == 214000, "drift still applied");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(m, "clock_drift_usable")), "still usable");
        v = cJSON_GetObjectItem(m, "start_epoch_ms");
        CHECK(v && (int64_t)v->valuedouble == 1705356600000LL + 214000, "drifted start");
        cJSON *d = cJSON_GetObjectItem(m, "somnobridge_drift");
        out.compact_present = (d != NULL);
        out.truncated = cJSON_IsTrue(cJSON_GetObjectItem(m, "somnobridge_drift_truncated"));
        if (d) {
            cJSON *fl = cJSON_GetObjectItem(cJSON_GetObjectItem(d, "source_of_truth"), "files");
            if (cJSON_IsArray(fl)) out.files_in_manifest = cJSON_GetArraySize(fl);
        }
        cJSON *fr = cJSON_GetObjectItem(m, "somnobridge_drift_file");
        if (fr && cJSON_IsString(fr)) {
            snprintf(p, sizeof(p), "%s/%s/%s", SD_STREAMS_DIR, DAY, fr->valuestring);
            cJSON *cp = parse_json_file(p);
            out.file_ref = (cp != NULL);
            if (cp) {
                cJSON *fl2 = cJSON_GetObjectItem(cJSON_GetObjectItem(cp, "source_of_truth"), "files");
                if (cJSON_IsArray(fl2)) out.files_in_copy = cJSON_GetArraySize(fl2);
                cJSON_Delete(cp);
            }
        }
        cJSON_Delete(m);
    }
    edf_import_ctx_destroy(c);
    return out;
}

static void phase_m(void)
{
    /* m1: small sidecar — embedded verbatim, nothing relocated (unchanged) */
    drift_case_t a = run_sidecar_case("small", 1, 0);
    CHECK(a.compact_present && !a.truncated && !a.file_ref, "small: verbatim embed");
    CHECK(a.files_in_manifest == 1, "small: files array kept in the manifest");

    /* m2: ~6.6 KB (a typical large GET_DRIFT record) — compact projection in
     * the manifest, verbatim copy beside it */
    a = run_sidecar_case("large", 60, 0);
    CHECK(a.compact_present && a.truncated, "large: compact projection + truncated flag");
    CHECK(a.files_in_manifest == -1, "large: bulky files[] kept out of the manifest");
    CHECK(a.file_ref && a.files_in_copy == 60, "large: verbatim copy relocated intact");

    /* m3: > 64 KiB — projection only, no relocated copy */
    a = run_sidecar_case("huge", 700, 0);
    CHECK(a.compact_present && a.truncated && !a.file_ref,
          "huge: projection only, copy skipped");

    /* m4: a giant scalar defeats even the projection — the embed is dropped,
     * the session (and its drift fields) survive */
    a = run_sidecar_case("giant-scalar", 1, 6000);
    CHECK(!a.compact_present && a.truncated, "giant-scalar: embed dropped, flagged");
    CHECK(a.file_ref, "giant-scalar: verbatim copy still relocated");
    printf("phase m OK (%d checks)\n", g_checks);
}

/* ── phase n: quiescence window (copy still in flight) ─────────────── */

/* Stamp every file under `root` with mtime = now + delta_s. */
static void touch_tree(const char *root, long delta_s)
{
    char cmd[900];
    snprintf(cmd, sizeof(cmd), "find '%s' -type f -exec touch -d @%ld {} +",
             root, (long)time(NULL) + delta_s);
    if (system(cmd) != 0) printf("  warn: touch_tree %s failed\n", root);
}

static void phase_n(void)
{
    char dir[256], p[700];
    snprintf(dir, sizeof(dir), "%s/%s", DATALOG_DAY, DAY);
    int units;

    /* n1: files modified moments ago → deferred, nothing consumed; once they
     * have been quiet for the window the same unit imports. */
    fresh_tree();
    build_session_dir(dir, "20240115_221007", "20240115_221000", true, 2024, 1, 15);
    edf_import_ctx_t *c = edf_import_ctx_create();
    edf_import_ctx_set_quiesce(c, 60);
    edf_import_unit_t u;
    CHECK(edf_import_process_next(c, &u) == ESP_OK, "n1: call ok");
    CHECK(u.deferred && u.sessions_imported == 0 && u.files_consumed == 0,
          "n1: fresh files are deferred");
    snprintf(p, sizeof(p), "%s/20240115_221007_BRP.edf", dir);
    CHECK(file_exists(p), "n1: sources untouched while deferred");
    CHECK(edf_import_pending(), "n1: still pending");
    touch_tree(IMPORT_DIR, -120);
    CHECK(edf_import_process_next(c, &u) == ESP_OK && !u.deferred &&
          u.sessions_imported == 1, "n1: quiet files import");
    CHECK(!file_exists(p), "n1: sources consumed after import");
    edf_import_ctx_destroy(c);

    /* n2: a root copy in flight (Identification.json just written) holds
     * the whole inbox, aged day files included — sessions need the root's
     * ident/settings. */
    fresh_tree();
    build_session_dir(dir, "20240115_221007", "20240115_221000", false, 2024, 1, 15);
    touch_tree(IMPORT_DIR, -120);
    snprintf(p, sizeof(p), "%s/Identification.json", IMPORT_DIR);
    FILE *f = fopen(p, "w");
    fputs("{\"FlowGenerator\":{\"IdentificationProfiles\":{\"Product\":"
          "{\"SerialNumber\":\"22199876543\",\"PlatformIdentifier\":46}}}}", f);
    fclose(f);                                   /* mtime = now */
    c = edf_import_ctx_create();
    edf_import_ctx_set_quiesce(c, 60);
    CHECK(edf_import_process_next(c, &u) == ESP_OK && u.deferred && u.root_pass,
          "n2: fresh root file defers the root pass");
    snprintf(p, sizeof(p), "%s/20240115_221007_BRP.edf", dir);
    CHECK(file_exists(p), "n2: day files held back too");
    touch_tree(IMPORT_DIR, -120);
    edf_import_unit_t t = drain(c, &units);
    CHECK(t.sessions_imported == 1, "n2: everything imports once quiet");
    edf_import_ctx_destroy(c);

    /* n3: a fresh drift sidecar also holds its group */
    fresh_tree();
    build_session_dir(dir, "20240115_221007", "20240115_221000", false, 2024, 1, 15);
    touch_tree(IMPORT_DIR, -120);
    snprintf(p, sizeof(p), "%s/_bridge_drift.json", dir);
    f = fopen(p, "w");
    fputs("{\"source_of_truth\":{\"clock_drift_ms\":\"1000\",\"half_ms\":300}}", f);
    fclose(f);
    c = edf_import_ctx_create();
    edf_import_ctx_set_quiesce(c, 60);
    CHECK(edf_import_process_next(c, &u) == ESP_OK && u.deferred,
          "n3: fresh sidecar defers the group");
    touch_tree(IMPORT_DIR, -120);
    t = drain(c, &units);
    CHECK(t.sessions_imported == 1, "n3: imports once the sidecar is quiet");
    edf_import_ctx_destroy(c);

    /* n4: files dated in the future (unset device clock / fast PC clock)
     * carry no recency information and must not defer forever */
    fresh_tree();
    build_session_dir(dir, "20240115_221007", "20240115_221000", false, 2024, 1, 15);
    touch_tree(IMPORT_DIR, 3600);
    c = edf_import_ctx_create();
    edf_import_ctx_set_quiesce(c, 60);
    t = drain(c, &units);
    CHECK(t.sessions_imported == 1, "n4: future-dated files import");
    edf_import_ctx_destroy(c);

    /* n5: window 0 (explicit "import now") ignores recency */
    fresh_tree();
    build_session_dir(dir, "20240115_221007", "20240115_221000", false, 2024, 1, 15);
    c = edf_import_ctx_create();
    edf_import_ctx_set_quiesce(c, 0);
    t = drain(c, &units);
    CHECK(t.sessions_imported == 1, "n5: quiesce 0 imports fresh files");
    edf_import_ctx_destroy(c);

    printf("phase n OK (%d checks)\n", g_checks);
}

/* ── phase o: AS10 *.crc sidecars + junk husks must not wedge the scan ── */

/* AS10 cards keep a <name>.crc beside every EDF in a DATALOG day folder. */
static int crc_siblings(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t nl = strlen(e->d_name);
        if (nl <= 4 || strcasecmp(e->d_name + nl - 4, ".edf") != 0) continue;
        char p[700], c[720];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        /* AS10 replaces the extension: <name>.edf → <name>.crc */
        snprintf(c, sizeof(c), "%.*s.crc", (int)(strlen(p) - 4), p);
        FILE *f = fopen(c, "wb");
        if (f) { fputs("0000\n", f); fclose(f); n++; }
    }
    closedir(d);
    return n;
}

static void phase_o(void)
{
    char dir[256], p[720];
    int units;

    /* o1: AS10 day folders carry a .crc beside every EDF.  Importing the
     * first day must consume them, remove the husk, and reach the SECOND
     * day in the same drain — issue #338: the husk used to shadow every
     * later day, so each reboot imported exactly one folder. */
    fresh_tree();
    snprintf(dir, sizeof(dir), "%s/20240115", IMPORT_DIR);
    build_session_dir(dir, "20240115_221007", "20240115_221000", true, 2024, 1, 15);
    CHECK(crc_siblings(dir) == 5, "o1: 5 .crc siblings written");
    snprintf(dir, sizeof(dir), "%s/20240116", IMPORT_DIR);
    build_session_dir(dir, "20240116_221007", "20240116_221000", true, 2024, 1, 16);
    CHECK(crc_siblings(dir) == 5, "o1: second day's .crc siblings written");
    edf_import_ctx_t *c = edf_import_ctx_create();
    edf_import_unit_t t = drain(c, &units);
    CHECK(units == 2 && t.sessions_imported == 2,
          "o1: both day folders import in one drain");
    char h1[300], h2[300];
    snprintf(h1, sizeof(h1), "%s/20240115", IMPORT_DIR);
    snprintf(h2, sizeof(h2), "%s/20240116", IMPORT_DIR);
    CHECK(!file_exists(h1) && !file_exists(h2),
          "o1: husks removed (.crc consumed with their EDF)");
    CHECK(!edf_import_pending(), "o1: inbox empty afterwards");
    edf_import_ctx_destroy(c);

    /* o2: a leftover-only folder with the LOWEST key is skipped, not an
     * abort of the whole scan */
    fresh_tree();
    snprintf(p, sizeof(p), "%s/20240114", IMPORT_DIR);
    mkpath(p);
    snprintf(p, sizeof(p), "%s/20240114/notes.txt", IMPORT_DIR);
    FILE *f = fopen(p, "w");
    if (f) { fputs("keep me\n", f); fclose(f); }
    snprintf(dir, sizeof(dir), "%s/20240115", IMPORT_DIR);
    build_session_dir(dir, "20240115_221007", "20240115_221000", false, 2024, 1, 15);
    c = edf_import_ctx_create();
    t = drain(c, &units);
    CHECK(t.sessions_imported == 1, "o2: day after a leftover-only folder imports");
    char notes[320];
    snprintf(notes, sizeof(notes), "%s/20240114/notes.txt", IMPORT_DIR);
    CHECK(file_exists(notes), "o2: the user's own file is left alone");
    edf_import_ctx_destroy(c);

    /* o3: a corrupt EDF is quarantined *.bad and must not pin the inbox */
    fresh_tree();
    snprintf(dir, sizeof(dir), "%s/20240115", IMPORT_DIR);
    build_session_dir(dir, "20240115_221007", "20240115_221000", false, 2024, 1, 15);
    snprintf(p, sizeof(p), "%s/20240115_221007_BRP.edf", dir);
    f = fopen(p, "wb");
    if (f) { fputs("garbage", f); fclose(f); }
    snprintf(dir, sizeof(dir), "%s/20240116", IMPORT_DIR);
    build_session_dir(dir, "20240116_221007", "20240116_221000", false, 2024, 1, 16);
    c = edf_import_ctx_create();
    t = drain(c, &units);
    CHECK(t.files_rejected >= 1, "o3: corrupt EDF quarantined");
    CHECK(t.sessions_imported == 1, "o3: the healthy day still imports");
    snprintf(h2, sizeof(h2), "%s/20240116", IMPORT_DIR);
    CHECK(!file_exists(h2), "o3: healthy day's husk removed");
    edf_import_ctx_destroy(c);

    printf("phase o OK (%d checks)\n", g_checks);
}

int main(void)
{
    setenv("TZ", "UTC", 1);
    tzset();
    snprintf(DATALOG_DAY, sizeof(DATALOG_DAY), "%s/DATALOG", IMPORT_DIR);
    mkpath(DATALOG_DAY);

    phase_a();
    phase_b();
    phase_c();
    phase_d();
    phase_e();
    phase_f();
    phase_g();
    phase_h();
    phase_i();
    phase_j();
    phase_k();
    phase_l();
    phase_m();
    phase_n();
    phase_o();

    rmtree(SD_MOUNT_POINT);
    printf("edf_import_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
