/*
 * SomnoTrace - Host unit tests for edf_gen.c (.snt → EDF conversion)
 * Copyright (C) 2026 Plantucha <https://github.com/Plantucha>
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

/*
 * These tests link the REAL converter: edf_gen.c is #included so that its
 * static helpers (convert_snt_to_edf, build_str_data_values,
 * ...) are reachable, and the generated EDF is parsed back and checked.
 * Nothing here is a copy of production code — when edf_gen.c changes, these
 * tests change behaviour with it.
 *
 * Oracles, in order of preference:
 *   1. differential — two representations of the same data (a v1 .snt and a
 *      v2 .snt, ESP-local vs AS11 noon-day) must produce identical output;
 *   2. format contract — values fixed by the SNT/EDF/AS11 file formats
 *      (the missing-data marker is -1 in the EDF, noon belongs to the new
 *      noon-day, a pressure setting is cmH2O × 50 exactly);
 *   3. never a constant copied out of the implementation.
 *
 * Build: see scripts/run_host_tests.sh (needs a real cJSON, the shim in
 * scripts/test_include is too small for edf_gen.c).
 */

/* v2.0.0 split the converter into five modules. All are #included so the tests still
 * reach the static helpers (spool_to_edf, build_str_data_values) they exercise. */
/* Each module defines its own static TAG; rename per include so they coexist. */
#define TAG TAG_hdr
#include "edf_header.c"       /* found via -I<main dir>, see run_host_tests.sh */
#undef TAG
#define TAG TAG_wav
#include "edf_waveform.c"
#undef TAG
#define TAG TAG_ann
#include "edf_annotations.c"
#undef TAG
#define TAG TAG_sum
#include "edf_summary.c"
#undef TAG
#define TAG TAG_gen
#include "edf_gen.c"
#undef TAG

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* ── link stubs for the storage arbitration edf_gen.c calls ─────────── */
bool sd_storage_is_ready(void) { return true; }
bool sd_storage_lease_acquire(sd_lease_t role, uint32_t timeout_ms)
{
    (void)role; (void)timeout_ms;
    return true;
}
void sd_storage_lease_release(sd_lease_t role) { (void)role; }

/* ── tiny harness ───────────────────────────────────────────────────── */
static int g_fails;            /* failures inside the current test */
static int n_pass, n_fail, n_xfail, n_xpass;

#define CHECK(cond, ...) do {                                             \
        if (!(cond)) {                                                    \
            g_fails++;                                                    \
            printf("    FAIL %s:%d: ", __FILE__, __LINE__);               \
            printf(__VA_ARGS__);                                          \
            printf("\n");                                                 \
        }                                                                 \
    } while (0)

/* expect_fail: a known, still-unfixed bug.  The test stays in the suite as
 * the executable statement of the bug; when it starts passing the runner
 * says so (XPASS) so the marker can be dropped in the same change as the fix.
 * Mutation runs ignore XFAIL tests — they cannot kill anything. */
static void run(const char *name, void (*fn)(void), const char *expect_fail)
{
    g_fails = 0;
    printf("%s %s\n", expect_fail ? "[xfail]" : "[test] ", name);
    fn();
    if (expect_fail) {
        if (g_fails) { n_xfail++; printf("    XFAIL (%s)\n", expect_fail); }
        else         { n_xpass++; printf("    XPASS — bug fixed? drop the expect_fail marker\n"); }
    } else if (g_fails) {
        n_fail++;
        printf("    FAILED %s (%d checks)\n", name, g_fails);   /* grep-able by the runner */
    } else {
        n_pass++;
    }
}

static void set_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

/* ── temp tree ──────────────────────────────────────────────────────── */
static char g_root[256];

static void mk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void mk(const char *fmt, ...)
{
    char p[512];
    va_list ap; va_start(ap, fmt); vsnprintf(p, sizeof(p), fmt, ap); va_end(ap);
    mkdir(p, 0775);
}

static void rmtree(const char *path)
{
    /* test trees are small and fully ours */
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    if (system(cmd) != 0) fprintf(stderr, "warn: could not remove %s\n", path);
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

/* Write an .snt file using the reader's own header struct.  `claimed` is what
 * the header says; `on_disk` is how many frames actually follow it (they
 * differ for a torn tail). */
static void write_snt(const char *path, uint8_t version, uint8_t n_ch,
                      uint16_t hz_x10, int64_t start_ms, uint32_t claimed,
                      const int16_t *frames, uint32_t on_disk)
{
    snt_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = SNT_MAGIC;
    h.version = version;
    h.n_channels = n_ch;
    h.sample_bytes = 2;
    h.sample_hz_x10 = hz_x10;
    h.start_epoch_ms = start_ms;
    h.sample_count = claimed;
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(&h, 1, sizeof(h), f) == sizeof(h));
    size_t n = (size_t)on_disk * n_ch;
    assert(fwrite(frames, sizeof(int16_t), n, f) == n);
    fclose(f);
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
    assert(buf);
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* ── minimal EDF reader (format-level, independent of edf_gen.c) ────── */
typedef struct {
    long hdr_bytes;
    int ndr, ns;
    int spr[160];
    long rec_bytes;
} edf_t;

static bool edf_parse(const uint8_t *b, size_t len, edf_t *e)
{
    char t[16];
    if (len < 256) return false;
    memcpy(t, b + 184, 8); t[8] = 0; e->hdr_bytes = atol(t);
    memcpy(t, b + 236, 8); t[8] = 0; e->ndr = atoi(t);
    memcpy(t, b + 252, 4); t[4] = 0; e->ns = atoi(t);
    if (e->ns <= 0 || e->ns > 160) return false;
    if ((size_t)e->hdr_bytes != 256 + 256 * (size_t)e->ns) return false;
    e->rec_bytes = 0;
    for (int i = 0; i < e->ns; i++) {
        memcpy(t, b + 256 + e->ns * 216 + i * 8, 8); t[8] = 0;
        e->spr[i] = atoi(t);
        e->rec_bytes += 2L * e->spr[i];
    }
    return (size_t)(e->hdr_bytes + e->rec_bytes * e->ndr) == len;
}

static int16_t edf_sample(const uint8_t *b, const edf_t *e, int rec, int sig, int s)
{
    long off = e->hdr_bytes + rec * e->rec_bytes;
    for (int j = 0; j < sig; j++) off += 2L * e->spr[j];
    off += 2L * s;
    return (int16_t)(b[off] | (b[off + 1] << 8));
}

/* Signal-block field offsets, from the EDF spec: ns×16 label, ns×80
 * transducer, ns×8 physical dimension, then phys min/max, dig min/max. */
static void edf_field(const uint8_t *b, const edf_t *e, long base, int width,
                      int i, char *out)
{
    const uint8_t *p = b + 256 + base * e->ns + (long)i * width;
    memcpy(out, p, (size_t)width);
    out[width] = '\0';
    for (int k = width - 1; k >= 0 && out[k] == ' '; k--) out[k] = '\0';
}
#define EDF_LABEL(b, e, i, o)   edf_field(b, e,   0, 16, i, o)
#define EDF_UNIT(b, e, i, o)    edf_field(b, e,  96,  8, i, o)
#define EDF_DIGMIN(b, e, i, o)  edf_field(b, e, 120,  8, i, o)
#define EDF_DIGMAX(b, e, i, o)  edf_field(b, e, 128,  8, i, o)

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), written here from the
 * algorithm's public definition rather than called out of edf_gen.c — the
 * point is to check the RANGE the exporter covers, independently. */
static uint16_t ccitt_false(const uint8_t *d, size_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= (uint16_t)*d++ << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* Structural contract every EDF this exporter writes must satisfy, whatever
 * the signal.  These are the reader's requirements (an EDF header is printable
 * ASCII, version is "0", a signal's digital range is non-empty, the physical
 * dimension is not the label again), plus the AS11's two header CRCs
 * recomputed here from the bytes on disk. */
static void check_edf_structure(const char *what, const uint8_t *b, size_t n,
                                const edf_t *e)
{
    for (long i = 0; i < e->hdr_bytes && i < (long)n; i++) {
        if (b[i] < 0x20 || b[i] > 0x7E) {
            CHECK(false, "%s: header byte %ld = 0x%02X is not printable ASCII",
                  what, i, b[i]);
            break;
        }
    }
    char f[96];
    memcpy(f, b, 8); f[8] = '\0';
    for (int k = 7; k >= 0 && f[k] == ' '; k--) f[k] = '\0';
    CHECK(strcmp(f, "0") == 0, "%s: EDF version field is \"%s\", must be \"0\"", what, f);

    for (int i = 0; i < e->ns; i++) {
        char label[24], unit[16], dmin[16], dmax[16];
        EDF_LABEL(b, e, i, label);
        EDF_UNIT(b, e, i, unit);
        EDF_DIGMIN(b, e, i, dmin);
        EDF_DIGMAX(b, e, i, dmax);
        CHECK(label[0] != '\0', "%s: signal %d has no label", what, i);
        CHECK(strcmp(unit, label) != 0,
              "%s: signal %d physical dimension is the label again (\"%s\")",
              what, i, unit);
        CHECK(atoi(dmin) < atoi(dmax),
              "%s: signal %d digital range %s..%s is empty or inverted",
              what, i, dmin, dmax);
    }

    /* CRC1 covers hdr[0x19..0xFF], CRC2 the whole signal block.  Both are
     * recorded in the patient-ID field as "X X X X <CRC1> <CRC2>". */
    unsigned h1 = 0, h2 = 0;
    char pid[81];
    memcpy(pid, b + 8, 80); pid[80] = '\0';
    if (sscanf(pid, "X X X X %04X %04X", &h1, &h2) != 2) {
        CHECK(false, "%s: patient ID field does not carry two CRCs: \"%s\"", what, pid);
        return;
    }
    CHECK(ccitt_false(b + 0x19, 256 - 0x19) == h1,
          "%s: header CRC1 %04X does not match the bytes it covers (%04X)",
          what, h1, ccitt_false(b + 0x19, 256 - 0x19));
    CHECK(ccitt_false(b + 256, (size_t)(e->hdr_bytes - 256)) == h2,
          "%s: signal-block CRC2 %04X does not match the block (%04X)",
          what, h2, ccitt_false(b + 256, (size_t)(e->hdr_bytes - 256)));
}

/* ── synthetic session ──────────────────────────────────────────────── */
#define SID        "20260301_220000"
#define START_MS   1772402400000LL     /* 2026-03-01T22:00:00Z */
/* A 3-minute session: long enough that a gated start and a MaskOff end each
 * change the record count, which is what makes those paths testable. */
#define SA2_N      180                 /* three 60 s records at 1 Hz */
#define BRP_N      4500                /* three 60 s records at 25 Hz; ≥1500 = "therapy delivered" */
#define PLD_N      90                  /* three records at 0.5 Hz */

#define FLOW_OVERFLOW_IDX 7            /* one flow sample that overflows int16 */

static bool sa2_hr_missing(int i)   { return i % 7 == 3; }
static bool sa2_spo2_missing(int i) { return i % 5 == 0; }
static int  sa2_hr_bpm(int i)       { return 60 + i % 30; }
static int  sa2_spo2_pct(int i)     { return 94 + i % 6; }

/* Build <root>/streams/20260301/<SID>_*.snt for the given SNT version.
 * v1: brp.snt (2-ch interleaved) + sa2.snt with -1 sentinels.
 * v2: flow.snt + press.snt (1-ch each) + sa2.snt with INT16_MIN sentinels.
 * The sample values are identical, only the representation differs.
 * sa2_claimed lets a test lie in the header (torn tail). */
static void build_session(const char *root, int version, uint32_t sa2_claimed,
                          char *session_dir, size_t sd_len)
{
    mk("%s", root);
    mk("%s/streams", root);
    snprintf(session_dir, sd_len, "%s/streams/20260301", root);
    mk("%s", session_dir);
    char p[400];

    int16_t sent = (version >= 2) ? INT16_MIN : -1;
    static int16_t sa2[SA2_N * 2];
    for (int i = 0; i < SA2_N; i++) {
        sa2[2 * i]     = sa2_hr_missing(i)   ? sent : (int16_t)(sa2_hr_bpm(i) * 100);
        sa2[2 * i + 1] = sa2_spo2_missing(i) ? sent : (int16_t)(sa2_spo2_pct(i) * 100);
    }
    snprintf(p, sizeof(p), "%s/%s_sa2.snt", session_dir, SID);
    write_snt(p, (uint8_t)version, 2, 10, START_MS, sa2_claimed, sa2, SA2_N);

    static int16_t flow[BRP_N], press[BRP_N], brp[BRP_N * 2];
    for (int i = 0; i < BRP_N; i++) {
        flow[i]  = (int16_t)((i % 200) - 100);      /* -1.00 .. +0.99 L/s */
        press[i] = (int16_t)(800 + (i % 300));      /*  8.00 .. 10.99 cmH2O */
        brp[2 * i] = flow[i];
        brp[2 * i + 1] = press[i];
    }
    /* One sample large enough that the physical→digital conversion overflows
     * int16.  A real sensor glitch does this, and the clamp is what stops it
     * from reading as a maximally NEGATIVE flow. */
    flow[FLOW_OVERFLOW_IDX] = 30000;
    brp[2 * FLOW_OVERFLOW_IDX] = 30000;
    if (version >= 2) {
        snprintf(p, sizeof(p), "%s/%s_flow.snt", session_dir, SID);
        write_snt(p, 2, 1, 250, START_MS, BRP_N, flow, BRP_N);
        snprintf(p, sizeof(p), "%s/%s_press.snt", session_dir, SID);
        write_snt(p, 2, 1, 250, START_MS, BRP_N, press, BRP_N);
    } else {
        snprintf(p, sizeof(p), "%s/%s_brp.snt", session_dir, SID);
        write_snt(p, 1, 2, 250, START_MS, BRP_N, brp, BRP_N);
    }
    /* PLD: 12 channels at 0.5 Hz, no sentinels (PLD channels are not
     * passthrough, so a sentinel would legitimately clamp differently). */
    static int16_t pld[PLD_N * 12];
    for (int i = 0; i < PLD_N; i++)
        for (int c = 0; c < 12; c++)
            pld[i * 12 + c] = (int16_t)((i * 7 + c * 3) % 50);   /* 0.00 .. 0.49 */
    snprintf(p, sizeof(p), "%s/%s_pld.snt", session_dir, SID);
    write_snt(p, (uint8_t)version, 12, 5, START_MS, PLD_N, pld, PLD_N);

    snprintf(p, sizeof(p), "%s/%s_ident.json", session_dir, SID);
    write_text(p, "{}\n");
    snprintf(p, sizeof(p), "%s/%s_settings.json", session_dir, SID);
    write_text(p, "{}\n");
}

/* events.snt: one JSON message per line.  A MaskOn event is the AS11's
 * data-gating signal when _ZLE is absent; the exporter must skip everything
 * the .snt files recorded before it. */
static void event_line(char *out, size_t n, const char *event, int64_t offset_ms)
{
    time_t tt = (time_t)((START_MS + offset_ms) / 1000);
    struct tm g;
    gmtime_r(&tt, &g);
    snprintf(out, n,
             "{\"params\":{\"events\":[{\"event\":\"%s\",\"reportTime\":"
             "\"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ\"}]}}\n",
             event, g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
             g.tm_hour, g.tm_min, g.tm_sec,
             (int)((START_MS + offset_ms) % 1000));
}

/* _ZLE (Zero Leak Estimate) is the AS11's real gating signal; MaskOn is only
 * the fallback.  Its timestamps are in the AS11 clock, so the exporter has to
 * add the measured clock drift to reach NTP time. */
