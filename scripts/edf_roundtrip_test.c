/*
 * SomnoTrace - Host round-trip test: live .snt -> EDF -> edf_import -> EDF
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

/*
 * Oracle: edf_gen's output is the reference structure — it is validated
 * against real ResMed cards (EVE/CSL are EDF+D with 0.00 s records, one per
 * annotation; BRP/PLD/SA2 carry 60 s records with a Crc16 trailer).  So:
 *
 *   A = export of a live night          (edf_gen_rebuild_day)
 *   B = export of A re-imported         (A staged in IMPORT/, edf_import,
 *                                        edf_gen_rebuild_day)
 *
 * B must be byte-identical to A.  Any importer defect in file naming,
 * clustering, waveform scaling, annotation parsing (EVE/CSL), identification,
 * settings or STR import shows up as a diff.  Hand-written fixtures cannot
 * catch what the author did not think of — the 0.00 s EVE/CSL record
 * duration slipped past them for exactly that reason.
 *
 * The importer is compiled separately with -DEDF_IMPORT_HOST; the converter
 * modules are #included as in edf_gen_test.c.  Needs a real cJSON.  Fixtures
 * are synthetic — no patient data.
 */

#define TAG TAG_hdr
#include "edf_header.c"
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

#include "edf_import.h"

#include <assert.h>
#include <dirent.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

int host_heap_caps_calloc_fail;
int host_heap_caps_realloc_fail;

/* storage arbitration edf_gen.c calls — single-threaded host, always free */
bool sd_storage_is_ready(void) { return true; }
bool sd_storage_lease_acquire(sd_lease_t role, uint32_t timeout_ms)
{
    (void)role; (void)timeout_ms;
    return true;
}
void sd_storage_lease_release(sd_lease_t role) { (void)role; }

static int g_checks, g_fail;
static void check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(bool ok, const char *fmt, ...)
{
    g_checks++;
    if (ok) return;
    g_fail++;
    char m[300];
    va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof(m), fmt, ap); va_end(ap);
    printf("  FAILED %s\n", m);
}

#define DAY      "20260301"
#define SID      "20260301_220000"
#define START_MS 1772402400000LL          /* 2026-03-01T22:00:00Z */
#define SA2_N    180                      /* 3 min @ 1 Hz   */
#define BRP_N    4500                     /* 3 min @ 25 Hz  */
#define PLD_N    90                       /* 3 min @ 0.5 Hz */

#ifndef SNAP_A
#define SNAP_A "/tmp/somnotrace_rt/A"
#endif

static char g_day[16];     /* the export's DATALOG day folder in A */

static void sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sh(const char *fmt, ...)
{
    char c[1500];
    va_list ap; va_start(ap, fmt); vsnprintf(c, sizeof(c), fmt, ap); va_end(ap);
    if (system(c) != 0) { printf("  warn: '%s' failed\n", c); }
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz > 0 ? (size_t)sz : 1);
    assert(b);
    if (sz > 0 && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

/* Golden hashes captured from the pre-change live export using the synthetic
 * New York fixture below. They cover filenames, timestamps and every byte. */
static void check_live_golden(void)
{
    static const struct { const char *path; uint64_t hash; } golden[] = {
        {"Identification.crc", UINT64_C(0xdee735cb95b8f785)},
        {"Identification.json", UINT64_C(0x27530f8d96dae0b0)},
        {"STR.edf", UINT64_C(0x9436d4b9601cc593)},
        {"SETTINGS/CurrentSettings.crc", UINT64_C(0xe9a5f920b2d8b6fa)},
        {"SETTINGS/CurrentSettings.json", UINT64_C(0x4dda3749430208bb)},
        {"DATALOG/20260301/20260301_170000_CSL.edf", UINT64_C(0x456ce4a823399e6d)},
        {"DATALOG/20260301/20260301_170000_EVE.edf", UINT64_C(0xe962382e50ddd1c1)},
        {"DATALOG/20260301/20260301_170010_BRP.edf", UINT64_C(0x8b183c4f43c719b2)},
        {"DATALOG/20260301/20260301_170010_PLD.edf", UINT64_C(0xc914174bad057d10)},
        {"DATALOG/20260301/20260301_170010_SA2.edf", UINT64_C(0x8be4cd938fc52b6d)},
    };
    for (size_t i = 0; i < sizeof(golden) / sizeof(golden[0]); i++) {
        char path[500];
        snprintf(path, sizeof(path), "%s/%s", SNAP_A, golden[i].path);
        size_t n = 0;
        uint8_t *bytes = slurp(path, &n);
        uint64_t hash = UINT64_C(14695981039346656037);
        for (size_t j = 0; bytes && j < n; j++)
            hash = (hash ^ bytes[j]) * UINT64_C(1099511628211);
        check(bytes && hash == golden[i].hash, "pre-change live export golden");
        free(bytes);
    }
}

static void write_snt(const char *path, uint8_t n_ch, uint16_t hz_x10,
                      const int16_t *frames, uint32_t count)
{
    snt_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = SNT_MAGIC;
    h.version = 2;
    h.n_channels = n_ch;
    h.sample_bytes = 2;
    h.sample_hz_x10 = hz_x10;
    h.start_epoch_ms = START_MS;
    h.sample_count = count;
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(&h, 1, sizeof(h), f) == sizeof(h));
    assert(fwrite(frames, sizeof(int16_t), (size_t)count * n_ch, f) == (size_t)count * n_ch);
    fclose(f);
}

static void iso(char *out, size_t n, int64_t ms)
{
    time_t t = (time_t)(ms / 1000);
    struct tm g;
    gmtime_r(&t, &g);
    snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", g.tm_year + 1900,
             g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, (int)(ms % 1000));
}

/* One event line.  resp != 0 → respiratory event with duration. */
static void ev(FILE *f, const char *name, int64_t dev_ms, int dur, bool resp)
{
    char t[40];
    iso(t, sizeof(t), dev_ms);
    if (resp)
        fprintf(f, "{\"params\":{\"dataId\":\"TherapyEvents-RespiratoryEvents\","
                   "\"events\":[{\"event\":\"%s\",\"reportTime\":\"%s\","
                   "\"durationSeconds\":%d,\"backdateSeconds\":0}]}}\n", name, t, dur);
    else
        fprintf(f, "{\"params\":{\"events\":[{\"event\":\"%s\",\"reportTime\":\"%s\"}]}}\n",
                name, t);
}

/* A live night in the raw store: gated 10 s after TherapyStart, ungated
 * SNT streams, AS11-clock events (NTP - drift), a live-style manifest. */
