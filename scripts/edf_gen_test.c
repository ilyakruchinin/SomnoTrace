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
    int spr[16];
    long rec_bytes;
} edf_t;

static bool edf_parse(const uint8_t *b, size_t len, edf_t *e)
{
    char t[16];
    if (len < 256) return false;
    memcpy(t, b + 184, 8); t[8] = 0; e->hdr_bytes = atol(t);
    memcpy(t, b + 236, 8); t[8] = 0; e->ndr = atoi(t);
    memcpy(t, b + 252, 4); t[4] = 0; e->ns = atoi(t);
    if (e->ns <= 0 || e->ns > 16) return false;
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
    run("the _ZLE gate applies the AS11->NTP clock drift",
        test_zle_gate_applies_clock_drift, NULL);
    run("a .snt with the wrong channel count is refused",
        test_channel_count_mismatch_is_refused, NULL);
    run("a PLD .snt shorter than its channel map is refused",
        test_pld_channel_map_out_of_range_is_refused, NULL);
    run("snt_missing_for per version", test_snt_missing_sentinel_per_version, NULL);
    run("spool_to_edf keeps the -1 sentinel", test_spool_to_edf, NULL);
    run("noon_day_folder: noon starts the new day", test_noon_day_folder_boundary, NULL);
    run("the session's AS11 offset beats the device timezone",
        test_as11_offset_beats_device_timezone, NULL);
    run("STR pressure settings are exact cmH2O x 50", test_str_pressure_settings_exact, NULL);

    if (!getenv("KEEP_TEST_TREE")) rmtree(g_root);
    else printf("test tree kept at %s\n", g_root);

    printf("\nedf_gen_test: %d passed, %d failed, %d expected failures, %d unexpected passes\n",
           n_pass, n_fail, n_xfail, n_xpass);
    return n_fail ? 1 : 0;
}