static void write_zle_sequence(const char *session_dir, const int64_t *as11_offs,
                               const int *values, int n)
{
    char all[2048] = "", rt[512], line[700], p[600];
    for (int i = 0; i < n; i++) {
        event_line(rt, sizeof(rt), "_ZLE", as11_offs[i]);   /* borrow the ISO stamp */
        char *q = strstr(rt, "\"reportTime\":");
        CHECK(q != NULL, "event_line format changed");
        if (!q) return;
        snprintf(line, sizeof(line),
                 "{\"params\":{\"dataId\":\"_ZLE\",\"events\":[{\"value\":%d,%s",
                 values[i], q);
        strncat(all, line, sizeof(all) - strlen(all) - 1);
    }
    snprintf(p, sizeof(p), "%s/%s_events.snt", session_dir, SID);
    write_text(p, all);
}

/* maskoff_ms < 0 writes a MaskOn only (the session runs to the end). */
static void write_events(const char *session_dir, int64_t maskon_ms, int64_t maskoff_ms)
{
    char on[512], off[512], both[1100], p[600];
    event_line(on, sizeof(on), "MaskOn", maskon_ms);
    if (maskoff_ms >= 0) {
        event_line(off, sizeof(off), "MaskOff", maskoff_ms);
        snprintf(both, sizeof(both), "%s%s", on, off);
    } else {
        snprintf(both, sizeof(both), "%s", on);
    }
    snprintf(p, sizeof(p), "%s/%s_events.snt", session_dir, SID);
    write_text(p, both);
}

static esp_err_t generate_drift(const char *root, const char *session_dir,
                                char *out_dir, size_t od_len, int64_t drift_ms)
{
    snprintf(out_dir, od_len, "%s/out", root);
    return edf_gen_generate_ex(out_dir, session_dir, SID, START_MS,
                               START_MS + SA2_N * 1000LL, drift_ms, EDF_GEN_PER_SESSION);
}

static esp_err_t generate(const char *root, const char *session_dir, char *out_dir, size_t od_len)
{
    return generate_drift(root, session_dir, out_dir, od_len, 0);
}

/* ── tests ──────────────────────────────────────────────────────────── */

/* The same night written as SNT v1 (interleaved brp.snt, -1 sentinel) and as
 * SNT v2 (flow.snt + press.snt, INT16_MIN sentinel) must export identically,
 * and the missing-data sentinel — whichever encoding — must reach the EDF as
 * the AS11's -1 marker, never as a scaled value. */
static void test_sa2_v1_v2_differential(void)
{
    set_tz("UTC");
    char root1[300], root2[300], sd1[400], sd2[400], out1[400], out2[400];
    snprintf(root1, sizeof(root1), "%s/v1", g_root);
    snprintf(root2, sizeof(root2), "%s/v2", g_root);
    build_session(root1, 1, SA2_N, sd1, sizeof(sd1));
    build_session(root2, 2, SA2_N, sd2, sizeof(sd2));

    CHECK(generate(root1, sd1, out1, sizeof(out1)) == ESP_OK, "v1 generate failed");
    CHECK(generate(root2, sd2, out2, sizeof(out2)) == ESP_OK, "v2 generate failed");

    static const char *files[] = { SID "_SA2.edf", SID "_BRP.edf", SID "_PLD.edf" };
    for (int k = 0; k < 3; k++) {
        char p1[600], p2[600];
        snprintf(p1, sizeof(p1), "%s/DATALOG/20260301/%s", out1, files[k]);
        snprintf(p2, sizeof(p2), "%s/DATALOG/20260301/%s", out2, files[k]);
        size_t n1 = 0, n2 = 0;
        uint8_t *b1 = read_file(p1, &n1), *b2 = read_file(p2, &n2);
        CHECK(b1 && b2, "%s missing (v1=%p v2=%p)", files[k], (void *)b1, (void *)b2);
        if (b1 && b2) {
            CHECK(n1 == n2 && memcmp(b1, b2, n1) == 0,
                  "%s differs between SNT v1 and v2 (%zu vs %zu bytes)", files[k], n1, n2);
        }
        if (b1) {
            edf_t es;
            if (edf_parse(b1, n1, &es)) check_edf_structure(files[k], b1, n1, &es);
            else CHECK(false, "%s: header/size inconsistent", files[k]);
        }
        if (k == 0 && b1) {
            edf_t e;
            CHECK(edf_parse(b1, n1, &e), "SA2.edf header/size inconsistent");
            if (edf_parse(b1, n1, &e)) {
                CHECK(e.ndr == SA2_N / 60, "SA2 records: got %d want %d", e.ndr, SA2_N / 60);
                CHECK(e.ns == 3, "SA2 signals: got %d want 3 (Pulse, SpO2, Crc16)", e.ns);
                CHECK(e.spr[0] == 60 && e.spr[1] == 60, "SA2 samples/record: %d %d", e.spr[0], e.spr[1]);
                for (int i = 0; i < SA2_N && e.ndr == 2 && e.ns == 3; i++) {
                    int16_t hr = edf_sample(b1, &e, i / 60, 0, i % 60);
                    int16_t sp = edf_sample(b1, &e, i / 60, 1, i % 60);
                    int want_hr = sa2_hr_missing(i) ? -1 : sa2_hr_bpm(i);
                    int want_sp = sa2_spo2_missing(i) ? -1 : sa2_spo2_pct(i);
                    if (hr != want_hr || sp != want_sp) {
                        CHECK(false, "sample %d: pulse %d/%d spo2 %d/%d (got/want)",
                              i, hr, want_hr, sp, want_sp);
                        break;
                    }
                }
            }
        }
        free(b1); free(b2);
    }
}

/* A session killed mid-write has a header that promises more samples than
 * the file holds.  The export must still succeed and contain exactly the
 * whole records that are on disk. */
static void test_sa2_torn_tail_exports_what_exists(void)
{
    set_tz("UTC");
    char root[300], sd[400], out[400], p[600];
    snprintf(root, sizeof(root), "%s/torn", g_root);
    build_session(root, 2, SA2_N + 60, sd, sizeof(sd));   /* claims 3 records, has 2 */
    CHECK(generate(root, sd, out, sizeof(out)) == ESP_OK, "generate failed on torn sa2.snt");
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/%s_SA2.edf", out, SID);
    size_t n = 0;
    uint8_t *b = read_file(p, &n);
    CHECK(b != NULL, "SA2.edf not written");
    if (b) {
        edf_t e;
        CHECK(edf_parse(b, n, &e), "SA2.edf header/size inconsistent");
        if (edf_parse(b, n, &e))
            CHECK(e.ndr == SA2_N / 60, "records: got %d want %d", e.ndr, SA2_N / 60);
        free(b);
    }
}

/* A torn tail that stops PART WAY THROUGH a record must drop that record, not
 * round up to it: the EDF header states the record count before the data is
 * written, so a promised record that does not exist is an unreadable file.
 * 119 samples at 1 Hz = one whole 60 s record and 59 orphans. */
static void test_torn_tail_drops_partial_record(void)
{
    set_tz("UTC");
    char root[300], sd[400], out[400], p[600];
    snprintf(root, sizeof(root), "%s/torn119", g_root);
    build_session(root, 2, SA2_N, sd, sizeof(sd));
    /* Rewrite sa2.snt: header still claims SA2_N, only 119 frames on disk. */
    static int16_t sa2[SA2_N * 2];
    for (int i = 0; i < SA2_N; i++) {
        sa2[2 * i]     = sa2_hr_missing(i)   ? INT16_MIN : (int16_t)(sa2_hr_bpm(i) * 100);
        sa2[2 * i + 1] = sa2_spo2_missing(i) ? INT16_MIN : (int16_t)(sa2_spo2_pct(i) * 100);
    }
    snprintf(p, sizeof(p), "%s/%s_sa2.snt", sd, SID);
    write_snt(p, 2, 2, 10, START_MS, SA2_N, sa2, 119);

    CHECK(generate(root, sd, out, sizeof(out)) == ESP_OK, "generate failed");
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/%s_SA2.edf", out, SID);
    size_t n = 0;
    uint8_t *b = read_file(p, &n);
    CHECK(b != NULL, "SA2.edf not written");
    if (!b) return;
    edf_t e;
    if (edf_parse(b, n, &e)) {
        CHECK(e.ndr == 1, "records: got %d want 1 (119 samples = one whole record)", e.ndr);
        for (int i = 0; i < 60 && e.ndr >= 1; i++) {
            int want = sa2_hr_missing(i) ? -1 : sa2_hr_bpm(i);
            int got = edf_sample(b, &e, 0, 0, i);
            if (got != want) {
                CHECK(false, "sample %d: pulse %d want %d", i, got, want);
                break;
            }
        }
    } else {
        CHECK(false, "SA2.edf header/size inconsistent — a record was promised but not written");
    }
    free(b);
}

/* The .snt files start at TherapyStart, but BRP/PLD/SA2 must start at the
 * gating event (_ZLE, else MaskOn).  The export therefore drops the leading
 * samples — and must drop the SAME instant from every channel, whether the
 * night is stored as one interleaved file (v1) or one file per channel (v2). */
static void test_maskon_skip_aligns_all_channels(void)
{
    set_tz("UTC");
    char root1[300], root2[300], sd1[400], sd2[400], out1[400], out2[400], p[600];
    snprintf(root1, sizeof(root1), "%s/skip_v1", g_root);
    snprintf(root2, sizeof(root2), "%s/skip_v2", g_root);
    build_session(root1, 1, SA2_N, sd1, sizeof(sd1));
    build_session(root2, 2, SA2_N, sd2, sizeof(sd2));
    write_events(sd1, 10000, -1);         /* mask on 10 s into the recording */
    write_events(sd2, 10000, -1);

    CHECK(generate(root1, sd1, out1, sizeof(out1)) == ESP_OK, "v1 generate failed");
    CHECK(generate(root2, sd2, out2, sizeof(out2)) == ESP_OK, "v2 generate failed");

    /* The gated streams are named after the gating instant, not after
     * TherapyStart: MaskOn at +10 s ⇒ 20260301_220010_BRP.edf. */
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/20260301_220010_BRP.edf", out1);
    size_t n1 = 0, n2 = 0;
    uint8_t *b1 = read_file(p, &n1);
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/20260301_220010_BRP.edf", out2);
    uint8_t *b2 = read_file(p, &n2);
    CHECK(b1 && b2, "BRP.edf missing after a gated start (v1=%p v2=%p)", (void *)b1, (void *)b2);
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/%s_BRP.edf", out1, SID);
    size_t nu = 0;
    uint8_t *bu = read_file(p, &nu);
    CHECK(bu == NULL, "BRP.edf was also written under the ungated TherapyStart name");
    free(bu);
    if (b1 && b2) {
        CHECK(n1 == n2 && memcmp(b1, b2, n1) == 0,
              "gated BRP.edf differs between SNT v1 and v2 (%zu vs %zu bytes) — "
              "the per-channel files were not skipped by the same amount", n1, n2);
        edf_t e;
        if (edf_parse(b1, n1, &e)) check_edf_structure("BRP.edf (gated)", b1, n1, &e);
        else CHECK(false, "gated BRP.edf header/size inconsistent");
    }

    /* The same night exported WITHOUT a gating event is the oracle: gating at
     * +10 s must produce exactly the ungated samples from 250 (10 s × 25 Hz)
     * onward — no expected values, no scale constants, so a change to the
     * physical scaling cannot make this test wrong. */
    char root0[300], sd0[400], out0[400];
    snprintf(root0, sizeof(root0), "%s/skip_none", g_root);
    build_session(root0, 2, SA2_N, sd0, sizeof(sd0));
    CHECK(generate(root0, sd0, out0, sizeof(out0)) == ESP_OK, "ungated generate failed");
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/%s_BRP.edf", out0, SID);
    size_t n0 = 0;
    uint8_t *b0 = read_file(p, &n0);
    CHECK(b0 != NULL, "ungated BRP.edf missing");
    if (b0 && b1) {
        edf_t eg, eu;
        if (edf_parse(b1, n1, &eg) && edf_parse(b0, n0, &eu)) {
            CHECK(eg.spr[0] == eu.spr[0], "samples/record changed with gating: %d vs %d",
                  eg.spr[0], eu.spr[0]);
            for (int sig = 0; sig < 2 && eg.ns >= 2 && eu.ns >= 2; sig++) {
                for (int i = 0; i < eg.spr[0]; i++) {
                    int j = i + 250;
                    int16_t g = edf_sample(b1, &eg, 0, sig, i);
                    int16_t u = edf_sample(b0, &eu, j / eu.spr[0], sig, j % eu.spr[0]);
                    if (g != u) {
                        CHECK(false, "signal %d sample %d: gated %d != ungated sample %d (%d)"
                              " — the skip is misaligned", sig, i, g, j, u);
                        break;
                    }
                }
            }
        }
    }
    free(b0); free(b1); free(b2);
}

/* The signal table an AS11 writes for each stream.  Transcribed from the
 * machine's own DATALOG output, not from this firmware: every one of these
 * values is identical across a 233-day card (292 BRP/PLD/SA2 sessions).
 * OSCAR and every other reader keys off these, so a stream that declares
 * something else is a stream that reads wrong, however right the samples are.
 * The Crc16 signal the writer appends is checked separately. */
typedef struct {
    const char *label, *unit, *pmin, *pmax, *dmin, *dmax;
    int spr;
} as11_sig_t;

static const as11_sig_t AS11_SA2[] = {
    { "Pulse.1s", "bpm", "0.00", "300.00", "0", "300", 60 },
    { "SpO2.1s",  "%",   "0.00", "100.00", "0", "100", 60 },
};
static const as11_sig_t AS11_BRP[] = {
    { "Flow.40ms",  "L/s",   "-2.00", "3.00",  "-1000", "1500", 1500 },
    { "Press.40ms", "cmH2O", "0.00",  "40.00", "0",     "2000", 1500 },
};
static const as11_sig_t AS11_PLD[] = {
    { "MaskPress.2s", "cmH2O", "0.00", "40.00", "0", "2000", 30 },
    { "Press.2s",     "cmH2O", "0.00", "50.00", "0", "2500", 30 },
    { "EprPress.2s",  "cmH2O", "0.00", "30.00", "0", "1500", 30 },
    { "Leak.2s",      "L/s",   "0.00", "2.00",  "0", "100",  30 },
    { "RespRate.2s",  "bpm",   "0.00", "90.00", "0", "450",  30 },
    { "TidVol.2s",    "L",     "0.00", "4.00",  "0", "200",  30 },
    { "MinVent.2s",   "L/min", "0.00", "30.00", "0", "240",  30 },
    { "Snore.2s",     "",      "0.00", "5.00",  "0", "250",  30 },
    { "FlowLim.2s",   "",      "0.00", "1.00",  "0", "100",  30 },
};