static void build_live_night(int64_t drift, const char *tz_off)
{
    sh("rm -rf '%s' '%s'; mkdir -p '%s/%s' '%s' '%s' '%s'", SD_MOUNT_POINT, SNAP_A,
       SD_STREAMS_DIR, DAY, SD_SUMMARIES_DIR, SD_SDCARD_DATALOG, SD_SDCARD_SETTINGS);
    char dir[300], p[420];
    snprintf(dir, sizeof(dir), "%s/%s", SD_STREAMS_DIR, DAY);

    static int16_t flow[BRP_N], press[BRP_N], sa2[SA2_N * 2], pld[PLD_N * 12];
    for (int i = 0; i < BRP_N; i++) {
        flow[i]  = (int16_t)((i % 200) - 100);       /* -1.00 .. +0.99 L/s */
        press[i] = (int16_t)(800 + (i % 300));       /*  8.00 .. 10.99 cmH2O */
    }
    flow[7] = 30000;                                 /* saturating spike */
    for (int i = 0; i < SA2_N; i++) {
        sa2[2 * i]     = (i % 7 == 3) ? INT16_MIN : (int16_t)((60 + i % 30) * 100);
        sa2[2 * i + 1] = (i % 5 == 0) ? INT16_MIN : (int16_t)((94 + i % 6) * 100);
    }
    for (int i = 0; i < PLD_N; i++)
        for (int c = 0; c < 12; c++) pld[i * 12 + c] = (int16_t)((i * 7 + c * 3) % 50);
    snprintf(p, sizeof(p), "%s/%s_flow.snt", dir, SID);  write_snt(p, 1, 250, flow, BRP_N);
    snprintf(p, sizeof(p), "%s/%s_press.snt", dir, SID); write_snt(p, 1, 250, press, BRP_N);
    snprintf(p, sizeof(p), "%s/%s_sa2.snt", dir, SID);   write_snt(p, 2, 10, sa2, SA2_N);
    snprintf(p, sizeof(p), "%s/%s_pld.snt", dir, SID);   write_snt(p, 12, 5, pld, PLD_N);

    snprintf(p, sizeof(p), "%s/%s_events.snt", dir, SID);
    FILE *f = fopen(p, "w");
    assert(f);
    int64_t d0 = START_MS - drift;
    ev(f, "MaskOn", d0 + 10000, 0, false);
    ev(f, "ObstructiveApneaEnd", d0 + 40000, 12, true);
    ev(f, "HypopneaEnd", d0 + 90000, 14, true);
    ev(f, "CsrStart", d0 + 100000, 0, true);
    ev(f, "CentralApneaEnd", d0 + 120000, 11, true);
    ev(f, "CsrEnd", d0 + 150000, 0, true);
    ev(f, "MaskOff", d0 + 170000, 0, false);
    fclose(f);

    snprintf(p, sizeof(p), "%s/%s_ident.json", dir, SID);
    put(p, "{\"SerialNumber\":\"22199876543\",\"PlatformIdentifier\":46,"
           "\"VariantIdentifier\":1,\"ProductName\":\"AirSense 11 AutoSet\","
           "\"SoftwareVersion\":\"SX566-0302\"}\n");
    /* The AS11 reports its own UTC offset in its settings; post_therapy feeds
     * it to as11_time, which then labels noon-days with it.  Without it the
     * exporter would infer a bogus offset from a session start (it is not a
     * noon stamp), so the live path always has it on a real card. */
    char st[300];
    snprintf(st, sizeof(st), "{\"SettingProfiles\":{\"FeatureProfiles\":{"
             "\"TimeZoneFeature\":{\"TimeZoneOffset\":\"%s\"}}}}\n", tz_off);
    snprintf(p, sizeof(p), "%s/%s_settings.json", dir, SID);
    put(p, st);
    cJSON *sj = cJSON_Parse(st);
    check(sj && as11_time_offset_from_settings(sj, NULL), "AS11 offset from settings (%s)", tz_off);
    cJSON_Delete(sj);

    char m[800];
    snprintf(m, sizeof(m),
        "{\"id\":\"%s\",\"start_epoch_ms\":%lld,\"end_epoch_ms\":%lld,"
        "\"state\":\"completed\",\"fmt\":2,\"brp_samples\":%d,\"sa2_samples\":%d,"
        "\"pld_samples\":%d,\"clock_drift_ms\":%lld,\"clock_drift_valid\":true,"
        "\"time_source\":\"ntp\"}\n",
        SID, (long long)START_MS, (long long)(START_MS + SA2_N * 1000LL),
        BRP_N, SA2_N, PLD_N, (long long)drift);
    snprintf(p, sizeof(p), "%s/%s_session.json", dir, SID);
    put(p, m);
}

/* Stage the exported card image as an IMPORT/ drop on an otherwise empty
 * device, exactly as a user copying a card would. */
static void stage_card_for_import(void)
{
    sh("rm -rf '%s'; mkdir -p '%s/DATALOG' '%s/SETTINGS' '%s' '%s' '%s' '%s'",
       SD_MOUNT_POINT, SD_IMPORT_DIR, SD_IMPORT_DIR, SD_STREAMS_DIR, SD_SUMMARIES_DIR,
       SD_SDCARD_DATALOG, SD_SDCARD_SETTINGS);
    sh("cp -a '%s/DATALOG/%s' '%s/DATALOG/'", SNAP_A, g_day, SD_IMPORT_DIR);
    sh("cp '%s/STR.edf' '%s/Identification.json' '%s/Identification.crc' '%s/'",
       SNAP_A, SNAP_A, SNAP_A, SD_IMPORT_DIR);
    sh("cp '%s'/SETTINGS/* '%s/SETTINGS/'", SNAP_A, SD_IMPORT_DIR);
}

static void same_bytes(const char *rel, bool from_header_end)
{
    char a[500], b[500];
    snprintf(a, sizeof(a), "%s/%s", SNAP_A, rel);
    snprintf(b, sizeof(b), "%s/%s", SD_SDCARD_DIR, rel);
    size_t na = 0, nb = 0;
    uint8_t *x = slurp(a, &na), *y = slurp(b, &nb);
    check(x && y, "%s missing (A=%p B=%p)", rel, (void *)x, (void *)y);
    if (x && y) {
        /* STR under clock drift: the exporter derives the header date from
         * the day's period start, which differs between a live night and a
         * re-import — compare the day records, not the header. */
        size_t off = 0;
        if (from_header_end && na > 256) {
            char t[9];
            memcpy(t, x + 184, 8); t[8] = '\0';
            off = (size_t)atol(t);
        }
        check(na == nb && memcmp(x + off, y + off, na - off) == 0,
              "%s differs between the live export (A) and the re-imported export (B): "
              "%zu vs %zu bytes", rel, na, nb);
    }
    free(x); free(y);
}