static void check_signal_table(const char *what, const uint8_t *b, const edf_t *e,
                               const as11_sig_t *want, int n)
{
    CHECK(e->ns == n + 1, "%s: %d signals, the AS11 writes %d (%d + Crc16)",
          what, e->ns, n + 1, n);
    if (e->ns != n + 1) return;
    char got[24];
    for (int i = 0; i < n; i++) {
        EDF_LABEL(b, e, i, got);
        CHECK(strcmp(got, want[i].label) == 0,
              "%s signal %d: label \"%s\", the AS11 writes \"%s\"", what, i, got, want[i].label);
        EDF_UNIT(b, e, i, got);
        CHECK(strcmp(got, want[i].unit) == 0,
              "%s %s: unit \"%s\", the AS11 writes \"%s\"", what, want[i].label, got, want[i].unit);
        edf_field(b, e, 104, 8, i, got);
        CHECK(strcmp(got, want[i].pmin) == 0,
              "%s %s: phys_min %s, the AS11 writes %s", what, want[i].label, got, want[i].pmin);
        edf_field(b, e, 112, 8, i, got);
        CHECK(strcmp(got, want[i].pmax) == 0,
              "%s %s: phys_max %s, the AS11 writes %s", what, want[i].label, got, want[i].pmax);
        EDF_DIGMIN(b, e, i, got);
        CHECK(strcmp(got, want[i].dmin) == 0,
              "%s %s: dig_min %s, the AS11 writes %s", what, want[i].label, got, want[i].dmin);
        EDF_DIGMAX(b, e, i, got);
        CHECK(strcmp(got, want[i].dmax) == 0,
              "%s %s: dig_max %s, the AS11 writes %s", what, want[i].label, got, want[i].dmax);
        CHECK(e->spr[i] == want[i].spr,
              "%s %s: %d samples/record, the AS11 writes %d",
              what, want[i].label, e->spr[i], want[i].spr);
    }
    EDF_LABEL(b, e, n, got);
    CHECK(strcmp(got, "Crc16") == 0, "%s: last signal is \"%s\", want \"Crc16\"", what, got);
    CHECK(e->spr[n] == 1, "%s: Crc16 has %d samples/record, want 1", what, e->spr[n]);
}

static void test_signal_tables_match_the_as11(void)
{
    set_tz("UTC");
    char root[300], sd[400], out[400], p[600];
    snprintf(root, sizeof(root), "%s/sigtable", g_root);
    build_session(root, 2, SA2_N, sd, sizeof(sd));
    CHECK(generate(root, sd, out, sizeof(out)) == ESP_OK, "generate failed");

    static const struct {
        const char *kind;
        const as11_sig_t *sigs;
        int n;
    } streams[] = {
        { "SA2", AS11_SA2, 2 },
        { "BRP", AS11_BRP, 2 },
        { "PLD", AS11_PLD, 9 },
    };
    for (size_t k = 0; k < sizeof(streams) / sizeof(streams[0]); k++) {
        snprintf(p, sizeof(p), "%s/DATALOG/20260301/%s_%s.edf", out, SID, streams[k].kind);
        size_t n = 0;
        uint8_t *b = read_file(p, &n);
        CHECK(b != NULL, "%s.edf not written", streams[k].kind);
        if (!b) continue;
        edf_t e;
        if (edf_parse(b, n, &e)) {
            check_signal_table(streams[k].kind, b, &e, streams[k].sigs, streams[k].n);
            char dur[16];
            memcpy(dur, b + 244, 8); dur[8] = '\0';
            for (int i = 7; i >= 0 && dur[i] == ' '; i--) dur[i] = '\0';
            CHECK(strcmp(dur, "60.00") == 0,
                  "%s: record duration \"%s\", the AS11 writes \"60.00\"", streams[k].kind, dur);
        } else {
            CHECK(false, "%s.edf header/size inconsistent", streams[k].kind);
        }
        free(b);
    }
}

/* A crash can tear a file in the middle of a sample, leaving a byte count that
 * is not a whole number of frames.  Those trailing bytes are not a sample, and
 * rounding them up promises a record the file cannot fill. */
static void test_torn_mid_sample_rounds_down(void)
{
    set_tz("UTC");
    char root[300], sd[400], out[400], p[600];
    snprintf(root, sizeof(root), "%s/torn_frag", g_root);
    build_session(root, 2, SA2_N, sd, sizeof(sd));
    static int16_t sa2[SA2_N * 2];
    for (int i = 0; i < SA2_N; i++) {
        sa2[2 * i]     = sa2_hr_missing(i)   ? INT16_MIN : (int16_t)(sa2_hr_bpm(i) * 100);
        sa2[2 * i + 1] = sa2_spo2_missing(i) ? INT16_MIN : (int16_t)(sa2_spo2_pct(i) * 100);
    }
    snprintf(p, sizeof(p), "%s/%s_sa2.snt", sd, SID);
    write_snt(p, 2, 2, 10, START_MS, SA2_N, sa2, 119);
    FILE *f = fopen(p, "ab");                    /* 119 frames + 3 stray bytes */
    CHECK(f != NULL, "cannot reopen sa2.snt");
    if (f) { fwrite("\0\0\0", 1, 3, f); fclose(f); }

    CHECK(generate(root, sd, out, sizeof(out)) == ESP_OK, "generate failed");
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/%s_SA2.edf", out, SID);
    size_t n = 0;
    uint8_t *b = read_file(p, &n);
    CHECK(b != NULL, "SA2.edf not written");
    if (b) {
        edf_t e;
        if (edf_parse(b, n, &e))
            CHECK(e.ndr == 1, "records: got %d want 1 — 3 stray bytes are not a 120th sample", e.ndr);
        else
            CHECK(false, "SA2.edf header/size inconsistent: a record was promised but not written");
        free(b);
    }
}

/* A sample too large to represent must saturate at the digital maximum the
 * header itself declares — never wrap to the other end of the range, which
 * would turn a sensor spike into a maximal negative flow. */
static void test_overflow_clamps_to_declared_digital_max(void)
{
    set_tz("UTC");
    char root[300], sd[400], out[400], p[600];
    snprintf(root, sizeof(root), "%s/clamp", g_root);
    build_session(root, 2, SA2_N, sd, sizeof(sd));
    CHECK(generate(root, sd, out, sizeof(out)) == ESP_OK, "generate failed");
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/%s_BRP.edf", out, SID);
    size_t n = 0;
    uint8_t *b = read_file(p, &n);
    CHECK(b != NULL, "BRP.edf not written");
    if (!b) return;
    edf_t e;
    if (edf_parse(b, n, &e)) {
        char dmax[16];
        EDF_DIGMAX(b, &e, 0, dmax);
        int16_t got = edf_sample(b, &e, 0, 0, FLOW_OVERFLOW_IDX);
        CHECK(got == (int16_t)atoi(dmax),
              "overflowing flow sample exported as %d; the header declares the "
              "digital maximum is %s", got, dmax);
    } else {
        CHECK(false, "BRP.edf header/size inconsistent");
    }
    free(b);
}

/* The export must also STOP at MaskOff: therapy that ended at 01:10 must not
 * carry a third minute of whatever the sensors were doing afterwards.  Mask on
 * at +10 s, off at +70 s ⇒ a 60 s window ⇒ exactly one record, out of a
 * 3-minute recording. */
static void test_maskoff_truncates_the_export(void)
{
    set_tz("UTC");
    char rootg[300], sdg[400], outg[400];
    char root0[300], sd0[400], out0[400], p[600];
    snprintf(rootg, sizeof(rootg), "%s/maskoff", g_root);
    snprintf(root0, sizeof(root0), "%s/maskoff_ref", g_root);
    build_session(rootg, 2, SA2_N, sdg, sizeof(sdg));
    build_session(root0, 2, SA2_N, sd0, sizeof(sd0));
    write_events(sdg, 10000, 70000);

    CHECK(generate(rootg, sdg, outg, sizeof(outg)) == ESP_OK, "gated generate failed");
    CHECK(generate(root0, sd0, out0, sizeof(out0)) == ESP_OK, "reference generate failed");

    snprintf(p, sizeof(p), "%s/DATALOG/20260301/20260301_220010_BRP.edf", outg);
    size_t ng = 0, n0 = 0;
    uint8_t *bg = read_file(p, &ng);
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/%s_BRP.edf", out0, SID);
    uint8_t *b0 = read_file(p, &n0);
    CHECK(bg && b0, "BRP.edf missing (gated=%p reference=%p)", (void *)bg, (void *)b0);
    if (bg && b0) {
        edf_t eg, eu;
        if (edf_parse(bg, ng, &eg) && edf_parse(b0, n0, &eu)) {
            CHECK(eg.ndr == 1,
                  "records: got %d want 1 — a 60 s mask-on window is one record, "
                  "the export did not stop at MaskOff", eg.ndr);
            CHECK(eu.ndr == 3, "reference records: got %d want 3", eu.ndr);
            /* And the record it does contain is the right 60 seconds. */
            for (int i = 0; i < eg.spr[0] && eg.ndr >= 1; i++) {
                int j = i + 250;                     /* 10 s at 25 Hz */
                int16_t g = edf_sample(bg, &eg, 0, 0, i);
                int16_t u = edf_sample(b0, &eu, j / eu.spr[0], 0, j % eu.spr[0]);
                if (g != u) {
                    CHECK(false, "flow sample %d: gated %d != ungated sample %d (%d)", i, g, j, u);
                    break;
                }
            }
        }
    }
    free(bg); free(b0);
}

/* Imported sessions carry "pre_gated":true in their manifest — the .snt
 * streams already start at MaskOn and end at MaskOff, so neither the
 * MaskOn skip nor the MaskOff end truncation may touch the export.
 * Without the guard a MaskOff at +70 s would cut the 3-minute wave back
 * to one record (the failure the IMPORT-EDFs spec calls out). */
static void test_pre_gated_session_is_not_truncated(void)
{
    set_tz("UTC");
    char root[300], sd[400], out[400], p[600];
    snprintf(root, sizeof(root), "%s/pregated", g_root);
    build_session(root, 2, SA2_N, sd, sizeof(sd));
    write_events(sd, 10000, 70000);
    snprintf(p, sizeof(p), "%s/%s_session.json", sd, SID);
    write_text(p, "{\"id\":\"" SID "\",\"pre_gated\":true,"
                  "\"waveform_offset_ms\":0,"
                  "\"brp_samples\":4500,\"sa2_samples\":180,"
                  "\"pld_samples\":90}\n");

    CHECK(generate(root, sd, out, sizeof(out)) == ESP_OK, "generate failed");
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/%s_BRP.edf", out, SID);
    size_t n = 0;
    uint8_t *b = read_file(p, &n);
    CHECK(b != NULL, "BRP.edf not written");
    if (b) {
        edf_t e;
        if (edf_parse(b, n, &e))
            CHECK(e.ndr == 3,
                  "records: got %d want 3 — pre_gated streams must not be "
                  "end-truncated at MaskOff", e.ndr);
        else
            CHECK(false, "BRP.edf header/size inconsistent");
        free(b);
    }
}

/* The importer clusters wave files within ±2 s of the EVE anchor, so a
 * manifest's waveform_offset_ms can legitimately be a small NEGATIVE
 * number — the BRP must keep that exact start (filename + starttime),
 * not snap forward to the TherapyStart anchor. */
static void test_pre_gated_negative_waveform_offset(void)
{
    set_tz("UTC");
    char root[300], sd[400], out[400], p[600];
    snprintf(root, sizeof(root), "%s/pregated_neg", g_root);
    build_session(root, 2, SA2_N, sd, sizeof(sd));
    snprintf(p, sizeof(p), "%s/%s_session.json", sd, SID);
    write_text(p, "{\"id\":\"" SID "\",\"pre_gated\":true,"
                  "\"waveform_offset_ms\":-2000,"
                  "\"brp_samples\":4500,\"sa2_samples\":180,"
                  "\"pld_samples\":90}\n");

    CHECK(generate(root, sd, out, sizeof(out)) == ESP_OK, "generate failed");
    /* maskon = START_MS - 2 s → 2026-03-01 21:59:58 */
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/20260301_215958_BRP.edf", out);
    size_t n = 0;
    uint8_t *b = read_file(p, &n);
    CHECK(b != NULL, "BRP.edf missing at the -2 s offset name");
    if (b) {
        char t[16];
        memcpy(t, b + 176, 8); t[8] = 0;
        CHECK(!strcmp(t, "21.59.58"), "EDF starttime %s, want 21.59.58", t);
        edf_t e;
        if (edf_parse(b, n, &e))
            CHECK(e.ndr == 3, "records: got %d want 3", e.ndr);
        else
            CHECK(false, "BRP.edf header/size inconsistent");
        free(b);
    }
}

/* The _ZLE rising edge is the primary gate, and its timestamp is in the AS11
 * clock: the exporter must ADD the measured drift to reach NTP time.  With
 * _ZLE at AS11 +7 s and a drift of +3 s, the gate is NTP +10 s — the same
 * instant as the MaskOn case, so the ungated export is again the oracle and
 * no expected sample value appears here either.
 *
 * The night also contains a mask-off and a second mask-on, because that is
 * what a real one looks like: data starts at the FIRST rising edge, not the
 * last one in the file. */
static void test_zle_gate_applies_clock_drift(void)
{
    set_tz("UTC");
    char rootg[300], sdg[400], outg[400];
    char root0[300], sd0[400], out0[400], p[600];
    snprintf(rootg, sizeof(rootg), "%s/zle", g_root);
    snprintf(root0, sizeof(root0), "%s/zle_ref", g_root);
    build_session(rootg, 2, SA2_N, sdg, sizeof(sdg));
    build_session(root0, 2, SA2_N, sd0, sizeof(sd0));
    /* AS11 clock: rising at +7 s (NTP +10 s), falling at +117 s — the falling
     * edge ends the export the way MaskOff does — then a second rising edge
     * that must NOT be the one the export starts from. */
    static const int64_t zle_at[] = { 7000, 117000, 147000 };
    static const int     zle_v[]  = {    1,      0,      1 };
    write_zle_sequence(sdg, zle_at, zle_v, 3);

    CHECK(generate_drift(rootg, sdg, outg, sizeof(outg), 3000) == ESP_OK,
          "gated generate failed");
    CHECK(generate(root0, sd0, out0, sizeof(out0)) == ESP_OK, "reference generate failed");

    snprintf(p, sizeof(p), "%s/DATALOG/20260301/20260301_220010_BRP.edf", outg);
    size_t ng = 0, n0 = 0;
    uint8_t *bg = read_file(p, &ng);
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/%s_BRP.edf", out0, SID);
    uint8_t *b0 = read_file(p, &n0);
    CHECK(bg != NULL, "no BRP.edf at the drift-corrected _ZLE time (22:00:10)");
    CHECK(b0 != NULL, "reference BRP.edf missing");
    if (bg && b0) {
        edf_t eg, eu;
        if (edf_parse(bg, ng, &eg) && edf_parse(b0, n0, &eu)) {
            for (int i = 0; i < eg.spr[0]; i++) {
                int j = i + 250;                    /* 10 s at 25 Hz */
                int16_t g = edf_sample(bg, &eg, 0, 0, i);
                int16_t u = edf_sample(b0, &eu, j / eu.spr[0], 0, j % eu.spr[0]);
                if (g != u) {
                    CHECK(false, "flow sample %d: gated %d != ungated sample %d (%d)", i, g, j, u);
                    break;
                }
            }
        }
    }
    free(bg); free(b0);
}

/* The offset the AS11 reports for the session is what defines its local day.
 * It must win over whatever timezone this device happens to be set to —
 * that is the whole point of carrying it (#183). */
static void test_as11_offset_beats_device_timezone(void)
{
    set_tz("UTC");
    /* 2026-03-01 23:00 UTC.  In UTC that is the afternoon of the 1st; at
     * +13:00 it is noon on the 2nd, and noon starts a new DATALOG day. */
    const int64_t t_ms = 1772406000000LL;
    char with_offset[16], utc_day[16];

    as11_time_set_offset((as11_offset_t)(13 * 3600), "test");
    as11_time_noon_day(t_ms, with_offset, sizeof(with_offset));
    as11_time_set_offset((as11_offset_t)0, "test");
    as11_time_noon_day(t_ms, utc_day, sizeof(utc_day));

    CHECK(strcmp(utc_day, "20260301") == 0,
          "at +00:00 the day label is %s, want 20260301", utc_day);
    CHECK(strcmp(with_offset, "20260302") == 0,
          "at +13:00 the day label is %s, want 20260302 — the session's own "
          "offset was ignored in favour of the device timezone", with_offset);
}

/* PLD is exported through a channel map, so a file with fewer channels than
 * the map indexes must be refused — reading channel 10 of a 10-channel record
 * is a read past the end of every frame. */
static void test_pld_channel_map_out_of_range_is_refused(void)
{
    set_tz("UTC");
    char root[300], sd[400], out[400], p[600];
    snprintf(root, sizeof(root), "%s/pldshort", g_root);
    build_session(root, 2, SA2_N, sd, sizeof(sd));
    static int16_t pld10[PLD_N * 10];
    for (int i = 0; i < PLD_N * 10; i++) pld10[i] = (int16_t)(i % 50);
    snprintf(p, sizeof(p), "%s/%s_pld.snt", sd, SID);
    write_snt(p, 2, 10, 5, START_MS, PLD_N, pld10, PLD_N);   /* map needs index 10 */

    generate(root, sd, out, sizeof(out));
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/%s_PLD.edf", out, SID);
    size_t n = 0;
    uint8_t *b = read_file(p, &n);
    CHECK(b == NULL, "PLD.edf was written from a 10-channel .snt whose map indexes channel 10");
    free(b);
}

/* A file whose header disagrees with the signal set it is being exported as
 * must be refused, not exported with the channels silently misaligned.  (A
 * firmware that adds a channel produces exactly this file.) */
/* The MaskOn fallback runs whenever _ZLE is absent - older firmware, a failed
 * subscription, a night the AS11 never gated.  Its timestamp is in the AS11
 * clock just like _ZLE's, so it needs the same correction, but until now only
 * the _ZLE path had a test that could tell the two directions apart.  Adding
 * the drift where it should be subtracted moves the gate the wrong way by
 * twice the drift, and every existing test still passed.
 *
 * MaskOn at AS11 +7 s with a +3 s drift gates at NTP +10 s.  Reversing the
 * sign would gate at +4 s, so the wrong instant is named explicitly and
 * asserted absent: a test that only checks the right file exists would still
 * pass if the exporter wrote both. */
static void test_maskon_fallback_adds_the_clock_drift(void)
{
    set_tz("UTC");
    char rootg[300], sdg[400], outg[400];
    char root0[300], sd0[400], out0[400], p[600];
    snprintf(rootg, sizeof(rootg), "%s/mon_drift", g_root);
    snprintf(root0, sizeof(root0), "%s/mon_drift_ref", g_root);
    build_session(rootg, 2, SA2_N, sdg, sizeof(sdg));
    build_session(root0, 2, SA2_N, sd0, sizeof(sd0));
    write_events(sdg, 7000, -1);            /* AS11 +7 s, no MaskOff */

    CHECK(generate_drift(rootg, sdg, outg, sizeof(outg), 3000) == ESP_OK,
          "gated generate failed");
    CHECK(generate(root0, sd0, out0, sizeof(out0)) == ESP_OK, "reference generate failed");

    size_t ng = 0, n0 = 0, nwrong = 0;
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/20260301_220010_BRP.edf", outg);
    uint8_t *bg = read_file(p, &ng);
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/20260301_220004_BRP.edf", outg);
    uint8_t *bwrong = read_file(p, &nwrong);
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/%s_BRP.edf", out0, SID);
    uint8_t *b0 = read_file(p, &n0);

    CHECK(bg != NULL, "no BRP.edf at the drift-corrected MaskOn time (22:00:10)");
    CHECK(bwrong == NULL,
          "an export exists at 22:00:04 - the drift was SUBTRACTED from the "
          "AS11 MaskOn time instead of added");
    CHECK(b0 != NULL, "reference BRP.edf missing");

    /* The ungated export is the oracle, so no expected sample value is pinned
     * here: whatever the reference holds 10 s in is what the gated one must
     * start with. */
    if (bg && b0) {
        edf_t eg, eu;
        if (edf_parse(bg, ng, &eg) && edf_parse(b0, n0, &eu)) {
            for (int i = 0; i < eg.spr[0]; i++) {
                int j = i + 250;                     /* 10 s at 25 Hz */
                int16_t g = edf_sample(bg, &eg, 0, 0, i);
                int16_t u = edf_sample(b0, &eu, j / eu.spr[0], 0, j % eu.spr[0]);
                if (g != u) {
                    CHECK(false, "flow sample %d: gated %d != ungated sample %d (%d)",
                          i, g, j, u);
                    break;
                }
            }
        }
    }
    free(bg); free(b0); free(bwrong);
}

/* MaskOff ends the export, and its timestamp needs the same AS11->NTP
 * correction as MaskOn.  Reversing it truncates the night early while leaving
 * the start untouched, which no filename reveals - so this one is observed
 * through the record count instead.
 *
 * The 30 s drift is chosen so each possible error has its own signature rather
 * than because it is typical: with MaskOn at AS11 +30 s and MaskOff at +150 s,
 * the correct export runs NTP +60 s to +180 s, which is two records.  Reverse
 * the MaskOff sign and it ends at +120 s, giving one.  Reverse the MaskOn sign
 * instead and the gate lands on the session start, is rejected as out of
 * range, and the export is never gated at all - a different filename. */
static void test_maskoff_end_adds_the_clock_drift(void)
{
    set_tz("UTC");
    char rootg[300], sdg[400], outg[400], p[600];
    snprintf(rootg, sizeof(rootg), "%s/moff_drift", g_root);
    build_session(rootg, 2, SA2_N, sdg, sizeof(sdg));
    write_events(sdg, 30000, 150000);       /* AS11 +30 s on, +150 s off */

    CHECK(generate_drift(rootg, sdg, outg, sizeof(outg), 30000) == ESP_OK,
          "gated generate failed");

    size_t ng = 0, nungated = 0;
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/20260301_220100_BRP.edf", outg);
    uint8_t *bg = read_file(p, &ng);
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/%s_BRP.edf", outg, SID);
    uint8_t *bungated = read_file(p, &nungated);

    CHECK(bg != NULL, "no BRP.edf at the drift-corrected MaskOn time (22:01:00)");
    CHECK(bungated == NULL,
          "an ungated export exists at %s - the drift was subtracted from the "
          "AS11 MaskOn time, putting the gate outside the session", SID);
    if (bg) {
        edf_t eg;
        if (edf_parse(bg, ng, &eg)) {
            CHECK(eg.ndr == 2,
                  "records: got %d want 2 - NTP +60 s to +180 s is two 60 s "
                  "records; one means MaskOff ended the export 2x the drift early",
                  eg.ndr);
        }
    }
    free(bg); free(bungated);
}

/* The entry guard refuses a call missing any ONE of its three required
 * pointers.  Replacing || with && would demand all three be absent before
 * refusing, so a single NULL walked straight into the body.  Nothing tested
 * it, because every existing caller passes all three. */
static void test_a_single_null_argument_is_refused(void)
{
    set_tz("UTC");
    char root[300], sd[400], out[400];
    snprintf(root, sizeof(root), "%s/nullargs", g_root);
    build_session(root, 2, SA2_N, sd, sizeof(sd));
    snprintf(out, sizeof(out), "%s/out", root);
    const int64_t end = START_MS + SA2_N * 1000LL;

    CHECK(edf_gen_generate_ex(NULL, sd, SID, START_MS, end, 0, EDF_GEN_PER_SESSION)
              == ESP_ERR_INVALID_ARG, "a NULL out_root must be refused on its own");
    CHECK(edf_gen_generate_ex(out, NULL, SID, START_MS, end, 0, EDF_GEN_PER_SESSION)
              == ESP_ERR_INVALID_ARG, "a NULL session_dir must be refused on its own");
    CHECK(edf_gen_generate_ex(out, sd, NULL, START_MS, end, 0, EDF_GEN_PER_SESSION)
              == ESP_ERR_INVALID_ARG, "a NULL session_id must be refused on its own");
}

/* The timestamp guard exists because crash recovery on a 0-byte .snt yields a
 * start near the epoch and a 19691231 folder.  Its bound is EXCLUSIVE: the
 * first instant of 2000 is a legitimate start, not a corrupt one.  Only a
 * start exactly on the bound separates < from <=, and no test used one. */
static void test_the_year_2000_guard_excludes_only_what_precedes_it(void)
{
    set_tz("UTC");
    char rootA[300], sdA[400], outA[400];
    char rootB[300], sdB[400], outB[400];
    snprintf(rootA, sizeof(rootA), "%s/y2k_before", g_root);
    snprintf(rootB, sizeof(rootB), "%s/y2k_on", g_root);
    build_session(rootA, 2, SA2_N, sdA, sizeof(sdA));
    build_session(rootB, 2, SA2_N, sdB, sizeof(sdB));
    snprintf(outA, sizeof(outA), "%s/out", rootA);
    snprintf(outB, sizeof(outB), "%s/out", rootB);

    const int64_t Y2K = 946684800000LL;          /* 2000-01-01T00:00:00Z */
    const int64_t dur = SA2_N * 1000LL;

    CHECK(edf_gen_generate_ex(outA, sdA, SID, Y2K - 1, Y2K - 1 + dur, 0,
                              EDF_GEN_PER_SESSION) == ESP_ERR_INVALID_ARG,
          "one millisecond before 2000 is a corrupt start and must be refused");
    /* Asserted as "not refused for THIS reason" rather than ESP_OK: whether a
     * Y2K session exports cleanly is a different question from whether the
     * guard lets it through. */
    CHECK(edf_gen_generate_ex(outB, sdB, SID, Y2K, Y2K + dur, 0,
                              EDF_GEN_PER_SESSION) != ESP_ERR_INVALID_ARG,
          "midnight on 2000-01-01 is a valid start and must not hit the guard");
}

/* The DATALOG day folder is a noon-to-noon day in the AS11's clock, not the
 * host's, so the drift is SUBTRACTED to get there — the opposite direction
 * from every other use of it in this file, which is exactly why it is worth
 * pinning.  Adding it instead shifts the instant by twice the drift.
 *
 * Only a drift large enough to carry the session across a noon boundary can
 * show which direction was taken, so the 12 h here is chosen for observability
 * rather than realism: the session starts at 22:00Z, so subtracting lands at
 * 10:00 on the same date (the noon-day that BEGAN the previous midday) while
 * adding lands at 10:00 the next date (the following noon-day). */
static void test_the_day_folder_is_computed_in_the_as11_clock(void)
{
    set_tz("UTC");
    char root[300], sd[400], out[400], p[600];
    struct stat st;
    snprintf(root, sizeof(root), "%s/dayfolder", g_root);
    build_session(root, 2, SA2_N, sd, sizeof(sd));

    CHECK(generate_drift(root, sd, out, sizeof(out), 12 * 3600 * 1000LL) == ESP_OK,
          "generate with a 12 h drift failed");

    snprintf(p, sizeof(p), "%s/DATALOG/20260228", out);
    CHECK(stat(p, &st) == 0,
          "no DATALOG/20260228 — the drift was not subtracted, so the folder "
          "was computed in the host clock instead of the AS11's");
    snprintf(p, sizeof(p), "%s/DATALOG/20260301", out);
    CHECK(stat(p, &st) != 0,
          "DATALOG/20260301 exists — the drift was ADDED, putting the session "
          "a whole noon-day late");
}

static void test_channel_count_mismatch_is_refused(void)
{
    set_tz("UTC");
    char root[300], sd[400], out[400], p[600];
    snprintf(root, sizeof(root), "%s/chmismatch", g_root);
    build_session(root, 2, SA2_N, sd, sizeof(sd));
    static int16_t three[SA2_N * 3];
    for (int i = 0; i < SA2_N * 3; i++) three[i] = (int16_t)i;
    snprintf(p, sizeof(p), "%s/%s_sa2.snt", sd, SID);
    write_snt(p, 2, 3, 10, START_MS, SA2_N, three, SA2_N);   /* 3 channels, not 2 */

    generate(root, sd, out, sizeof(out));
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/%s_SA2.edf", out, SID);
    size_t n = 0;
    uint8_t *b = read_file(p, &n);
    CHECK(b == NULL, "SA2.edf was written from a 3-channel .snt exported as 2 signals");
    free(b);
}

/* Format contract: v1 marks missing data with -1, v2 with INT16_MIN. */
static void test_snt_missing_sentinel_per_version(void)
{
    CHECK(snt_missing_for(1) == -1, "v1 sentinel");
    CHECK(snt_missing_for(2) == INT16_MIN, "v2 sentinel");
    CHECK(snt_missing_for(3) == INT16_MIN, "future versions keep the v2 sentinel");
}

/* Summary spool "no data" is -1 and must survive the logical-scale division. */
static void test_spool_to_edf(void)
{
    CHECK(spool_to_edf(-1, 1, 50) == -1, "sentinel scaled away");
    CHECK(spool_to_edf(100, 1, 50) == 2, "100/50");
    CHECK(spool_to_edf(7, 1, 1) == 7, "identity scale");
}

/* AS11 noon-day: noon itself belongs to the NEW day, 11:59:59 to the old. Since v2.0.0
 * edf_gen derives the DATALOG day through as11_time_noon_day() (71e2e64), so that is the
 * function under test; the offset is pinned so the label is a pure function of the epoch. */
static void test_noon_day_folder_boundary(void)
{
    as11_time_set_offset(0, "test");
    char d[16];
    as11_time_noon_day(1772366399000LL, d, sizeof(d));   /* 2026-03-01 11:59:59Z */
    CHECK(strcmp(d, "20260228") == 0, "11:59:59 → %s want 20260228", d);
    as11_time_noon_day(1772366400000LL, d, sizeof(d));   /* 2026-03-01 12:00:00Z */
    CHECK(strcmp(d, "20260301") == 0, "12:00:00 → %s want 20260301", d);
    as11_time_noon_day(1772409599000LL, d, sizeof(d));   /* 2026-03-01 23:59:59Z */
    CHECK(strcmp(d, "20260301") == 0, "23:59:59 → %s want 20260301", d);
}

/* STR settings: the AS11 stores every pressure at 50 digits per cmH2O — its
 * own STR.edf declares phys[4,20] against dig[200,1000] for the setting
 * signals — and a therapy pressure moves in 0.2 cmH2O steps.  So a settings
 * pressure is always an exact multiple of TEN digits, and a value that is not
 * one is a value the machine itself would never write.
 *
 * That grid is the assertion.  It is not arithmetic done here: it is what
 * every S.C./S.A./S.AFH. pressure in a real STR.edf does. */
#define STR_DIGITS_PER_CMH2O   50
#define STR_STEP_DIGITS        10     /* 0.2 cmH2O */

static void test_str_pressure_settings_exact(void)
{
    const char *json =
        "{\"SettingProfiles\":{\"TherapyProfiles\":{"
        "\"CpapProfile\":{\"StartPressure\":4.6,\"SetPressure\":8.2},"
        "\"AutoSetProfile\":{\"StartPressure\":9.2,\"MaxPressure\":16.4,\"MinPressure\":10.2}"
        "}}}";
    cJSON *settings = cJSON_Parse(json);
    CHECK(settings != NULL, "test JSON did not parse");
    if (!settings) return;
    summary_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    int16_t v[STR_DATA_COUNT];
    memset(v, 0xFF, sizeof(v));
    build_str_data_values(&ctx, v, settings);
    struct { int idx; double cmh2o; } want[] = {
        { 6, 4.6 }, { 7, 8.2 }, { 8, 9.2 }, { 9, 16.4 }, { 10, 10.2 },
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        int got = v[want[i].idx];
        int exact = (int)lrint(want[i].cmh2o * STR_DIGITS_PER_CMH2O);
        CHECK(got % STR_STEP_DIGITS == 0,
              "STR[%d] %.1f cmH2O stored as %d, which is not a multiple of %d "
              "— off the AS11's own 0.2 cmH2O grid (it would store %d)",
              want[i].idx, want[i].cmh2o, got, STR_STEP_DIGITS, exact);
        CHECK(got == exact, "STR[%d] %.1f cmH2O: got %d want %d",
              want[i].idx, want[i].cmh2o, got, exact);
    }
    cJSON_Delete(settings);
}