/* Names of every file in A's DATALOG/<day> (the export is the reference set) */
static int list_day_files(const char *root, char names[][80], int max)
{
    char p[500];
    snprintf(p, sizeof(p), "%s/DATALOG/%s", root, g_day);
    DIR *d = opendir(p);
    int n = 0;
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        if (e->d_name[0] == '.') continue;
        snprintf(names[n++], 80, "%s", e->d_name);
    }
    closedir(d);
    return n;
}

static void roundtrip(const char *label, const char *tz, const char *tz_off,
                      int64_t drift, bool full_str)
{
    printf("scenario: %s\n", label);
    setenv("TZ", tz, 1);
    tzset();

    /* A: live night → export */
    build_live_night(drift, tz_off);
    check(edf_gen_rebuild_day(DAY) == ESP_OK, "%s: live export failed", label);
    sh("cp -a '%s' '%s'", SD_SDCARD_DIR, SNAP_A);

    /* The export's day folder and file set are the reference */
    char a_files[16][80];
    {
        char p[500];
        snprintf(p, sizeof(p), "%s/DATALOG", SNAP_A);
        DIR *d = opendir(p);
        g_day[0] = '\0';
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL)
            if (strlen(e->d_name) == 8) { snprintf(g_day, sizeof(g_day), "%s", e->d_name); break; }
        if (d) closedir(d);
    }
    check(g_day[0] != '\0', "%s: export has a DATALOG day folder", label);
    int na_files = list_day_files(SNAP_A, a_files, 16);
    check(na_files == 5, "%s: export holds 5 session EDFs (got %d)", label, na_files);

    /* Not vacuous: the export must actually hold the events */
    for (int i = 0; i < na_files; i++) {
        size_t n = 0;
        char p[500];
        snprintf(p, sizeof(p), "%s/DATALOG/%s/%s", SNAP_A, g_day, a_files[i]);
        uint8_t *b = slurp(p, &n);
        size_t len = strlen(a_files[i]);
        if (len > 8 && !strcmp(a_files[i] + len - 8, "_EVE.edf"))
            check(b && n == 768 + 64 * 4, "%s: A's EVE.edf should hold 3 events + the start "
                                          "record (got %zu bytes)", label, n);
        if (len > 8 && !strcmp(a_files[i] + len - 8, "_CSL.edf"))
            check(b && n == 768 + 64 * 3, "%s: A's CSL.edf should hold the CSR pair + the start "
                                          "record (got %zu bytes)", label, n);
        free(b);
    }

    /* B: card image A → import → export */
    stage_card_for_import();
    edf_import_ctx_t *c = edf_import_ctx_create();
    check(c != NULL, "ctx");
    edf_import_unit_t u;
    int units = 0, imported = 0, failed = 0, skipped = 0, rejected = 0;
    char days[4][9];
    int nd = 0;
    while (c && edf_import_process_next(c, &u) == ESP_OK && units < 20) {
        units++;
        imported += u.sessions_imported; failed += u.sessions_failed;
        skipped += u.sessions_skipped; rejected += u.files_rejected;
        const char *dl[2] = { u.day_label, u.day2_label };
        for (int k = 0; k < 2; k++) {
            if (!dl[k][0]) continue;
            bool seen = false;
            for (int i = 0; i < nd; i++) seen |= !strcmp(days[i], dl[k]);
            if (!seen && nd < 4) memcpy(days[nd++], dl[k], 9);
        }
    }
    edf_import_ctx_destroy(c);
    check(imported == 1 && failed == 0 && skipped == 0 && rejected == 0,
          "%s: import: imported=%d failed=%d skipped=%d rejected=%d", label,
          imported, failed, skipped, rejected);
    check(nd == 1 && !strcmp(days[0], g_day), "%s: imported day %s (export day %s)", label,
          nd ? days[0] : "(none)", g_day);
    check(edf_import_pending(), "%s: native STR rebuild remains pending", label);
    for (int i = 0; i < nd; i++)
        check(edf_gen_rebuild_day(days[i]) == ESP_OK, "%s: re-export of %s failed", label, days[i]);
    check(unlink(SD_SUMMARIES_DIR "/native-str.needs_rebuild") == 0,
          "%s: clear completed test rebuild marker", label);
    check(!edf_import_pending(), "%s: inbox drained after rebuild", label);

    /* A == B: same file set, same bytes */
    char b_files[16][80];
    int nb_files = list_day_files(SD_SDCARD_DIR, b_files, 16);
    check(nb_files == na_files, "%s: re-export holds %d session EDFs, want %d", label,
          nb_files, na_files);
    for (int i = 0; i < na_files; i++) {
        char rel[160];
        snprintf(rel, sizeof(rel), "DATALOG/%s/%s", g_day, a_files[i]);
        same_bytes(rel, false);
    }
    same_bytes("Identification.json", false);
    same_bytes("Identification.crc", false);
    same_bytes("SETTINGS/CurrentSettings.json", false);
    same_bytes("SETTINGS/CurrentSettings.crc", false);
    same_bytes("STR.edf", !full_str);
}

static int as10_sample(const uint8_t *bytes, size_t len, const char *label, int record)
{
    if (!bytes || len < 256) return INT_MIN;
    char text[9] = {0};
    memcpy(text, bytes + 252, 4);
    int ns = atoi(text);
    memcpy(text, bytes + 184, 8);
    int header = atoi(text);
    if (ns <= 0 || header < 256 || (size_t)header > len) return INT_MIN;
    int stride = 0, offset = -1;
    for (int i = 0; i < ns; i++) {
        char name[17];
        memcpy(name, bytes + 256 + i * 16, 16);
        name[16] = '\0';
        for (int j = 15; j >= 0 && name[j] == ' '; j--) name[j] = '\0';
        if (strcmp(name, label) == 0) offset = stride;
        memcpy(text, bytes + 256 + ns * 216 + i * 8, 8);
        stride += atoi(text) * 2;
    }
    size_t pos = (size_t)header + (size_t)record * stride + offset;
    if (offset < 0 || pos + 2 > len) return INT_MIN;
    int16_t value;
    memcpy(&value, bytes + pos, 2);
    return value;
}

static int signal_index(const uint8_t *bytes, size_t len, const char *label, int *ns_out)
{
    if (!bytes || len < 256) return -1;
    char text[5] = {0};
    memcpy(text, bytes + 252, 4);
    int ns = atoi(text);
    if (ns <= 0 || (size_t)(256 + ns * 256) > len) return -1;
    *ns_out = ns;
    for (int i = 0; i < ns; i++) {
        char name[17];
        memcpy(name, bytes + 256 + i * 16, 16);
        name[16] = '\0';
        for (int j = 15; j >= 0 && name[j] == ' '; j--) name[j] = '\0';
        if (!strcmp(name, label)) return i;
    }
    return -1;
}

static bool signal_headers_match(const uint8_t *a, size_t alen,
                                 const uint8_t *b, size_t blen,
                                 const char *label)
{
    int an = 0, bn = 0;
    int ai = signal_index(a, alen, label, &an);
    int bi = signal_index(b, blen, label, &bn);
    if (ai < 0 || bi < 0) return false;
    static const int offsets[] = {96, 104, 112, 120, 128, 216};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
        if (memcmp(a + 256 + offsets[i] * an + ai * 8,
                   b + 256 + offsets[i] * bn + bi * 8, 8)) return false;
    return true;
}

/* Exercise every field in the supported family tables through real EDF
 * parsing, native daily persistence and EDF generation. */
static void native_field_matrix(bool as10)
{
    setenv("TZ", "UTC", 1);
    tzset();
    sh("rm -rf '%s'; mkdir -p '%s' '%s' '%s'",
       SD_MOUNT_POINT, SD_IMPORT_DIR, SD_STREAMS_DIR, SD_SUMMARIES_DIR);
    edf_signal_def_t signals[74];
    int16_t values[74];
    int n = 0;
    signals[n] = g_str_signals[0];
    values[n++] = (int16_t)as11_time_day_number("20260301");
    signals[n] = g_str_signals[4];
    values[n++] = 120;
    int fields = as10 ? (int)(sizeof(g_as10_setting_fields) / sizeof(g_as10_setting_fields[0]))
                      : 71;
    for (int i = 0; i < fields; i++) {
        int idx = as10 ? g_as10_setting_fields[i].str_index : i + 5;
        signals[n] = g_str_signals[idx];
        if (as10) signals[n].label = g_as10_setting_fields[i].label;
        values[n] = idx == 5 ? 2 :
            (int16_t)((signals[n].dig_min + signals[n].dig_max) / 2);
        n++;
    }
    char source[400];
    snprintf(source, sizeof(source), "%s/STR.edf", SD_IMPORT_DIR);
    FILE *f = fopen(source, "wb");
    check(f != NULL, "create synthetic settings matrix");
    if (!f) return;
    const char *recording = as10 ? "SRN=matrix10 MID=36 VID=1"
                                 : "SRN=matrix11 MID=46 VID=1";
    check(edf_write_header(f, "X X X X", recording, "01.03.26", "12.00.00",
                           1, "86400", "EDF", signals, n) > 0,
          "write settings matrix header");
    uint16_t crc = edf_crc16_ccitt((const uint8_t *)values, (size_t)n * 2);
    fwrite(values, 2, (size_t)n, f);
    fwrite(&crc, 2, 1, f);
    fclose(f);
    size_t source_len = 0;
    uint8_t *source_bytes = slurp(source, &source_len);
    edf_import_ctx_t *ctx = edf_import_ctx_create();
    edf_import_unit_t unit;
    check(ctx && edf_import_process_next(ctx, &unit) == ESP_OK &&
          unit.settings_written == 1 && !unit.root_retained,
          "import settings matrix");
    edf_import_ctx_destroy(ctx);
    check(edf_gen_rebuild_native_str() == ESP_OK, "export settings matrix");
    size_t generated_len = 0;
    uint8_t *generated = slurp(SD_SDCARD_DIR "/STR.edf", &generated_len);
    for (int i = 2; i < n; i++) {
        check(as10_sample(generated, generated_len, signals[i].label, 0) == values[i],
              "%s %s round-trip value", as10 ? "AS10" : "AS11", signals[i].label);
        check(signal_headers_match(source_bytes, source_len, generated, generated_len,
                                   signals[i].label),
              "%s %s EDF header", as10 ? "AS10" : "AS11", signals[i].label);
    }
    free(source_bytes);
    free(generated);
}

static void write_history_spool(int64_t noon_ms, char path[400])
{
    time_t sec = (time_t)(noon_ms / 1000);
    struct tm date;
    gmtime_r(&sec, &date);
    char label[16];
    strftime(label, sizeof(label), "%Y%m%d", &date);
    snprintf(path, 400, "%s/%s.spool", SD_SUMMARIES_DIR, label);
    FILE *f = fopen(path, "wb");
    check(f != NULL, "create history spool");
    if (!f) return;
    const uint64_t vals[] = {
        (uint64_t)SUM_F_PERIOD_START << 3, (uint64_t)noon_ms,
        (uint64_t)SUM_F_PERIOD_END << 3, (uint64_t)(noon_ms + 86400000LL)
    };
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        uint64_t v = vals[i];
        while (v >= 128) { fputc((int)((v & 127) | 128), f); v >>= 7; }
        fputc((int)v, f);
    }
    fclose(f);
}