static void test_spont_settings_strings_and_numeric(void)
{
    const char *json =
        "{\"SettingProfiles\":{\"TherapyProfiles\":{\"SpontProfile\":{"
        "\"SetMaxInspiratoryTime\":\"PT1.2S\","
        "\"SetMinInspiratoryTime\":\"PT0.4S\","
        "\"RiseTime\":\"PT0.25S\","
        "\"TriggerSensitivity\":\"Low\",\"CycleSensitivity\":\"High\""
        "}}}}";
    cJSON *settings = cJSON_Parse(json);
    CHECK(settings != NULL, "Spont JSON did not parse");
    if (!settings) return;
    summary_ctx_t ctx = {0};
    int16_t values[STR_DATA_COUNT];
    memset(values, 0xFF, sizeof(values));
    build_str_data_values(&ctx, values, settings);
    CHECK(values[27] == 60 && values[28] == 20, "duration strings not scaled to Ti");
    CHECK(values[30] == 250, "rise-time string not converted to milliseconds");
    CHECK(values[31] == 2 && values[32] == 4, "named sensitivities mapped incorrectly");
    cJSON_Delete(settings);

    settings = cJSON_Parse(
        "{\"SettingProfiles\":{\"TherapyProfiles\":{\"SpontProfile\":{"
        "\"SetMaxInspiratoryTime\":1.2,\"SetMinInspiratoryTime\":0.4,"
        "\"RiseTime\":250,\"TriggerSensitivity\":1,\"CycleSensitivity\":3"
        "}}}}");
    CHECK(settings != NULL, "numeric Spont JSON did not parse");
    if (!settings) return;
    memset(values, 0xFF, sizeof(values));
    build_str_data_values(&ctx, values, settings);
    CHECK(values[27] == 60 && values[28] == 20 && values[30] == 250 &&
          values[31] == 2 && values[32] == 4,
          "imported numeric settings changed");
    cJSON_Delete(settings);

    const struct { const char *name; int expected; } names[] = {
        {"VeryLow", 1}, {"Very Low", 1}, {"Low", 2}, {"Medium", 3},
        {"Med", 3}, {"High", 4}, {"VeryHigh", 5}, {"Very High", 5},
        {"unknown", -1}, {"", -1},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        cJSON *v = cJSON_CreateString(names[i].name);
        CHECK(sensitivity_to_edf(v) == names[i].expected,
              "sensitivity %s", names[i].name);
        cJSON_Delete(v);
    }
    const char *invalid[] = {
        "", "P", "PT", "PT.S", "PT1.S", "PT-1S", "PT1e0S",
        "PT1Sx", "PT4.01S", "PT999999999999999999999999999999999S",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        cJSON *v = cJSON_CreateString(invalid[i]);
        CHECK(duration_string_to_edf(v, 50.0, 200) == -1,
              "accepted invalid Ti duration %s", invalid[i]);
        cJSON_Delete(v);
    }
}

static void test_pld_snore_missing_and_clamping(void)
{
    set_tz("UTC");
    for (int version = 1; version <= 2; version++) {
        char root[300], sd[400], out[400], path[600];
        snprintf(root, sizeof(root), "%s/snore_v%d", g_root, version);
        build_session(root, version, SA2_N, sd, sizeof(sd));
        int16_t frames[PLD_N * 12] = {0};
        frames[9] = version == 1 ? SNT_MISSING_V1 : SNT_MISSING_V2;
        frames[12 + 9] = 0;
        frames[24 + 9] = 100;
        frames[36 + 9] = 2000;
        frames[10] = version == 1 ? SNT_MISSING_V1 : SNT_MISSING_V2;
        snprintf(path, sizeof(path), "%s/%s_pld.snt", sd, SID);
        write_snt(path, (uint8_t)version, 12, 5, START_MS, PLD_N, frames, PLD_N);
        CHECK(generate(root, sd, out, sizeof(out)) == ESP_OK, "generate PLD v%d", version);
        snprintf(path, sizeof(path), "%s/DATALOG/20260301/%s_PLD.edf", out, SID);
        size_t len = 0;
        uint8_t *bytes = read_file(path, &len);
        CHECK(bytes != NULL, "missing PLD v%d", version);
        if (!bytes) continue;
        edf_t edf;
        if (edf_parse(bytes, len, &edf)) {
            CHECK(edf_sample(bytes, &edf, 0, 7, 0) == -1, "missing snore v%d", version);
            CHECK(edf_sample(bytes, &edf, 0, 7, 1) == 0, "zero snore v%d", version);
            CHECK(edf_sample(bytes, &edf, 0, 7, 2) == 50, "measured snore v%d", version);
            CHECK(edf_sample(bytes, &edf, 0, 7, 3) == 250, "snore clamp v%d", version);
            CHECK(edf_sample(bytes, &edf, 0, 8, 0) == -1, "missing flow limit v%d", version);
            check_edf_structure("PLD.edf", bytes, len, &edf);
        } else {
            CHECK(false, "invalid PLD EDF v%d", version);
        }
        free(bytes);
    }
}

static void write_live_ident(const char *path, const char *day, const char *sid)
{
    cJSON *ident = cJSON_Parse("{\"SerialNumber\":\"fixture-machine\",\"PlatformIdentifier\":46}");
    CHECK(ident && settings_provenance_set(ident, SETTINGS_SOURCE_LIVE_IDENT, day, sid),
          "mark live identity");
    if (ident) {
        char *json = cJSON_PrintUnformatted(ident);
        if (json) { write_text(path, json); free(json); }
        cJSON_Delete(ident);
    }
}

static void test_daily_settings_provenance(void)
{
    const char *day = "20260301";
    const char *sid = SID;
    char dir[400], path[600];
    mk("%s", SD_MOUNT_POINT);
    mk("%s", SD_APP_DIR);
    mk("%s", SD_SESSIONS_DIR);
    mk("%s", SD_STREAMS_DIR);
    mk("%s", SD_SUMMARIES_DIR);
    snprintf(dir, sizeof(dir), "%s/%s", SD_STREAMS_DIR, day);
    mk("%s", dir);

    cJSON *settings = cJSON_Parse(
        "{\"SettingProfiles\":{\"ActiveProfiles\":{\"TherapyProfile\":\"CpapProfile\"},"
        "\"TherapyProfiles\":{\"CpapProfile\":{\"SetPressure\":8}}}}");
    CHECK(settings != NULL, "settings JSON");
    if (!settings) return;
    snprintf(path, sizeof(path), "%s/%s.settings.json", SD_SUMMARIES_DIR, day);
    char *json = cJSON_PrintUnformatted(settings);
    write_text(path, json);
    free(json);
    cJSON *read = read_daily_settings(day, SETTINGS_FAMILY_AS11, "fixture-machine");
    CHECK(read == NULL, "legacy day snapshot must remain unknown");
    cJSON_Delete(read);

    CHECK(settings_provenance_set(settings, SETTINGS_SOURCE_LIVE, day, sid), "mark live");
    snprintf(path, sizeof(path), "%s/%s_session.json", dir, sid);
    write_text(path, "{}");
    snprintf(path, sizeof(path), "%s/%s_ident.json", dir, sid);
    write_text(path, "{\"SerialNumber\":\"fixture-machine\"}");
    snprintf(path, sizeof(path), "%s/%s.live.settings.json", dir, sid);
    json = cJSON_PrintUnformatted(settings);
    write_text(path, json);
    snprintf(path, sizeof(path), "%s/%s.live.settings.json", SD_SUMMARIES_DIR, day);
    write_text(path, json);
    free(json);
    read = read_daily_settings(day, SETTINGS_FAMILY_AS11, "fixture-machine");
    CHECK(read == NULL, "unmarked or backfilled identity must not establish provenance");
    cJSON_Delete(read);
    snprintf(path, sizeof(path), "%s/%s_ident.json", dir, sid);
    write_live_ident(path, day, sid);
    read = read_daily_settings(day, SETTINGS_FAMILY_AS11, "fixture-machine");
    CHECK(read != NULL, "matching live session accepted");
    cJSON_Delete(read);
    read = read_daily_settings(day, SETTINGS_FAMILY_AS11, "different-machine");
    CHECK(read == NULL, "another machine must not use live settings");
    cJSON_Delete(read);
    read = read_daily_settings(day, SETTINGS_FAMILY_UNKNOWN, "fixture-machine");
    CHECK(read == NULL, "unknown machine family must not use live settings");
    cJSON_Delete(read);
    CHECK(!settings_provenance_is(settings, SETTINGS_SOURCE_LIVE, "20260228", sid),
          "wrong day accepted");
    CHECK(!settings_provenance_is(settings, SETTINGS_SOURCE_LIVE, day, "other"),
          "borrowed session accepted");

    snprintf(path, sizeof(path), "%s/other_session.json", dir);
    write_text(path, "{}");
    snprintf(path, sizeof(path), "%s/other_ident.json", dir);
    write_live_ident(path, day, "other");
    read = read_daily_settings(day, SETTINGS_FAMILY_AS11, "fixture-machine");
    CHECK(read == NULL, "day with an unverified second session accepted");
    cJSON_Delete(read);
    cJSON *other = cJSON_Duplicate(settings, true);
    CHECK(other && settings_provenance_set(other, SETTINGS_SOURCE_LIVE, day, "other"),
          "mark second live session");
    if (other) {
        cJSON *profiles = cJSON_GetObjectItem(other, "SettingProfiles");
        cJSON_AddStringToObject(profiles, "UnexportedMetadata", "changed");
        snprintf(path, sizeof(path), "%s/other.live.settings.json", dir);
        json = cJSON_PrintUnformatted(other);
        write_text(path, json);
        free(json);
        read = read_daily_settings(day, SETTINGS_FAMILY_AS11, "fixture-machine");
        CHECK(read != NULL, "irrelevant metadata must not invalidate equal EDF settings");
        cJSON_Delete(read);
        cJSON *cpap = cJSON_GetObjectItem(cJSON_GetObjectItem(profiles, "TherapyProfiles"),
                                          "CpapProfile");
        cJSON_ReplaceItemInObject(cpap, "SetPressure", cJSON_CreateNumber(9));
        json = cJSON_PrintUnformatted(other);
        write_text(path, json);
        free(json);
        read = read_daily_settings(day, SETTINGS_FAMILY_AS11, "fixture-machine");
        CHECK(read == NULL, "different session settings must be unknown");
        cJSON_Delete(read);
        cJSON_Delete(other);
    }

    cJSON *native = cJSON_Duplicate(settings, true);
    CHECK(native && settings_provenance_set(native, SETTINGS_SOURCE_IMPORTED_STR, day, NULL) &&
          settings_provenance_set_serial(native, "fixture-machine") &&
          settings_provenance_set_family(native, SETTINGS_FAMILY_AS11) &&
          settings_provenance_record_sessions(native, day),
          "mark native");
    if (native) {
        cJSON *unchecked = cJSON_Duplicate(native, true);
        cJSON *unchecked_meta = cJSON_GetObjectItemCaseSensitive(unchecked, SETTINGS_PROVENANCE_KEY);
        cJSON_DeleteItemFromObjectCaseSensitive(unchecked_meta, "local_sessions");
        cJSON_DeleteItemFromObjectCaseSensitive(unchecked_meta, "coverage_verified");
        cJSON_AddArrayToObject(unchecked_meta, "local_sessions");
        CHECK(!settings_provenance_covers_sessions(unchecked, "20260227"),
              "empty unverified coverage is not proof");
        cJSON_DeleteItemFromObjectCaseSensitive(unchecked_meta, "local_sessions");
        cJSON_AddBoolToObject(unchecked_meta, "coverage_verified", true);
        CHECK(!settings_provenance_covers_sessions(unchecked, "20260227"),
              "absent coverage is not proof");
        cJSON_Delete(unchecked);
        snprintf(path, sizeof(path), "%s/%s.imported.settings.json", SD_SUMMARIES_DIR, day);
        json = cJSON_PrintUnformatted(native);
        write_text(path, json);
        free(json);
        read = read_daily_settings(day, SETTINGS_FAMILY_AS11, "fixture-machine");
        CHECK(read && settings_provenance_is(read, SETTINGS_SOURCE_IMPORTED_STR, day, NULL),
              "native daily snapshot should take precedence");
        cJSON_Delete(read);

        snprintf(path, sizeof(path), "%s/imported_session.json", dir);
        write_text(path, "{\"import_source\":\"file_import\",\"source_serial\":\"fixture-machine\","
                         "\"source_device_family\":\"AS11\","
                         "\"source_identity_verified\":true}");
        read = read_daily_settings(day, SETTINGS_FAMILY_AS11, "fixture-machine");
        CHECK(read != NULL, "imported waveform session should not stale STR settings");
        cJSON_Delete(read);
        write_text(path, "{\"import_source\":\"file_import\",\"source_serial\":\"another-machine\","
                         "\"source_device_family\":\"AS11\","
                         "\"source_identity_verified\":true}");
        read = read_daily_settings(day, SETTINGS_FAMILY_AS11, "fixture-machine");
        CHECK(read == NULL, "another machine must not use imported settings");
        cJSON_Delete(read);
        write_text(path, "{\"import_source\":\"file_import\",\"source_serial\":\"fixture-machine\","
                         "\"source_device_family\":\"AS11\","
                         "\"source_identity_verified\":true}");
        snprintf(path, sizeof(path), "%s/later_session.json", dir);
        write_text(path, "{}");
        snprintf(path, sizeof(path), "%s/later_ident.json", dir);
        write_live_ident(path, day, "later");
        read = read_daily_settings(day, SETTINGS_FAMILY_AS11, "fixture-machine");
        CHECK(read == NULL, "new local session must stale the imported snapshot");
        cJSON_Delete(read);
        CHECK(settings_provenance_record_sessions(native, day),
              "refresh imported coverage after re-import");
        snprintf(path, sizeof(path), "%s/%s.imported.settings.json", SD_SUMMARIES_DIR, day);
        json = cJSON_PrintUnformatted(native);
        write_text(path, json);
        free(json);
        read = read_daily_settings(day, SETTINGS_FAMILY_AS11, "fixture-machine");
        CHECK(read != NULL, "re-import covers the later session");
        cJSON_Delete(read);
        cJSON_Delete(native);
    }
    cJSON_Delete(settings);
}

static void test_pb_varint(FILE *f, uint64_t value)
{
    while (value >= 0x80) {
        fputc((int)((value & 0x7f) | 0x80), f);
        value >>= 7;
    }
    fputc((int)value, f);
}

static void write_test_summary(const char *path, int64_t noon_ms)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "create synthetic spool");
    if (!f) return;
    const struct { int field; int64_t value; } vals[] = {
        {SUM_F_PERIOD_START, noon_ms}, {SUM_F_PERIOD_END, noon_ms + 86400000},
        {SUM_F_DURATION_MIN, 120}, {SUM_F_TUBE_CONNECTED, 1},
        {SUM_F_HUM_CONNECTED, 2},
    };
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        test_pb_varint(f, (uint64_t)vals[i].field << 3);
        test_pb_varint(f, (uint64_t)vals[i].value);
    }
    fclose(f);
}