static void history_limits(void)
{
    setenv("TZ", "UTC", 1);
    tzset();
    sh("rm -rf '%s'; mkdir -p '%s' '%s' '%s'",
       SD_MOUNT_POINT, SD_SUMMARIES_DIR, SD_STREAMS_DIR, SD_SDCARD_DIR);
    put(SD_SUMMARIES_DIR "/native.ident.json",
        "{\"SerialNumber\":\"fixture11\",\"PlatformIdentifier\":46,"
        "\"_somnotrace_native_source\":\"native_str\"}");
    const int64_t start = 1577880000000LL; /* 2020-01-01 12:00 UTC */
    char path[400];
    for (int i = 0; i < 367; i++)
        write_history_spool(start + (int64_t)i * 86400000LL, path);
    check(edf_gen_rebuild_native_str() == ESP_OK, "export 367 spool days");
    size_t len = 0;
    uint8_t *bytes = slurp(SD_SDCARD_DIR "/STR.edf", &len);
    char count[9] = {0};
    if (bytes && len >= 244) memcpy(count, bytes + 236, 8);
    check(atoi(count) == 365 &&
          as10_sample(bytes, len, "Date", 0) == as11_time_day_number("20200103") &&
          as10_sample(bytes, len, "Date", 364) == as11_time_day_number("20210101"),
          "365-day selection keeps the newest distinct days");
    host_heap_caps_calloc_fail = 1;
    check(edf_gen_rebuild_native_str() != ESP_OK, "PSRAM exhaustion aborts STR generation");
    host_heap_caps_calloc_fail = 0;
    size_t failed_len = 0;
    uint8_t *failed_bytes = slurp(SD_SDCARD_DIR "/STR.edf", &failed_len);
    check(bytes && failed_bytes && len == failed_len && !memcmp(bytes, failed_bytes, len),
          "allocation failure preserves previous STR");
    free(failed_bytes);
    host_heap_caps_realloc_fail = 1;
    check(edf_gen_rebuild_native_str() != ESP_OK, "spool index allocation failure aborts");
    host_heap_caps_realloc_fail = 0;
    failed_bytes = slurp(SD_SDCARD_DIR "/STR.edf", &failed_len);
    check(bytes && failed_bytes && len == failed_len && !memcmp(bytes, failed_bytes, len),
          "spool index failure preserves previous STR");
    free(failed_bytes);
    char saved_spool[440];
    snprintf(path, sizeof(path), "%s/20210101.spool", SD_SUMMARIES_DIR);
    snprintf(saved_spool, sizeof(saved_spool), "%s.test-backup", path);
    check(rename(path, saved_spool) == 0, "save test spool before corruption");
    FILE *bad = fopen(path, "wb");
    check(bad != NULL, "create truncated spool");
    if (bad) { fputc(0x80, bad); fclose(bad); }
    check(edf_gen_rebuild_native_str() != ESP_OK, "truncated spool aborts generation");
    failed_bytes = slurp(SD_SDCARD_DIR "/STR.edf", &failed_len);
    check(bytes && failed_bytes && len == failed_len && !memcmp(bytes, failed_bytes, len),
          "truncated spool preserves previous STR");
    free(failed_bytes);
    write_history_spool(start + 366LL * 86400000LL, path);
    bad = fopen(path, "ab");
    check(bad != NULL, "open nested-corruption fixture");
    if (bad) {
        const uint8_t nested[] = {0x32, 0x02, 0x0a, 0x7f};
        fwrite(nested, 1, sizeof(nested), bad);
        fclose(bad);
    }
    check(edf_gen_rebuild_native_str() != ESP_OK, "invalid nested session length aborts");
    failed_bytes = slurp(SD_SDCARD_DIR "/STR.edf", &failed_len);
    check(bytes && failed_bytes && len == failed_len && !memcmp(bytes, failed_bytes, len),
          "nested spool corruption preserves previous STR");
    free(failed_bytes);
    check(unlink(path) == 0 && rename(saved_spool, path) == 0,
          "restore test-owned spool");
    free(bytes);
    for (int i = 0; i < 367; i++) {
        time_t sec = (time_t)((start + (int64_t)i * 86400000LL) / 1000);
        struct tm date;
        gmtime_r(&sec, &date);
        char label[16];
        strftime(label, sizeof(label), "%Y%m%d", &date);
        snprintf(path, sizeof(path), "%s/%s.spool", SD_SUMMARIES_DIR, label);
        check(unlink(path) == 0, "remove test-owned history spool");
    }
    write_history_spool(start, path);
    write_history_spool(start + (EDF_STR_MAX_RECORDS - 1LL) * 86400000LL, path);
    check(edf_gen_rebuild_native_str() == ESP_OK, "maximum calendar span is accepted");
    bytes = slurp(SD_SDCARD_DIR "/STR.edf", &len);
    memset(count, 0, sizeof(count));
    if (bytes && len >= 244) memcpy(count, bytes + 236, 8);
    check(atoi(count) == EDF_STR_MAX_RECORDS, "calendar gaps are padded to the limit");
    check(unlink(path) == 0, "remove test-owned last spool");
    write_history_spool(start + (int64_t)EDF_STR_MAX_RECORDS * 86400000LL, path);
    check(edf_gen_rebuild_native_str() != ESP_OK, "span over the limit is rejected");
    size_t after_len = 0;
    uint8_t *after = slurp(SD_SDCARD_DIR "/STR.edf", &after_len);
    check(bytes && after && len == after_len && !memcmp(bytes, after, len),
          "over-limit span leaves previous export untouched");
    free(after);
    free(bytes);
}

static void as11_native_roundtrip(void)
{
    setenv("TZ", "UTC", 1);
    tzset();
    sh("rm -rf '%s'; mkdir -p '%s' '%s' '%s'",
       SD_MOUNT_POINT, SD_IMPORT_DIR, SD_STREAMS_DIR, SD_SUMMARIES_DIR);
    const edf_signal_def_t signals[] = {
        {"Date", "", "", 0, 24836, 0, 24836, "", 1},
        {"MaskOn", "", "min", 0, 1440, 0, 1440, "", 2},
        {"MaskOff", "", "min", 0, 1440, 0, 1440, "", 2},
        {"Duration", "", "min", 0, 1440, 0, 1440, "", 1},
        {"Mode", "", "", 0, 16, 0, 16, "", 1},
        {"S.C.Press", "", "cmH2O", 0, 30, 0, 3000, "", 1},
        {"S.A.MinPress", "", "cmH2O", 0, 30, 0, 300, "", 1},
        {"S.A.MaxPress", "", "cmH2O", 0, 30, 0, 300, "", 1},
        {"S.EPR.Level", "", "cmH2O", 0, 3, 0, 300, "", 1},
        {"S.EPR.EPREnable", "", "", 0, 16, 0, 16, "", 1},
        {"S.RampEnable", "", "", 0, 16, 0, 16, "", 1},
        {"S.RampTime", "", "min", 0, 60, 0, 60, "", 1},
        {"S.HumLevel", "", "bad-unit", 0, 8, 0, 8, "", 1},
    };
    const int16_t rows[][15] = {
        {(int16_t)as11_time_day_number("20260301"), 600, 800, 660, 860,
         120, 3, 850, -1, -1, 200, 2, 2, 20, 6},
        {(int16_t)as11_time_day_number("20260302"), 700, -1, 760, -1,
         60, 1, -1, 70, 150, 100, 1, 3, -1, -1},
        {(int16_t)as11_time_day_number("20260303"), -1, -1, -1, -1,
         0, 3, 875, -1, -1, -1, -1, -1, -1, -1},
    };
    const char *source = SD_IMPORT_DIR "/STR.edf";
    const char *copy = SD_MOUNT_POINT "/original-str.edf";
    FILE *f = fopen(source, "wb");
    check(f != NULL, "AS11 fixture source");
    if (!f) return;
    check(edf_write_header(f, "X X X X",
          "Startdate 01-MAR-2026 X X X SRN=fixture11 MID=46 VID=1",
          "01.03.26", "12.00.00", 3, "86400", "EDF",
          signals, (int)(sizeof(signals) / sizeof(signals[0]))) > 0,
          "write AS11 STR header");
    for (int i = 0; i < 3; i++) {
        uint16_t crc = edf_crc16_ccitt((const uint8_t *)rows[i], sizeof(rows[i]));
        fwrite(rows[i], 1, sizeof(rows[i]), f);
        fwrite(&crc, 1, sizeof(crc), f);
    }
    fclose(f);
    sh("cp '%s' '%s'", source, copy);
    edf_import_ctx_t *ctx = edf_import_ctx_create();
    edf_import_unit_t unit;
    check(ctx && edf_import_process_next(ctx, &unit) == ESP_OK &&
          unit.settings_written == 3 && unit.spools_written == 3 && !unit.root_retained,
          "import independent AS11 daily records");
    edf_import_ctx_destroy(ctx);
    check(edf_import_pending(), "rebuild request survives context destruction");
    check(edf_gen_rebuild_native_str() == ESP_OK, "publish AS11 STR with no session");
    size_t len = 0;
    uint8_t *bytes = slurp(SD_SDCARD_DIR "/STR.edf", &len);
    check(as10_sample(bytes, len, "Mode", 0) == 3 &&
          as10_sample(bytes, len, "S.C.Press", 0) == 425 &&
          as10_sample(bytes, len, "Mode", 1) == 1 &&
          as10_sample(bytes, len, "S.A.MinPress", 1) == 350 &&
          as10_sample(bytes, len, "S.A.MaxPress", 1) == 750,
          "daily AS11 modes and pressures use source physical scaling");
    check(as10_sample(bytes, len, "S.EPR.Level", 0) == 100 &&
          as10_sample(bytes, len, "S.EPR.EPREnable", 0) == 2 &&
          as10_sample(bytes, len, "S.RampEnable", 0) == 2 &&
          as10_sample(bytes, len, "S.RampTime", 0) == 20,
          "AS11 EPR and ramp settings retained");
    check(as10_sample(bytes, len, "S.HumLevel", 0) == -1 &&
          as10_sample(bytes, len, "S.C.Press", 1) == -1 &&
          as10_sample(bytes, len, "S.RampTime", 1) == -1,
          "unsupported and missing settings stay unknown");
    check(as10_sample(bytes, len, "S.C.Press", 2) == 438,
          "AS11 pressure quantizes to within half a destination step");
    check(as10_sample(bytes, len, "MaskOn", 0) == 600 &&
          as10_sample(bytes, len, "MaskOff", 0) == 660 &&
          as10_sample(bytes, len, "MaskOn", 1) == 700 &&
          as10_sample(bytes, len, "MaskOff", 1) == 760,
          "native mask timing remains unchanged");
    f = fopen(SD_SUMMARIES_DIR "/20260302.native.settings.json", "wb");
    check(f != NULL, "create corrupt synthetic native day");
    if (f) {
        for (int i = 0; i < 512; i++) fputc(0, f);
        fputs("damaged", f);
        fclose(f);
    }
    check(edf_gen_rebuild_native_str() != ESP_OK, "corrupt day blocks publication");
    size_t blocked_len = 0;
    uint8_t *blocked = slurp(SD_SDCARD_DIR "/STR.edf", &blocked_len);
    check(bytes && blocked && len == blocked_len && !memcmp(bytes, blocked, len),
          "failed rebuild preserves the published STR");
    free(blocked);
    sh("cp '%s' '%s'", copy, source);
    ctx = edf_import_ctx_create();
    check(ctx && edf_import_process_next(ctx, &unit) == ESP_OK &&
          !unit.root_retained && unit.settings_written == 1,
          "reimport reconstructs only the corrupt day");
    edf_import_ctx_destroy(ctx);
    check(edf_gen_rebuild_native_str() == ESP_OK, "publish after native day recovery");
    size_t recovered_len = 0;
    uint8_t *recovered = slurp(SD_SDCARD_DIR "/STR.edf", &recovered_len);
    check(bytes && recovered && len == recovered_len && !memcmp(bytes, recovered, len),
          "recovered STR preserves all settings and mask times byte-for-byte");
    free(recovered);
    check(access(SD_SDCARD_SETTINGS "/CurrentSettings.json", F_OK) != 0,
          "historical settings do not create CurrentSettings");
    check(rename(SD_SDCARD_DIR "/Identification.json",
                 SD_SDCARD_DIR "/Identification.json.replace-backup") == 0 &&
          rename(SD_SDCARD_DIR "/STR.edf",
                 SD_SDCARD_DIR "/STR.edf.replace-backup") == 0 &&
          unlink(SD_SDCARD_DIR "/Identification.crc") == 0,
          "simulate interrupted root publication");
    check(edf_gen_rebuild_native_str() == ESP_OK,
          "restart recovers root files and missing identity checksum");
    size_t ident_len = 0, crc_len = 0;
    uint8_t *ident_bytes = slurp(SD_SDCARD_DIR "/Identification.json", &ident_len);
    uint8_t *crc_bytes = slurp(SD_SDCARD_DIR "/Identification.crc", &crc_len);
    uint32_t crc = ident_bytes ? edf_crc32_ieee(ident_bytes, ident_len) : 0;
    check(crc_bytes && crc_len == 4 && crc_bytes[0] == (crc & 0xff) &&
          crc_bytes[1] == ((crc >> 8) & 0xff) &&
          crc_bytes[2] == ((crc >> 16) & 0xff) &&
          crc_bytes[3] == ((crc >> 24) & 0xff),
          "recovered identity checksum matches published JSON");
    free(ident_bytes);
    free(crc_bytes);
    put(SD_SDCARD_DIR "/Identification.crc", "bad");
    check(edf_gen_rebuild_native_str() != ESP_OK,
          "invalid existing identity checksum is not silently rewritten");
    check(unlink(SD_SDCARD_DIR "/Identification.crc") == 0 &&
          edf_gen_rebuild_native_str() == ESP_OK,
          "a missing checksum is repaired on retry");
    check(unlink(SD_SUMMARIES_DIR "/native-str.needs_rebuild") == 0,
          "clear completed test rebuild");
    sh("cp '%s' '%s'", copy, source);
    ctx = edf_import_ctx_create();
    check(ctx && edf_import_process_next(ctx, &unit) == ESP_OK &&
          !unit.root_retained && unit.settings_written == 0 && unit.spools_written == 0,
          "unchanged AS11 import does not rewrite daily evidence");
    edf_import_ctx_destroy(ctx);
    check(!edf_import_pending(), "unchanged AS11 import does not request rebuild");
    sh("mkdir -p '%s/20260304'", SD_STREAMS_DIR);
    check(edf_gen_rebuild_native_str() == ESP_OK,
          "empty streams directories do not block native-only export");
    sh("mkdir -p '%s/20260301'", SD_STREAMS_DIR);
    put(SD_STREAMS_DIR "/20260301/test_session.json",
        "{\"start_epoch_ms\":1772402400000,\"end_epoch_ms\":1772406000000}");
    put(SD_STREAMS_DIR "/20260301/test_ident.json",
        "{\"SerialNumber\":\"foreign\",\"PlatformIdentifier\":46}");
    check(edf_gen_rebuild_day_for_native("20260301") != ESP_OK,
          "native rebuild refuses a session with a different identity");
    check(edf_gen_rebuild_native_str() != ESP_OK,
          "STR-only refresh refuses a different session identity");
    put(SD_STREAMS_DIR "/20260301/test_ident.json",
        "{\"SerialNumber\":\"fixture11\",\"PlatformIdentifier\":46}");
    sh("mkdir -p '%s/20260301'", SD_SDCARD_DATALOG);
    put(SD_SDCARD_DATALOG "/20260301/keep.edf", "unchanged test waveform");
    check(edf_gen_rebuild_native_str() == ESP_OK,
          "STR-only refresh uses the latest session");
    size_t wave_len = 0;
    uint8_t *wave = slurp(SD_SDCARD_DATALOG "/20260301/keep.edf", &wave_len);
    check(wave && wave_len == strlen("unchanged test waveform") &&
          !memcmp(wave, "unchanged test waveform", wave_len),
          "STR-only refresh leaves DATALOG files untouched");
    free(wave);
    size_t refreshed_len = 0;
    uint8_t *refreshed = slurp(SD_SDCARD_DIR "/STR.edf", &refreshed_len);
    check(as10_sample(refreshed, refreshed_len, "MaskOn", 0) == 600 &&
          as10_sample(refreshed, refreshed_len, "MaskOff", 0) == 660 &&
          as10_sample(refreshed, refreshed_len, "S.C.Press", 0) == 425,
          "STR-only refresh preserves existing daily timing and settings");
    free(bytes);
    bytes = refreshed;
    len = refreshed_len;
    put(SD_SDCARD_DIR "/Identification.json",
        "{\"FlowGenerator\":{\"IdentificationProfiles\":{\"Product\":"
        "{\"SerialNumber\":\"foreign\"},\"Software\":{\"PlatformIdentifier\":46}}}}");
    check(edf_gen_rebuild_day_for_native("20260301") != ESP_OK,
          "native day rebuild refuses another machine's exported card");
    check(edf_gen_rebuild_native_str() != ESP_OK,
          "native-only export refuses another machine's card identity");
    size_t after_len = 0;
    uint8_t *after = slurp(SD_SDCARD_DIR "/STR.edf", &after_len);
    check(bytes && after && len == after_len && !memcmp(bytes, after, len),
          "identity conflict leaves previous STR untouched");
    free(after);
    free(bytes);
}