static void test_str_settings_history_end_to_end(void)
{
    set_tz("UTC");
    const char *sid = "20260302_220000";
    const char *day = "20260302";
    const int64_t start = START_MS + 86400000LL;
    char dir[400], path[600], out[400], current_spool[600];
    snprintf(dir, sizeof(dir), "%s/%s", SD_STREAMS_DIR, day);
    mk("%s", dir);
    snprintf(out, sizeof(out), "%s/str_provenance", g_root);
    mk("%s", out);

    cJSON *settings = cJSON_Parse(
        "{\"SettingProfiles\":{\"ActiveProfiles\":{\"TherapyProfile\":\"CpapProfile\"},"
        "\"TherapyProfiles\":{\"CpapProfile\":{\"SetPressure\":8}}}}");
    CHECK(settings && settings_provenance_set(settings, SETTINGS_SOURCE_LIVE, day, sid),
          "create live settings");
    if (!settings) return;
    char *json = cJSON_PrintUnformatted(settings);
    snprintf(path, sizeof(path), "%s/%s.live.settings.json", dir, sid);
    write_text(path, json);
    snprintf(path, sizeof(path), "%s/%s.live.settings.json", SD_SUMMARIES_DIR, day);
    write_text(path, json);
    free(json);
    snprintf(path, sizeof(path), "%s/%s_session.json", dir, sid);
    write_text(path, "{}");
    snprintf(path, sizeof(path), "%s/%s_ident.json", dir, sid);
    write_live_ident(path, day, sid);
    snprintf(path, sizeof(path), "%s/20260228.settings.json", SD_SUMMARIES_DIR);
    write_text(path, "{\"SettingProfiles\":{\"ActiveProfiles\":{\"TherapyProfile\":\"CpapProfile\"}}}");
    snprintf(path, sizeof(path), "%s/20260228.spool", SD_SUMMARIES_DIR);
    write_test_summary(path, START_MS - 34LL * 3600000);
    snprintf(current_spool, sizeof(current_spool), "%s/%s.spool", SD_SUMMARIES_DIR, day);
    write_test_summary(current_spool, start - 10LL * 3600000);
    cJSON *ident = cJSON_Parse("{\"SerialNumber\":\"fixture-machine\",\"PlatformIdentifier\":46}");
    CHECK(ident != NULL, "create exporting device identity");

    for (int pass = 0; pass < 8; pass++) {
        if (pass == 1) {
            cJSON *native = cJSON_Duplicate(settings, true);
            CHECK(native && settings_provenance_set(native, SETTINGS_SOURCE_IMPORTED_STR,
                                                   "20260228", NULL) &&
                  settings_provenance_set_serial(native, "fixture-machine") &&
                  settings_provenance_set_family(native, SETTINGS_FAMILY_AS11) &&
                  settings_provenance_record_sessions(native, "20260228"), "mark imported history");
            if (native) {
                snprintf(path, sizeof(path), "%s/20260228.imported.settings.json", SD_SUMMARIES_DIR);
                json = cJSON_PrintUnformatted(native);
                write_text(path, json);
                free(json);
                cJSON_Delete(native);
            }
        }
        if (pass == 2) unlink(current_spool);  /* Exercise synthesized current day. */
        cJSON *export_settings = settings;
        cJSON *borrowed = NULL;
        if (pass == 3) {
            borrowed = cJSON_Duplicate(settings, true);
            CHECK(borrowed && settings_provenance_set(borrowed, SETTINGS_SOURCE_CACHED, day, sid),
                  "mark cached settings");
            snprintf(path, sizeof(path), "%s/%s.live.settings.json", dir, sid);
            json = cJSON_PrintUnformatted(borrowed);
            write_text(path, json);
            free(json);
            export_settings = borrowed;
        } else if (pass == 4) {
            snprintf(path, sizeof(path), "%s/%s.live.settings.json", dir, sid);
            unlink(path);
            snprintf(path, sizeof(path), "%s/%s.live.settings.json", SD_SUMMARIES_DIR, day);
            unlink(path);
            borrowed = cJSON_Duplicate(settings, true);
            CHECK(borrowed && settings_provenance_set(borrowed, SETTINGS_SOURCE_LIVE,
                                                     day, "different_session"),
                  "mark borrowed same-day settings");
            export_settings = borrowed;
        } else if (pass == 5) {
            snprintf(path, sizeof(path), "%s/%s.live.settings.json", dir, sid);
            json = cJSON_PrintUnformatted(settings);
            write_text(path, json);
            free(json);
        } else if (pass == 6) {
            snprintf(path, sizeof(path), "%s/%s_ident.json", dir, sid);
            write_text(path, "{\"SerialNumber\":\"fixture-machine\",\"PlatformIdentifier\":46}");
        } else if (pass == 7) {
            snprintf(path, sizeof(path), "%s/%s_ident.json", dir, sid);
            write_live_ident(path, "20260301", sid);
        }
        CHECK(edf_generate_str_edf(out, "X X X X", "Startdate 02-MAR-2026 X X X",
              "02.03.26", export_settings, ident, dir, sid, start, start + 7200000, 0) == ESP_OK,
              "generate STR pass %d", pass);
        snprintf(path, sizeof(path), "%s/STR.edf", out);
        size_t len = 0;
        uint8_t *bytes = read_file(path, &len);
        CHECK(bytes != NULL, "STR exists pass %d", pass);
        if (!bytes) continue;
        edf_t e;
        CHECK(edf_parse(bytes, len, &e) && e.ndr == 3, "STR structure pass %d", pass);
        if (edf_parse(bytes, len, &e) && e.ndr == 3) {
            CHECK(edf_sample(bytes, &e, 0, 5, 0) == (pass > 0 && pass < 6 ? 3 : -1),
                  "historical Mode pass %d", pass);
            CHECK(edf_sample(bytes, &e, 0, 7, 0) == (pass > 0 && pass < 6 ? 400 : -1),
                  "historical pressure pass %d", pass);
            CHECK(edf_sample(bytes, &e, 0, 76, 0) == 1 &&
                  edf_sample(bytes, &e, 0, 77, 0) == 2,
                  "spool tube/humidifier lost pass %d", pass);
            CHECK(edf_sample(bytes, &e, 1, 0, 0) ==
                  as11_time_day_number("20260301") &&
                  edf_sample(bytes, &e, 1, 3, 0) == -1 &&
                  edf_sample(bytes, &e, 1, 4, 0) == -1 &&
                  edf_sample(bytes, &e, 1, 5, 0) == -1,
                  "calendar gap is an unknown record pass %d", pass);
            CHECK(edf_sample(bytes, &e, 2, 0, 0) == as11_time_day_number(day) &&
                  edf_sample(bytes, &e, 2, 5, 0) == (pass < 3 || pass == 5 ? 3 : -1) &&
                  edf_sample(bytes, &e, 2, 7, 0) == (pass < 3 || pass == 5 ? 400 : -1),
                  "current settings provenance pass %d", pass);
            check_edf_structure("STR.edf", bytes, len, &e);
            for (int r = 0; r < e.ndr; r++) {
                const uint8_t *rec = bytes + e.hdr_bytes + r * e.rec_bytes;
                CHECK(ccitt_false(rec, (size_t)e.rec_bytes - 2) ==
                      (uint16_t)edf_sample(bytes, &e, r, e.ns - 1, 0),
                      "STR record CRC pass %d record %d", pass, r);
            }
        }
        free(bytes);
        cJSON_Delete(borrowed);
    }
    cJSON_Delete(ident);
    cJSON_Delete(settings);
}

/* ── Identification.json / Identification.crc ────────────────────────────────
 * edf_generate_identification_files() was the largest untested function in the
 * exporter. The mutation harness picks it as edf_header.c's canary, empties its
 * body, and every test still passed — so the harness refused a verdict on all
 * 456 lines of that file rather than report a clean sweep it had not earned.
 *
 * These exist to make that verdict possible, so they pin behaviour a mutant
 * would change rather than merely reaching the function: the CRC's byte order
 * and length, the number->string coercion, the defaults for absent keys, and
 * the one input/output key name that differs on purpose.
 */
static void ident_write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    size_t n = strlen(text);
    assert(fwrite(text, 1, n, f) == n);
    assert(fclose(f) == 0);
}

static char *ident_slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    assert(fseek(f, 0, SEEK_END) == 0);
    long n = ftell(f);
    assert(n >= 0);
    assert(fseek(f, 0, SEEK_SET) == 0);
    char *buf = malloc((size_t)n + 1);
    assert(buf);
    assert(fread(buf, 1, (size_t)n, f) == (size_t)n);
    buf[n] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)n;
    return buf;
}

static void ident_make_dir(char *dst, size_t cap, const char *name)
{
    snprintf(dst, cap, "%s/%s", g_root, name);
    mkdir(dst, 0777);
}

/* The full profile the AS11 expects, so the shape is pinned rather than sampled. */
static const char *IDENT_FULL =
    "{"
    "\"UniversalIdentifier\":\"UID-1\","
    "\"SerialNumber\":\"SN-2\","
    "\"ProductCode\":37001,"
    "\"ProductName\":\"AirSense 11\","
    "\"ProductGeographicIdentifier\":\"EUR\","
    "\"HardwareIdentifier\":\"HW-3\","
    "\"BootloaderIdentifier\":\"BL-4\","
    "\"ApplicationIdentifier\":\"APP-5\","
    "\"ConfigurationIdentifier\":\"CFG-6\","
    "\"PlatformIdentifier\":12,"
    "\"VariantIdentifier\":34,"
    "\"RegionIdentifier\":56,"
    "\"ProfileVariantIdentifier\":\"PV-7\","
    "\"DataVersionIdentifier\":78,"
    "\"DataModelVersionIdentifier\":\"DM-8\""
    "}";

static cJSON *ident_profile(cJSON *root, const char *which)
{
    cJSON *fg = cJSON_GetObjectItem(root, "FlowGenerator");
    assert(fg);
    cJSON *profiles = cJSON_GetObjectItem(fg, "IdentificationProfiles");
    assert(profiles);
    cJSON *p = cJSON_GetObjectItem(profiles, which);
    assert(p);
    return p;
}

static void ident_expect_str(cJSON *obj, const char *key, const char *want)
{
    cJSON *j = cJSON_GetObjectItem(obj, key);
    assert(j);
    assert(cJSON_IsString(j));
    assert(strcmp(j->valuestring, want) == 0);
}

static void test_crc32_matches_the_published_check_vector(void)
{
    /* CRC-32/ISO-HDLC (IEEE 802.3) has a published check value: the CRC of the
     * nine bytes "123456789" is 0xCBF43926.
     *
     * Asserting against the constant rather than against another call of the same
     * function is the entire point of this test. The Identification.crc test below
     * compares the file's four bytes to edf_crc32_ieee() — so a mutation INSIDE the
     * CRC changes both sides equally and survives, which is exactly what the
     * mutation harness reported when this test did not exist: the loop bounds at
     * lines 50 and 52 and the shift at line 56 all survived a suite that looked
     * like it covered them. A test whose oracle is the code under test defends the
     * bug instead of catching it. */
    assert(edf_crc32_ieee((const uint8_t *)"123456789", 9) == 0xCBF43926u);
    assert(edf_crc32_ieee((const uint8_t *)"", 0) == 0u);
    assert(edf_crc32_ieee((const uint8_t *)"a", 1) == 0xE8B7BE43u);
    /* Length is honoured, not strlen: a CRC over a prefix differs. */
    assert(edf_crc32_ieee((const uint8_t *)"123456789", 8) != 0xCBF43926u);
}

static void test_identification_profile_shape(void)
{
    char dir[300], ident[400], out_path[400];
    ident_make_dir(dir, sizeof(dir), "ident_shape");
    snprintf(ident, sizeof(ident), "%s/identification.json", dir);
    ident_write_file(ident, IDENT_FULL);

    assert(edf_generate_identification_files(dir, ident) == ESP_OK);

    snprintf(out_path, sizeof(out_path), "%s/Identification.json", dir);
    char *out = ident_slurp(out_path, NULL);
    assert(out);
    cJSON *root = cJSON_Parse(out);
    assert(root);

    cJSON *product = ident_profile(root, "Product");
    ident_expect_str(product, "UniversalIdentifier", "UID-1");
    ident_expect_str(product, "SerialNumber", "SN-2");
    ident_expect_str(product, "ProductName", "AirSense 11");
    ident_expect_str(product, "ProductGeographicIdentifier", "EUR");
    /* A numeric ProductCode is rendered as a STRING. The AS11 writes it either
     * way and the profile always carries a string, so the coercion is load-bearing. */
    ident_expect_str(product, "ProductCode", "37001");
    /* Always blank, never copied from the source, even when the source has them. */
    ident_expect_str(product, "SerialNumberVerificationCode", "");
    ident_expect_str(product, "FdaUniqueDeviceIdentifier", "");

    ident_expect_str(ident_profile(root, "Hardware"), "HardwareIdentifier", "HW-3");

    cJSON *software = ident_profile(root, "Software");
    ident_expect_str(software, "BootloaderIdentifier", "BL-4");
    ident_expect_str(software, "ApplicationIdentifier", "APP-5");
    ident_expect_str(software, "ConfigurationIdentifier", "CFG-6");
    ident_expect_str(software, "DataModelVersionIdentifier", "DM-8");
    /* THE KEY NAME CHANGES ON PURPOSE: the source says ProfileVARIANTIdentifier,
     * the profile says ProfileVARIATIONIdentifier. It reads like a typo and it is
     * not — pinned here so that "fixing" it breaks a test instead of a device. */
    ident_expect_str(software, "ProfileVariationIdentifier", "PV-7");
    assert(cJSON_GetObjectItem(software, "ProfileVariantIdentifier") == NULL);

    /* These four stay NUMBERS. */
    cJSON *plat = cJSON_GetObjectItem(software, "PlatformIdentifier");
    assert(plat && cJSON_IsNumber(plat) && plat->valuedouble == 12);
    cJSON *var = cJSON_GetObjectItem(software, "VariantIdentifier");
    assert(var && cJSON_IsNumber(var) && var->valuedouble == 34);
    cJSON *reg = cJSON_GetObjectItem(software, "RegionIdentifier");
    assert(reg && cJSON_IsNumber(reg) && reg->valuedouble == 56);
    cJSON *dv = cJSON_GetObjectItem(software, "DataVersionIdentifier");
    assert(dv && cJSON_IsNumber(dv) && dv->valuedouble == 78);

    cJSON_Delete(root);
    free(out);
}

static void test_identification_crc_is_le_crc32_of_the_json(void)
{
    char dir[300], ident[400], json_path[400], crc_path[400];
    ident_make_dir(dir, sizeof(dir), "ident_crc");
    snprintf(ident, sizeof(ident), "%s/identification.json", dir);
    ident_write_file(ident, IDENT_FULL);

    assert(edf_generate_identification_files(dir, ident) == ESP_OK);

    snprintf(json_path, sizeof(json_path), "%s/Identification.json", dir);
    snprintf(crc_path, sizeof(crc_path), "%s/Identification.crc", dir);

    size_t json_len = 0, crc_len = 0;
    char *json = ident_slurp(json_path, &json_len);
    unsigned char *crc_file = (unsigned char *)ident_slurp(crc_path, &crc_len);
    assert(json && crc_file);

    /* Exactly four bytes — not a text rendering, not a trailing newline. */
    assert(crc_len == 4);

    /* Little-endian, over the WHOLE json as written. Both halves of that matter:
     * a byte-order flip and an off-by-one on the length are the two mutations a
     * CRC written this way invites. */
    uint32_t want = edf_crc32_ieee((const uint8_t *)json, json_len);
    uint32_t got = (uint32_t)crc_file[0]
                 | ((uint32_t)crc_file[1] << 8)
                 | ((uint32_t)crc_file[2] << 16)
                 | ((uint32_t)crc_file[3] << 24);
    assert(got == want);
    assert(got != edf_crc32_ieee((const uint8_t *)json, json_len - 1));

    free(json);
    free(crc_file);
}