/* Independent synthetic Air 10 card -> importer -> actual STR generator. */
static void as10_roundtrip(void)
{
    setenv("TZ", "UTC", 1);
    tzset();
    sh("rm -rf '%s'; mkdir -p '%s' '%s' '%s'",
       SD_MOUNT_POINT, SD_IMPORT_DIR, SD_STREAMS_DIR, SD_SUMMARIES_DIR);
    const edf_signal_def_t signals[] = {
        {"Date", "", "", 0, 24836, 0, 24836, "", 1},
        {"Duration", "", "min.", 0, 1440, 0, 1440, "", 1},
        {"Mode", "", "", 0, 16, 0, 16, "", 1},
        {"S.C.StartPress", "", "cmH2O", 0, 30, 0, 3000, "", 1},
        {"S.C.Press", "", "cmH2O", 0, 30, 0, 300, "", 1},
        {"S.AS.MinPress", "", "cmH2O", 0, 30, 0, 300, "", 1},
        {"S.VA.MinEPAP", "", "cmH2O", 0, 30, 0, 300, "", 1},
        {"S.BL.IPAP", "", "cmH2O", 0, 30, 0, 300, "", 1},
        {"S.BL.EPAP", "", "cmH2O", 0, 30, 0, 300, "", 1},
        {"S.EasyBreathe", "", "", 0, 16, 0, 16, "", 1},
        {"S.RiseTime", "", "ms", 0, 2000, 0, 2000, "", 1},
        {"S.TiMax", "", "ms", 0, 4000, 0, 4000, "", 1},
        {"S.TiMin", "", "unknown", 0, 4000, 0, 4000, "", 1},
        {"S.Trigger", "", "", 0, 16, 0, 16, "", 1},
        {"S.Cycle", "", "", 0, 16, 0, 16, "", 1},
        {"S.Mask", "", "", 0, 16, 0, 16, "", 1},
        {"HeatedTube", "", "", 0, 16, 0, 16, "", 1},
    };
    char source[400], out[400];
    snprintf(source, sizeof(source), "%s/STR.edf", SD_IMPORT_DIR);
    snprintf(out, sizeof(out), "%s/as10-export", SD_MOUNT_POINT);
    FILE *f = fopen(source, "wb");
    check(f != NULL, "AS10 fixture source");
    if (!f) return;
    int header = edf_write_header(f, "X X X X", "Startdate 27-FEB-2026 X X X SRN=fixture10 MID=36 VID=1",
                                  "27.02.26", "12.00.00", 5, "86400", "EDF",
                                  signals, (int)(sizeof(signals) / sizeof(signals[0])));
    check(header > 0, "write AS10 STR header");
    int16_t rows[5][17] = {
        {(int16_t)as11_time_day_number("20260227"), 120, 0, 801, 90, 80, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 2},
        {(int16_t)as11_time_day_number("20260228"), 120, 3, -1, -1, -1, -1, 100, 65, 1, 300, 1200, 400, 2, 1, 1, 3},
        {(int16_t)as11_time_day_number("20260301"), 120, 6, -1, -1, -1, 115, -1, -1, -1, -1, 1200, 400, 2, 1, 2, 3},
        {(int16_t)as11_time_day_number("20260302"), 120, 11, -1, -1, 80, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 2},
        {(int16_t)as11_time_day_number("20260303"), 120, 2, -1, -1, -1, -1, 100, 65, -1, 300, -1, -1, -1, -1, 0, 2},
    };
    for (int r = 0; r < 5; r++) {
        uint16_t crc = edf_crc16_ccitt((const uint8_t *)rows[r], sizeof(rows[r]));
        fwrite(rows[r], 1, sizeof(rows[r]), f);
        fwrite(&crc, 1, sizeof(crc), f);
    }
    fclose(f);
    edf_import_ctx_t *ctx = edf_import_ctx_create();
    edf_import_unit_t unit;
    check(ctx != NULL && edf_import_process_next(ctx, &unit) == ESP_OK &&
          unit.settings_written == 5, "import AS10 STR daily settings");
    cJSON *metadata = NULL;
    char meta_path[400];
    snprintf(meta_path, sizeof(meta_path), "%s", SD_DEVICE_METADATA_PATH);
    size_t meta_len = 0;
    uint8_t *meta_bytes = slurp(meta_path, &meta_len);
    if (meta_bytes) {
        char *text = malloc(meta_len + 1);
        if (text) {
            memcpy(text, meta_bytes, meta_len);
            text[meta_len] = '\0';
            metadata = cJSON_Parse(text);
            free(text);
        }
        free(meta_bytes);
    }
    check(metadata && !cJSON_GetObjectItemCaseSensitive(metadata, "settings"),
          "historical STR does not become current settings");
    cJSON_Delete(metadata);
    edf_import_ctx_destroy(ctx);
    check(edf_gen_rebuild_native_str() == ESP_OK,
          "publish native-only STR without a local session");
    size_t native_len = 0;
    uint8_t *native_bytes = slurp(SD_SDCARD_DIR "/STR.edf", &native_len);
    check(as10_sample(native_bytes, native_len, "S.C.Press", 0) == 450 &&
          as10_sample(native_bytes, native_len, "S.BL.IPAP", 1) == 500,
          "native-only export preserves multi-day AS10 settings");
    free(native_bytes);
    char session_dir[400], manifest[480];
    snprintf(session_dir, sizeof(session_dir), "%s/20260301", SD_STREAMS_DIR);
    mkdir(session_dir, 0775);
    snprintf(manifest, sizeof(manifest), "%s/%s_session.json", session_dir, SID);
    FILE *mf = fopen(manifest, "w");
    check(mf != NULL, "create verified AS10 session");
    if (mf) {
        fputs("{}", mf);
        fclose(mf);
    }
    mkdir(out, 0775);
    cJSON *ident = cJSON_Parse("{\"SerialNumber\":\"fixture10\",\"PlatformIdentifier\":36}");
    check(ident != NULL && edf_generate_str_edf(out, "X X X X",
          "Startdate 01-MAR-2026 X X X SRN=fixture10 MID=36 VID=1", "01.03.26",
          NULL, ident, SD_STREAMS_DIR "/20260301", SID,
          START_MS, START_MS + 7200000, 0) == ESP_OK, "generate AS10 STR");
    cJSON_Delete(ident);
    char generated[450];
    snprintf(generated, sizeof(generated), "%s/STR.edf", out);
    size_t len = 0;
    uint8_t *bytes = slurp(generated, &len);
    check(as10_sample(bytes, len, "Mode", 0) == 0 &&
          as10_sample(bytes, len, "Mode", 1) == 3 &&
          as10_sample(bytes, len, "Mode", 2) == 6, "AS10 mode indices");
    check(as10_sample(bytes, len, "S.C.Press", 0) == 450,
          "AS10 pressure uses physical header scaling");
    check(as10_sample(bytes, len, "S.C.StartPress", 0) == -1,
          "AS10 unrepresentable pressure remains unknown");
    check(as10_sample(bytes, len, "S.AS.MinPress", 0) == 400,
          "AS10 AutoSet label and pressure");
    check(as10_sample(bytes, len, "Mode", 3) == 11 &&
          as10_sample(bytes, len, "S.AS.MinPress", 3) == 400 &&
          as10_sample(bytes, len, "S.AFH.MinPress", 3) == 400 &&
          as10_sample(bytes, len, "S.AFH.MaxPress", 3) == -1,
          "AS10 for Her exposes sourced AutoSet settings to OSCAR");
    check(as10_sample(bytes, len, "Mode", 4) == 2 &&
          as10_sample(bytes, len, "S.BL.IPAP", 4) == 500,
          "AS10 timed mode retains native mode and pressure");
    check(as10_sample(bytes, len, "S.VA.MinEPAP", 2) == 575,
          "AS10 VAuto pressure");
    check(as10_sample(bytes, len, "S.BL.IPAP", 1) == 500 &&
          as10_sample(bytes, len, "S.BL.EPAP", 1) == 325 &&
          as10_sample(bytes, len, "S.RiseTime", 1) == 300,
          "AS10 S-mode settings and rise time");
    check(as10_sample(bytes, len, "S.TiMax", 2) == 60 &&
          as10_sample(bytes, len, "S.TiMin", 2) == -1,
          "AS10 duration units and unknown unit");
    check(as10_sample(bytes, len, "S.Trigger", 1) == 2 &&
          as10_sample(bytes, len, "S.Cycle", 1) == 1 &&
          as10_sample(bytes, len, "S.Mask", 0) == 0 &&
          as10_sample(bytes, len, "S.Mask", 2) == 2, "AS10 native enum values");
    check(as10_sample(bytes, len, "HeatedTube", 0) == 2 &&
          as10_sample(bytes, len, "HeatedTube", 1) == 3,
          "AS10 spool-derived signals");
    if (bytes && len >= 256) {
        char t[9] = {0};
        memcpy(t, bytes + 184, 8);
        size_t hdr = (size_t)atoi(t);
        size_t stride = len >= hdr ? (len - hdr) / 5 : 0;
        for (int r = 0; r < 5 && stride >= 2; r++) {
            const uint8_t *rec = bytes + hdr + (size_t)r * stride;
            uint16_t stored;
            memcpy(&stored, rec + stride - 2, 2);
            check(stored == edf_crc16_ccitt(rec, stride - 2), "AS10 STR CRC");
        }
    }
    free(bytes);
    ident = cJSON_Parse("{\"SerialNumber\":\"fixture10\",\"PlatformIdentifier\":46}");
    check(ident != NULL && edf_generate_str_edf(out, "X X X X",
          "Startdate 01-MAR-2026 X X X SRN=fixture10 MID=46 VID=1", "01.03.26",
          NULL, ident, SD_STREAMS_DIR "/20260301", SID,
          START_MS, START_MS + 7200000, 0) == ESP_OK, "generate different-family STR");
    cJSON_Delete(ident);
    bytes = slurp(generated, &len);
    check(as10_sample(bytes, len, "Mode", 0) == -1 &&
          as10_sample(bytes, len, "S.C.Press", 0) == -1,
          "AS10 snapshot must not be used under an AS11 identity");
    free(bytes);
}

int main(void)
{
    roundtrip("no clock drift", "UTC", "+00:00", 0, true);
    roundtrip("AS11 clock slow by 214 s", "UTC", "+00:00", 214000, false);
    roundtrip("AS11 clock fast by 508 s", "UTC", "+00:00", -508150, false);
    /* EDF names/headers carry LOCAL civil time; the importer converts them
     * back with mktime(), so a non-UTC zone (with DST) must round-trip too */
    /* (STR header date: the live fixture has no spool, so the exporter
     * synthesises the day's record anchored at the session start — for
     * Melbourne that is the next calendar date, whereas the importer's
     * spool carries the true noon PeriodStart.  Day records are compared.) */
    roundtrip("Melbourne (AEDT)", "Australia/Melbourne", "+11:00", 0, false);
    roundtrip("Melbourne (AEDT), clock slow by 214 s", "Australia/Melbourne", "+11:00", 214000, false);
    roundtrip("New York (EST)", "America/New_York", "-05:00", 0, true);
    check_live_golden();
    as11_native_roundtrip();
    as10_roundtrip();
    native_field_matrix(false);
    native_field_matrix(true);
    history_limits();
    printf("edf_roundtrip_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