static void test_identification_absent_fields_are_empty_or_zero(void)
{
    char dir[300], ident[400], out_path[400];
    ident_make_dir(dir, sizeof(dir), "ident_sparse");
    snprintf(ident, sizeof(ident), "%s/identification.json", dir);
    ident_write_file(ident, "{\"SerialNumber\":\"ONLY\"}");

    assert(edf_generate_identification_files(dir, ident) == ESP_OK);

    snprintf(out_path, sizeof(out_path), "%s/Identification.json", dir);
    char *out = ident_slurp(out_path, NULL);
    assert(out);
    cJSON *root = cJSON_Parse(out);
    assert(root);

    cJSON *product = ident_profile(root, "Product");
    ident_expect_str(product, "SerialNumber", "ONLY");
    /* Absent strings are present-and-empty, not absent and not null: the profile
     * always carries every key. */
    ident_expect_str(product, "UniversalIdentifier", "");
    ident_expect_str(product, "ProductCode", "");
    ident_expect_str(product, "ProductName", "");

    cJSON *software = ident_profile(root, "Software");
    cJSON *plat = cJSON_GetObjectItem(software, "PlatformIdentifier");
    assert(plat && cJSON_IsNumber(plat) && plat->valuedouble == 0);
    cJSON *dv = cJSON_GetObjectItem(software, "DataVersionIdentifier");
    assert(dv && cJSON_IsNumber(dv) && dv->valuedouble == 0);

    cJSON_Delete(root);
    free(out);
}

static void test_identification_missing_source_is_refused(void)
{
    char dir[300], ident[400], json_path[400], crc_path[400];
    ident_make_dir(dir, sizeof(dir), "ident_missing");
    snprintf(ident, sizeof(ident), "%s/does-not-exist.json", dir);

    assert(edf_generate_identification_files(dir, ident) == ESP_FAIL);

    /* And it writes nothing at all: a refused generation must not leave a
     * half-made profile behind for the next reader to trust. */
    snprintf(json_path, sizeof(json_path), "%s/Identification.json", dir);
    snprintf(crc_path, sizeof(crc_path), "%s/Identification.crc", dir);
    assert(ident_slurp(json_path, NULL) == NULL);
    assert(ident_slurp(crc_path, NULL) == NULL);
}

/* Issue #326: when a session's post-therapy Get RPCs fail on a fresh day with
 * only one session, edf_gen must:
 *   1. refuse to write headerless EDFs (with blank SRN=/MID=/VID=) when no
 *      identification exists anywhere on the card;
 *   2. borrow _ident.json and _settings.json from another day in streams/
 *      (when same-day metadata is absent) and backfill the session's files;
 *   3. fall back to <sessions_root>/device_metadata.json when no day in
 *      streams/ has metadata yet (first-night fallback). */
static void test_cross_day_and_snapshot_metadata_fallback(void)
{
    set_tz("UTC");
    char root[300], sd[400], out[400], p[600];
    snprintf(root, sizeof(root), "%s/meta_fallback", g_root);
    build_session(root, 2, SA2_N, sd, sizeof(sd));

    /* Remove the session's own _ident.json and _settings.json. */
    snprintf(p, sizeof(p), "%s/%s_ident.json", sd, SID);
    unlink(p);
    snprintf(p, sizeof(p), "%s/%s_settings.json", sd, SID);
    unlink(p);

    /* 1. With no metadata anywhere, export is refused before writing headerless EDFs. */
    CHECK(generate(root, sd, out, sizeof(out)) == ESP_FAIL,
          "export without any identification source must fail");
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/%s_BRP.edf", out, SID);
    size_t brp_len = 0;
    uint8_t *brp = read_file(p, &brp_len);
    CHECK(brp == NULL, "headerless BRP.edf must not be written when ident is missing");
    free(brp);

    /* 2. Add a sibling day (20260302) with valid _ident.json and _settings.json. */
    mk("%s/streams/20260302", root);
    snprintf(p, sizeof(p), "%s/streams/20260302/20260302_220000_ident.json", root);
    write_text(p, "{\"SerialNumber\":\"23249999999\",\"PlatformIdentifier\":46,\"VariantIdentifier\":3}\n");
    snprintf(p, sizeof(p), "%s/streams/20260302/20260302_220000_settings.json", root);
    write_text(p, "{\"SettingProfiles\":{\"ActiveProfiles\":{\"TherapyProfile\":\"AutoSetProfile\"}}}\n");

    CHECK(generate(root, sd, out, sizeof(out)) == ESP_OK,
          "cross-day metadata fallback should allow export to succeed");
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/%s_BRP.edf", out, SID);
    brp = read_file(p, &brp_len);
    CHECK(brp != NULL && brp_len >= 256, "BRP.edf missing after cross-day fallback");
    if (brp && brp_len >= 256) {
        char rec_id[81];
        memcpy(rec_id, brp + 88, 80);
        rec_id[80] = '\0';
        CHECK(strstr(rec_id, "SRN=23249999999 MID=46 VID=3") != NULL,
              "BRP.edf recording_id did not carry borrowed identity: '%s'", rec_id);
    }
    free(brp);

    /* 3. Device snapshot fallback when no streams/<day>/ has _ident.json. */
    char root2[300], sd2[400], out2[400];
    snprintf(root2, sizeof(root2), "%s/snap_fallback", g_root);
    build_session(root2, 2, SA2_N, sd2, sizeof(sd2));
    snprintf(p, sizeof(p), "%s/%s_ident.json", sd2, SID);
    unlink(p);
    snprintf(p, sizeof(p), "%s/%s_settings.json", sd2, SID);
    unlink(p);
    snprintf(p, sizeof(p), "%s/device_metadata.json", root2);
    write_text(p, "{\"serial\":\"23248888888\","
                  "\"ident\":{\"SerialNumber\":\"23248888888\",\"PlatformIdentifier\":46,\"VariantIdentifier\":3},"
                  "\"settings\":{\"SettingProfiles\":{\"ActiveProfiles\":{\"TherapyProfile\":\"CpapProfile\"}}}}\n");

    CHECK(generate(root2, sd2, out2, sizeof(out2)) == ESP_OK,
          "device_metadata.json snapshot fallback should allow export to succeed");
    snprintf(p, sizeof(p), "%s/DATALOG/20260301/%s_BRP.edf", out2, SID);
    brp = read_file(p, &brp_len);
    CHECK(brp != NULL && brp_len >= 256, "BRP.edf missing after snapshot fallback");
    if (brp && brp_len >= 256) {
        char rec_id[81];
        memcpy(rec_id, brp + 88, 80);
        rec_id[80] = '\0';
        CHECK(strstr(rec_id, "SRN=23248888888 MID=46 VID=3") != NULL,
              "BRP.edf recording_id did not carry snapshot identity: '%s'", rec_id);
    }
    free(brp);
}

/* ── edf_waveform: the guards in the event-file readers ──────────────────────
 * Eighteen of the harness's thirty-three survivors lived in these three
 * functions, all of them the same shape: a guard whose false branch nothing
 * exercised. `events && cJSON_IsArray(events)` flipped to `||` still returns
 * the right answer on every well-formed file, because a well-formed file always
 * has an array there. The tests below are therefore all MALFORMED input — the
 * only input that tells the two versions apart.
 *
 * Two of the mutants matter beyond the score. Dropping the IsString check on a
 * label reaches strcmp(NULL, ...) and dropping it on reportTime reaches the
 * ISO-8601 parser with a NULL pointer, both from a file the device wrote after
 * a crash or a partial flush.
 */
static void wf_write(const char *path, const char *jsonl)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fputs(jsonl, f) >= 0);
    assert(fclose(f) == 0);
}

static const char *wf_path(char *dst, size_t cap, const char *name)
{
    snprintf(dst, cap, "%s/%s", g_root, name);
    return dst;
}

/* ---- snt_available_samples -------------------------------------------------- */

static void test_wf_available_samples_guards(void)
{
    char p[400];
    wf_path(p, sizeof(p), "wf_avail.snt");

    /* A file holding exactly the header and nothing else: no samples, and that
     * is a 0, not an error. */
    FILE *f = fopen(p, "wb");
    assert(f);
    snt_header_t h;
    memset(&h, 0, sizeof(h));
    assert(fwrite(&h, 1, sizeof(h), f) == sizeof(h));
    assert(fclose(f) == 0);
    f = fopen(p, "rb");
    assert(f);
    assert(snt_available_samples(f, 2) == 0);

    /* CHANNELS ZERO IS REFUSED, not divided by. `channels_in_file <= 0` weakened
     * to `< 0` lets a zero through to `data_bytes / frame` with frame == 0. */
    assert(snt_available_samples(f, 0) == UINT32_MAX);
    assert(snt_available_samples(f, -1) == UINT32_MAX);
    assert(snt_available_samples(NULL, 2) == UINT32_MAX);
    assert(fclose(f) == 0);

    /* An EMPTY file is 0 samples. end == 0 here, so a guard written `end <= 0`
     * instead of `end < 0` reports UINT32_MAX — an error where there is none. */
    f = fopen(p, "wb"); assert(f); assert(fclose(f) == 0);
    f = fopen(p, "rb"); assert(f);
    assert(snt_available_samples(f, 2) == 0);
    assert(fclose(f) == 0);

    /* Header plus three whole frames of two channels, read from the START of the
     * file: ftell is 0 there, so `cur < 0` weakened to `<= 0` would refuse a
     * perfectly ordinary call. */
    f = fopen(p, "wb");
    assert(f);
    assert(fwrite(&h, 1, sizeof(h), f) == sizeof(h));
    int16_t frames[6] = { 1, 2, 3, 4, 5, 6 };
    assert(fwrite(frames, sizeof(int16_t), 6, f) == 6);
    assert(fclose(f) == 0);
    f = fopen(p, "rb");
    assert(f);
    assert(ftell(f) == 0);
    assert(snt_available_samples(f, 2) == 3);
    assert(fclose(f) == 0);
}

/* ---- edf_find_mask_on_time / edf_find_mask_off_time -------------------------- */

static void test_wf_mask_times_are_found(void)
{
    char p[400];
    wf_path(p, sizeof(p), "wf_mask_ok.jsonl");
    wf_write(p,
        "{\"params\":{\"events\":[{\"event\":\"MaskOn\","
        "\"reportTime\":\"2026-09-01T22:10:00.000\"}]}}\n"
        "{\"params\":{\"events\":[{\"event\":\"MaskOff\","
        "\"reportTime\":\"2026-09-02T06:20:00.000\"}]}}\n");
    int64_t on = edf_find_mask_on_time(p);
    int64_t off = edf_find_mask_off_time(p);
    assert(on > 0);
    assert(off > on);
    /* MaskOff takes the LAST one in the file, MaskOn the first — asserted as an
     * ordering rather than a constant so the test does not pin a timezone. */
    assert(off - on == 8 * 3600 * 1000 + 10 * 60 * 1000);
}

static void test_wf_mask_on_last_wins_is_first_wins(void)
{
    char p[400];
    wf_path(p, sizeof(p), "wf_mask_first.jsonl");
    wf_write(p,
        "{\"params\":{\"events\":[{\"event\":\"MaskOn\","
        "\"reportTime\":\"2026-09-01T22:00:00.000\"}]}}\n"
        "{\"params\":{\"events\":[{\"event\":\"MaskOn\","
        "\"reportTime\":\"2026-09-01T23:00:00.000\"}]}}\n");
    int64_t a = edf_find_mask_on_time(p);
    wf_path(p, sizeof(p), "wf_mask_one.jsonl");
    wf_write(p,
        "{\"params\":{\"events\":[{\"event\":\"MaskOn\","
        "\"reportTime\":\"2026-09-01T22:00:00.000\"}]}}\n");
    /* MaskOn returns on the FIRST match; a second one later must not move it. */
    assert(a == edf_find_mask_on_time(p));
}

static void test_wf_mask_off_takes_the_last(void)
{
    char p[400];
    wf_path(p, sizeof(p), "wf_maskoff_last.jsonl");
    wf_write(p,
        "{\"params\":{\"events\":[{\"event\":\"MaskOff\","
        "\"reportTime\":\"2026-09-02T05:00:00.000\"}]}}\n"
        "{\"params\":{\"events\":[{\"event\":\"MaskOff\","
        "\"reportTime\":\"2026-09-02T06:00:00.000\"}]}}\n");
    int64_t last = edf_find_mask_off_time(p);
    wf_path(p, sizeof(p), "wf_maskoff_one.jsonl");
    wf_write(p,
        "{\"params\":{\"events\":[{\"event\":\"MaskOff\","
        "\"reportTime\":\"2026-09-02T06:00:00.000\"}]}}\n");
    /* MaskOff keeps scanning and reports the last — the session ends when the
     * mask last came off, not the first time it did. */
    assert(last == edf_find_mask_off_time(p));
}

static void test_wf_events_must_be_an_array(void)
{
    char p[400];
    /* "events" as an OBJECT. cJSON_GetArraySize counts an object's children too,
     * so a mutant that drops the IsArray half of the guard walks into it and
     * finds the MaskOn inside. */
    wf_path(p, sizeof(p), "wf_events_obj.jsonl");
    wf_write(p,
        "{\"params\":{\"events\":{\"x\":{\"event\":\"MaskOn\","
        "\"reportTime\":\"2026-09-01T22:00:00.000\"}}}}\n");
    assert(edf_find_mask_on_time(p) == -1);

    wf_path(p, sizeof(p), "wf_events_obj_off.jsonl");
    wf_write(p,
        "{\"params\":{\"events\":{\"x\":{\"event\":\"MaskOff\","
        "\"reportTime\":\"2026-09-02T06:00:00.000\"}}}}\n");
    assert(edf_find_mask_off_time(p) == -1);
}

static void test_wf_a_non_string_label_is_skipped(void)
{
    char p[400];
    /* A numeric "event". The guard is `!label || !cJSON_IsString(label)`; turn
     * that `||` into `&&` and a number reaches strcmp(label->valuestring, ...)
     * with valuestring NULL. */
    wf_path(p, sizeof(p), "wf_label_num.jsonl");
    wf_write(p, "{\"params\":{\"events\":[{\"event\":123,"
                "\"reportTime\":\"2026-09-01T22:00:00.000\"}]}}\n");
    assert(edf_find_mask_on_time(p) == -1);
    assert(edf_find_mask_off_time(p) == -1);
}

static void test_wf_a_non_string_report_time_is_skipped(void)
{
    char p[400];
    /* A numeric "reportTime" behind a matching label. Dropping the IsString half
     * of `rt && cJSON_IsString(rt)` hands a NULL to the ISO-8601 parser. */
    wf_path(p, sizeof(p), "wf_rt_num.jsonl");
    wf_write(p, "{\"params\":{\"events\":[{\"event\":\"MaskOn\","
                "\"reportTime\":99999}]}}\n");
    assert(edf_find_mask_on_time(p) == -1);

    wf_path(p, sizeof(p), "wf_rt_num_off.jsonl");
    wf_write(p, "{\"params\":{\"events\":[{\"event\":\"MaskOff\","
                "\"reportTime\":99999}]}}\n");
    assert(edf_find_mask_off_time(p) == -1);
}

/* ---- edf_find_zle_edge_time --------------------------------------------------- */

static void test_wf_zle_only_reads_zle_records(void)
{
    char p[400];
    /* dataId is present but is not _ZLE. The guard is
     * `data_id && cJSON_IsString(data_id) && strcmp(...) == 0`; turning the
     * first && into || makes it `data_id || (IsString && strcmp == 0)`, so ANY
     * record with a dataId is treated as a ZLE record. */
    wf_path(p, sizeof(p), "wf_zle_other.jsonl");
    wf_write(p,
        "{\"params\":{\"dataId\":\"OTHER\",\"events\":[{\"value\":1,"
        "\"reportTime\":\"2026-09-01T22:00:00.000\"}]}}\n");
    assert(edf_find_zle_edge_time(p, 1, 0) == -1);
}

static void test_wf_zle_value_must_be_a_number(void)
{
    char p[400];
    /* "value" as a STRING. `val && cJSON_IsNumber(val) && ...` with the first
     * && turned into || accepts it, because val is merely non-NULL. */
    wf_path(p, sizeof(p), "wf_zle_valstr.jsonl");
    wf_write(p,
        "{\"params\":{\"dataId\":\"_ZLE\",\"events\":[{\"value\":\"1\","
        "\"reportTime\":\"2026-09-01T22:00:00.000\"}]}}\n");
    assert(edf_find_zle_edge_time(p, 1, 0) == -1);
}

static void test_wf_zle_falls_back_to_ntp_when_report_time_is_unusable(void)
{
    char p[400];
    /* A numeric reportTime is not a string, so the AS11 path is skipped and the
     * ntpTimeMs fallback answers. Dropping the IsString check parses NULL. */
    wf_path(p, sizeof(p), "wf_zle_ntp.jsonl");
    wf_write(p,
        "{\"params\":{\"dataId\":\"_ZLE\",\"events\":[{\"value\":1,"
        "\"reportTime\":7,\"ntpTimeMs\":1756000000000}]}}\n");
    assert(edf_find_zle_edge_time(p, 1, 0) == 1756000000000LL);
}

static void test_wf_zle_clock_drift_is_added_to_the_as11_time(void)
{
    char p[400];
    wf_path(p, sizeof(p), "wf_zle_drift.jsonl");
    wf_write(p,
        "{\"params\":{\"dataId\":\"_ZLE\",\"events\":[{\"value\":1,"
        "\"reportTime\":\"2026-09-01T22:00:00.000\"}]}}\n");
    int64_t zero = edf_find_zle_edge_time(p, 1, 0);
    int64_t plus = edf_find_zle_edge_time(p, 1, 4321);
    assert(zero > 0);
    assert(plus - zero == 4321);
}

/* Round two: the guards that a single malformed record cannot separate.
 *
 * `rt && cJSON_IsString(rt)` weakened to `||` still yields -1 on a file whose
 * only MaskOn has a numeric reportTime, because the parser hands back a failure
 * for the NULL valuestring and -1 is what the caller would have returned anyway.
 * The two versions diverge only when a USABLE record follows a broken one:
 * MaskOn returns on its first match, so the mutant returns -1 and never reaches
 * the good line; MaskOff keeps the last, so the mutant overwrites a good time
 * with -1. Each therefore needs the bad record on the side its function cares
 * about. */

static void test_wf_mask_on_survives_a_broken_record_before_a_good_one(void)
{
    char p[400];
    wf_path(p, sizeof(p), "wf_on_bad_then_good.jsonl");
    wf_write(p,
        "{\"params\":{\"events\":[{\"event\":\"MaskOn\",\"reportTime\":99999}]}}\n"
        "{\"params\":{\"events\":[{\"event\":\"MaskOn\","
        "\"reportTime\":\"2026-09-01T22:00:00.000\"}]}}\n");
    int64_t got = edf_find_mask_on_time(p);
    wf_path(p, sizeof(p), "wf_on_good_only.jsonl");
    wf_write(p,
        "{\"params\":{\"events\":[{\"event\":\"MaskOn\","
        "\"reportTime\":\"2026-09-01T22:00:00.000\"}]}}\n");
    assert(got > 0);
    assert(got == edf_find_mask_on_time(p));
}

static void test_wf_mask_off_survives_a_broken_record_after_a_good_one(void)
{
    char p[400];
    wf_path(p, sizeof(p), "wf_off_good_then_bad.jsonl");
    wf_write(p,
        "{\"params\":{\"events\":[{\"event\":\"MaskOff\","
        "\"reportTime\":\"2026-09-02T06:00:00.000\"}]}}\n"
        "{\"params\":{\"events\":[{\"event\":\"MaskOff\",\"reportTime\":99999}]}}\n");
    int64_t got = edf_find_mask_off_time(p);
    wf_path(p, sizeof(p), "wf_off_good_only.jsonl");
    wf_write(p,
        "{\"params\":{\"events\":[{\"event\":\"MaskOff\","
        "\"reportTime\":\"2026-09-02T06:00:00.000\"}]}}\n");
    assert(got > 0);
    assert(got == edf_find_mask_off_time(p));
}

static void test_wf_zle_events_must_be_an_array(void)
{
    char p[400];
    /* The same object-shaped events as the MaskOn case, but inside a _ZLE
     * record — a separate copy of the guard, and it needed its own test. */
    wf_path(p, sizeof(p), "wf_zle_events_obj.jsonl");
    wf_write(p,
        "{\"params\":{\"dataId\":\"_ZLE\",\"events\":{\"x\":{\"value\":1,"
        "\"ntpTimeMs\":1756000000000}}}}\n");
    assert(edf_find_zle_edge_time(p, 1, 0) == -1);
}

static void test_wf_zle_rejects_a_zero_ntp_time(void)
{
    char p[400];
    /* ntpTimeMs of 0 is not a time, it is an unset field. `cand > 0` weakened to
     * `>= 0` accepts it and reports the epoch as a ZLE edge. */
    wf_path(p, sizeof(p), "wf_zle_ntp_zero.jsonl");
    wf_write(p,
        "{\"params\":{\"dataId\":\"_ZLE\",\"events\":[{\"value\":1,"
        "\"ntpTimeMs\":0}]}}\n");
    assert(edf_find_zle_edge_time(p, 1, 0) == -1);
}

static void test_wf_zle_an_epoch_report_time_is_not_a_time(void)
{
    char p[400];
    /* A reportTime that parses to 0 must not become `0 + drift`. `as11_ms > 0`
     * weakened to `>= 0` turns an unset timestamp into a real-looking edge
     * exactly one drift away from the epoch. The ntpTimeMs here is what the
     * function SHOULD fall back to, so the assertion names the right answer
     * rather than merely rejecting the wrong one. */
    wf_path(p, sizeof(p), "wf_zle_epoch.jsonl");
    wf_write(p,
        "{\"params\":{\"dataId\":\"_ZLE\",\"events\":[{\"value\":1,"
        "\"reportTime\":\"1970-01-01T00:00:00.000\","
        "\"ntpTimeMs\":1756000000000}]}}\n");
    assert(edf_find_zle_edge_time(p, 1, 60000) == 1756000000000LL);
}

static void test_wf_zle_a_drift_cancelled_time_is_not_a_time(void)
{
    char p[400], q[400];
    /* THE ONE CASE WHERE cand IS EXACTLY ZERO. `cand < 0` weakened to `<= 0`
     * makes a drift that happens to cancel the AS11 timestamp fall through to
     * the ntpTimeMs fallback — so a computed zero silently becomes a different
     * clock's answer instead of being rejected.
     *
     * The drift is derived at run time rather than hard-coded, because the AS11
     * time depends on the host timezone and pinning it would make this test a
     * clock test. */
    wf_path(p, sizeof(p), "wf_zle_drift_probe.jsonl");
    wf_write(p,
        "{\"params\":{\"dataId\":\"_ZLE\",\"events\":[{\"value\":1,"
        "\"reportTime\":\"2026-09-01T22:00:00.000\"}]}}\n");
    int64_t as11 = edf_find_zle_edge_time(p, 1, 0);
    assert(as11 > 0);

    wf_path(q, sizeof(q), "wf_zle_cancel.jsonl");
    wf_write(q,
        "{\"params\":{\"dataId\":\"_ZLE\",\"events\":[{\"value\":1,"
        "\"reportTime\":\"2026-09-01T22:00:00.000\","
        "\"ntpTimeMs\":1756000000000}]}}\n");
    /* as11 + (-as11) == 0, which is not a time. The ntp value must NOT rescue it:
     * the AS11 path answered, and it answered zero. */
    assert(edf_find_zle_edge_time(q, 1, -as11) == -1);
}

/* The dedup pass compares each event only against the previous KEPT one, so it
 * is correct only if identical (onset, label) pairs are adjacent after sorting.
 * Ordering on onset alone did not guarantee that. */
static void test_annotation_dedup_interleaved_duplicate(void)
{
    snt_event_t ev[3] = {
        { 100, 0, "Obstructive Apnea" },
        { 100, 0, "CSR Start" },
        { 100, 0, "Obstructive Apnea" },
    };
    size_t n = sort_and_dedup_events(ev, 3);
    CHECK(n == 2, "the repeat separated by another label is still dropped");
    CHECK(ev[0].onset_sec == 100 && ev[1].onset_sec == 100, "survivors keep the onset");
    CHECK(strcmp(ev[0].label, ev[1].label) != 0, "the survivors are the two distinct labels");
}

static void test_annotation_dedup_orders_and_keeps_distinct(void)
{
    snt_event_t ev[5] = {
        { 300, 0, "Hypopnea" },
        { 100, 0, "Obstructive Apnea" },
        { 100, 0, "Obstructive Apnea" },
        { 200, 0, "Arousal" },
        { 100, 0, "CSR End" },
    };
    size_t n = sort_and_dedup_events(ev, 5);
    CHECK(n == 4, "only the exact repeat is dropped");
    CHECK(ev[0].onset_sec == 100 && ev[1].onset_sec == 100, "both distinct 100s survive");
    CHECK(strcmp(ev[0].label, "CSR End") == 0, "label breaks the onset tie");
    CHECK(ev[2].onset_sec == 200 && ev[3].onset_sec == 300, "onsets ascend");
    CHECK(sort_and_dedup_events(ev, 0) == 0, "an empty list is handled");
}

int main(void)
{
    snprintf(g_root, sizeof(g_root), "%s/snt_edf_test_XXXXXX",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(g_root)) { perror("mkdtemp"); return 2; }

    run("SA2/BRP export identical for SNT v1 and v2; sentinel → -1",
        test_sa2_v1_v2_differential, NULL);
    run("torn sa2.snt tail exports the whole records on disk",
        test_sa2_torn_tail_exports_what_exists, NULL);
    run("torn tail drops the partial record instead of promising it",
        test_torn_tail_drops_partial_record, NULL);
    run("MaskOn skip is the same instant on every channel, v1 and v2",
        test_maskon_skip_aligns_all_channels, NULL);
    run("SA2/BRP/PLD declare the AS11's own signal table",
        test_signal_tables_match_the_as11, NULL);
    run("a tail torn mid-sample rounds down", test_torn_mid_sample_rounds_down, NULL);
    run("overflow saturates at the declared digital maximum",
        test_overflow_clamps_to_declared_digital_max, NULL);
    run("MaskOff ends the export", test_maskoff_truncates_the_export, NULL);
    run("a pre_gated session is not end-truncated at MaskOff",
        test_pre_gated_session_is_not_truncated, NULL);
    run("a pre_gated session keeps a negative waveform offset",
        test_pre_gated_negative_waveform_offset, NULL);
    run("the _ZLE gate applies the AS11->NTP clock drift",
        test_zle_gate_applies_clock_drift, NULL);
    run("the MaskOn fallback applies the AS11->NTP clock drift",
        test_maskon_fallback_adds_the_clock_drift, NULL);
    run("MaskOff applies the AS11->NTP clock drift when ending the export",
        test_maskoff_end_adds_the_clock_drift, NULL);
    run("a single NULL argument is refused", test_a_single_null_argument_is_refused, NULL);
    run("the year-2000 guard excludes only what precedes it",
        test_the_year_2000_guard_excludes_only_what_precedes_it, NULL);
    run("the DATALOG day folder is computed in the AS11 clock",
        test_the_day_folder_is_computed_in_the_as11_clock, NULL);
    run("a .snt with the wrong channel count is refused",
        test_channel_count_mismatch_is_refused, NULL);
    run("a PLD .snt shorter than its channel map is refused",
        test_pld_channel_map_out_of_range_is_refused, NULL);
    run("snt_missing_for per version", test_snt_missing_sentinel_per_version, NULL);
    run("spool_to_edf keeps the -1 sentinel", test_spool_to_edf, NULL);
    run("annotation dedup drops a repeat separated by another label at one onset",
        test_annotation_dedup_interleaved_duplicate, NULL);
    run("annotation dedup orders by onset then label and keeps distinct events",
        test_annotation_dedup_orders_and_keeps_distinct, NULL);
    run("noon_day_folder: noon starts the new day", test_noon_day_folder_boundary, NULL);
    run("the session's AS11 offset beats the device timezone",
        test_as11_offset_beats_device_timezone, NULL);
    run("STR pressure settings are exact cmH2O x 50", test_str_pressure_settings_exact, NULL);
    run("Spont settings accept device strings and imported numbers",
        test_spont_settings_strings_and_numeric, NULL);
    run("PLD snore preserves missing data, zero and range",
        test_pld_snore_missing_and_clamping, NULL);
    run("daily settings require provenance and unambiguous live sessions",
        test_daily_settings_provenance, NULL);
    run("STR settings history preserves unknowns and trusted snapshots",
        test_str_settings_history_end_to_end, NULL);

    run("crc32 matches the published CRC-32/ISO-HDLC check vector",
        test_crc32_matches_the_published_check_vector, NULL);
    run("Identification profile carries every key, with the deliberate rename",
        test_identification_profile_shape, NULL);
    run("Identification.crc is four bytes, little-endian crc32 of the json",
        test_identification_crc_is_le_crc32_of_the_json, NULL);
    run("absent identification fields become empty strings and zeroes",
        test_identification_absent_fields_are_empty_or_zero, NULL);
    run("a missing identification.json is refused and writes nothing",
        test_identification_missing_source_is_refused, NULL);
    run("cross-day and device_metadata.json fallbacks recover missing ident/settings",
        test_cross_day_and_snapshot_metadata_fallback, NULL);

    run("snt_available_samples refuses 0 channels and an empty file is 0",
        test_wf_available_samples_guards, NULL);
    run("MaskOn and MaskOff are read from the event file",
        test_wf_mask_times_are_found, NULL);
    run("MaskOn takes the first match", test_wf_mask_on_last_wins_is_first_wins, NULL);
    run("MaskOff takes the last match", test_wf_mask_off_takes_the_last, NULL);
    run("an events object is not an events array", test_wf_events_must_be_an_array, NULL);
    run("a non-string event label is skipped", test_wf_a_non_string_label_is_skipped, NULL);
    run("a non-string reportTime is skipped",
        test_wf_a_non_string_report_time_is_skipped, NULL);
    run("_ZLE search ignores other dataIds", test_wf_zle_only_reads_zle_records, NULL);
    run("_ZLE value must be a number", test_wf_zle_value_must_be_a_number, NULL);
    run("_ZLE falls back to ntpTimeMs",
        test_wf_zle_falls_back_to_ntp_when_report_time_is_unusable, NULL);
    run("_ZLE adds the clock drift to the AS11 time",
        test_wf_zle_clock_drift_is_added_to_the_as11_time, NULL);

    run("MaskOn skips a broken record and finds the next",
        test_wf_mask_on_survives_a_broken_record_before_a_good_one, NULL);
    run("MaskOff is not overwritten by a broken later record",
        test_wf_mask_off_survives_a_broken_record_after_a_good_one, NULL);
    run("_ZLE events object is not an array", test_wf_zle_events_must_be_an_array, NULL);
    run("_ZLE rejects a zero ntpTimeMs", test_wf_zle_rejects_a_zero_ntp_time, NULL);
    run("_ZLE epoch reportTime falls through to ntp",
        test_wf_zle_an_epoch_report_time_is_not_a_time, NULL);

    run("_ZLE rejects an AS11 time cancelled by the drift",
        test_wf_zle_a_drift_cancelled_time_is_not_a_time, NULL);

    if (!getenv("KEEP_TEST_TREE")) rmtree(g_root);
    else printf("test tree kept at %s\n", g_root);

    printf("\nedf_gen_test: %d passed, %d failed, %d expected failures, %d unexpected passes\n",
           n_pass, n_fail, n_xfail, n_xpass);
    return n_fail ? 1 : 0;
}
