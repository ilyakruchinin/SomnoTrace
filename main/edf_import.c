/*
 * SomnoTrace - EDF → SNT import engine (historical ResMed card imports)
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

/* ────────────────────────────────────────────────────────────────────
 *  See edf_import.h and .ai/IMPORT-EDFs.md for the architecture.
 *
 *  Timestamp domains (same convention as the live pipeline):
 *   - "device domain" / t_dev_ms: the source CPAP's clock, expressed as a
 *     UTC-epoch number.  Filename prefixes and EDF header startdate/
 *     starttime are the device's *local civil* time, so they are parsed
 *     with mktime() in the ESP's configured timezone — the same inverse
 *     used by as11_time_format_session_prefix()/format_edf_datetime(),
 *     which keeps import→export a byte-stable round-trip whenever the
 *     ESP zone matches the source device's zone.
 *   - "NTP domain" / start_epoch_ms: t_dev_ms + clock_drift_ms.
 *     Manual/FTP imports have no drift measurement: drift is 0 with
 *     valid/usable = false.  SomnoBridge imports carry the measured
 *     offset from <day>/_bridge_drift.json.
 *   - Noon-day stream folders are labelled from t_dev_ms (raw device
 *     time), never from the drifted NTP time.
 * ──────────────────────────────────────────────────────────────────── */

#include "edf_import.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "cJSON.h"
#include "sd_storage.h"
#include "snt_format.h"
#include "as11_time.h"

#ifndef EDF_IMPORT_HOST
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "psram_task.h"
#include "edf_gen.h"
#include "somno_ml.h"
#include "uploader.h"
#include "ftp.h"
#endif

static const char *TAG = "edf_import";

/* PSRAM-first heap helpers — keep large EDF/group/histogram buffers out of
 * scarce internal SRAM on target while compiling with plain libc on host. */
#ifdef EDF_IMPORT_HOST
static inline void *imp_malloc(size_t sz) { return malloc(sz); }
static inline void *imp_calloc(size_t n, size_t sz) { return calloc(n, sz); }
static inline void *imp_realloc(void *p, size_t sz) { return realloc(p, sz); }
#else
static inline void *imp_malloc(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(sz);
}
static inline void *imp_calloc(size_t n, size_t sz)
{
    void *p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(n, sz);
}
static inline void *imp_realloc(void *p, size_t sz)
{
    void *r = heap_caps_realloc(p, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return r ? r : realloc(p, sz);
}
#endif

/* ── Tunables / bounds ─────────────────────────────────────────────── */
#define IMP_MAX_SIGS        140   /* STR has 134 (133 + Crc16) */
#define IMP_MAX_FILES       192   /* per day group */
#define IMP_MAX_SESSIONS    24    /* per day group */
#define IMP_MAX_STR_REC     370   /* STR covers ~a year of days */
#define IMP_MATCH_MS        60000 /* EVE↔wave cluster max TherapyStart→MaskOn gap */
#define IMP_CLUSTER_MS      2000  /* same-cluster timestamp tolerance */
#define IMP_SESS_DUP_MS     45000 /* existing-session duplicate window */
#define IMP_LINE_MAX        470   /* keep event JSON lines < consumer buffers */

/* SomnoBridge drift sidecar */
#define IMP_DRIFT_FILE      "_bridge_drift.json"

/* ── Small utilities ────────────────────────────────────────────────── */

/* CRC16-CCITT (0x1021, init 0xFFFF) — same polynomial the ResMed record
 * trailer and edf_header.c's edf_crc16_ccitt() use.  Kept local so the
 * importer stays host-testable without dragging in the export stack. */
static uint16_t imp_crc16(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

/* Howard Hinnant's civil↔days algorithm (proleptic Gregorian). */
static void imp_civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp + (mp < 10 ? 3 : -9));
    *y = (int)(yy + (*m <= 2));
}

/* Epoch ms for a device-civil datetime, interpreted in the ESP's local
 * timezone (see file-header comment).  Returns -1 on bad input. */
static int64_t imp_civil_to_dev_ms(int y, int mo, int d, int h, int mi, int s)
{
    struct tm tm = {
        .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d,
        .tm_hour = h, .tm_min = mi, .tm_sec = s, .tm_isdst = -1,
    };
    time_t t = mktime(&tm);
    return (t == (time_t)-1) ? -1 : (int64_t)t * 1000;
}

/* "YYYY-MM-DDTHH:MM:SS.mmmZ" (device domain ms, so formatted as UTC). */
static void imp_dev_to_iso(int64_t dev_ms, char *out, size_t out_len)
{
    time_t t = (time_t)(dev_ms / 1000);
    int ms = (int)(dev_ms % 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(out, out_len, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             (tm.tm_year + 1900) % 10000, (tm.tm_mon + 1) % 100,
             tm.tm_mday % 100, tm.tm_hour % 100, tm.tm_min % 100,
             tm.tm_sec % 100, ms % 1000);
}

/* "YYYY-MM-DDTHH:MM:SS" in ESP local time (matches session_writer.c's
 * start_iso / end_iso manifest fields). */
static void imp_epoch_to_local_iso(int64_t epoch_ms, char *out, size_t out_len)
{
    time_t t = (time_t)(epoch_ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, out_len, "%Y-%m-%dT%H:%M:%S", &tm);
}

/* FATFS f_rename returns FR_EXIST if dst exists — unlink dst first. */
static int imp_rename_replace(const char *src, const char *dst)
{
    unlink(dst);
    return rename(src, dst);
}

/* Parse a filename session prefix "YYYYMMDD_HHMMSS" at *p (needs the first
 * 15 chars).  Returns device-domain epoch ms or -1. */
static int64_t imp_parse_prefix_ms(const char *p)
{
    int y, mo, d, h, mi, s;
    if (sscanf(p, "%4d%2d%2d_%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) != 6)
        return -1;
    if (y < 2000 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
        h > 23 || mi > 59 || s > 60)
        return -1;
    return imp_civil_to_dev_ms(y, mo, d, h, mi, s);
}

/* fsync a file and close it.  Returns ESP_OK only when everything flushed. */
static esp_err_t imp_flush_close(FILE *f, const char *path)
{
    (void)path;   /* only used by ESP_LOGE — a no-op under the host shim */
    if (fflush(f) != 0) {
        ESP_LOGE(TAG, "fflush %s: %s", path, strerror(errno));
        fclose(f);
        return ESP_FAIL;
    }
    if (fsync(fileno(f)) != 0) {
        ESP_LOGE(TAG, "fsync %s: %s", path, strerror(errno));
        fclose(f);
        return ESP_FAIL;
    }
    fclose(f);
    return ESP_OK;
}

/* ── EDF file model ─────────────────────────────────────────────────── */

typedef struct {
    char   label[17];
    double pmin, pmax;
    int    dmin, dmax;
    int    spr;          /* samples per record */
    long   rec_off;      /* byte offset of this signal inside a record */
    bool   is_crc;       /* label == "Crc16" — the 2-byte record trailer */
    bool   is_annot;     /* label == "EDF Annotations" */
    char   unit[9];
} imp_sig_t;

typedef struct {
    char      path[300];
    int64_t   ts_dev_ms;      /* header startdate+starttime → device epoch ms */
    int64_t   n_records;      /* clamped to whole records actually on disk */
    int64_t   n_declared;     /* n_records as written in the header */
    double    rdur_s;
    int       n_sigs;
    int       crc_sig;        /* index of the "Crc16" signal, -1 when absent */
    imp_sig_t sigs[IMP_MAX_SIGS];
    long      data_off;       /* header_bytes */
    long      rec_stride;     /* bytes per data record incl. Crc16 */
    bool      is_ann;         /* "EDF+D"/"EDF+C" */
    char      rec_id[81];     /* recording_id (carries SRN=/MID=/VID=) */
} imp_edf_t;

static void imp_trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\0')) s[--n] = '\0';
    /* ResMed pads with spaces; also drop stray leading spaces. */
    if (s[0] == ' ') {
        size_t i = 0;
        while (s[i] == ' ') i++;
        memmove(s, s + i, strlen(s + i) + 1);
    }
}

static void imp_field(const uint8_t *raw, char *out, int out_len)
{
    memcpy(out, raw, out_len - 1);
    out[out_len - 1] = '\0';
    imp_trim(out);
}

/* Parse and validate an EDF fixed header + signal table.
 * CRC record trailers are part of the declared record size when the file
 * carries a "Crc16" signal, which every ResMed writer emits.
 * Returns ESP_OK and fills *e on success. */
static esp_err_t imp_edf_open(imp_edf_t *e, const char *path)
{
    memset(e, 0, sizeof(*e));
    e->crc_sig = -1;
    snprintf(e->path, sizeof(e->path), "%s", path);

    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    struct stat st;
    if (stat(path, &st) != 0) { fclose(f); return ESP_FAIL; }
    if (st.st_size < 256) { fclose(f); return ESP_ERR_INVALID_SIZE; }

    uint8_t fixed[256];
    if (fread(fixed, 1, 256, f) != 256) { fclose(f); return ESP_ERR_INVALID_SIZE; }

    char buf[96];
    /* version must be "0" */
    imp_field(fixed + 0, buf, 9);
    if (strcmp(buf, "0") != 0) { fclose(f); return ESP_ERR_INVALID_VERSION; }

    imp_field(fixed + 88, e->rec_id, sizeof(e->rec_id));
    char sdate[9], stime[9], rsvd[45];
    imp_field(fixed + 168, sdate, sizeof(sdate));
    imp_field(fixed + 176, stime, sizeof(stime));
    imp_field(fixed + 192, rsvd, sizeof(rsvd));
    e->is_ann = (strncmp(rsvd, "EDF+", 4) == 0 || strncmp(rsvd, "EDF+C", 5) == 0);

    char hb[9], nrec[9], rdur[9], nsig[5];
    imp_field(fixed + 184, hb, sizeof(hb));
    imp_field(fixed + 236, nrec, sizeof(nrec));
    imp_field(fixed + 244, rdur, sizeof(rdur));
    imp_field(fixed + 252, nsig, sizeof(nsig));
    e->n_declared = strtol(nrec, NULL, 10);
    e->rdur_s     = strtod(rdur, NULL);
    e->n_sigs     = (int)strtol(nsig, NULL, 10);
    long hdr_bytes = strtol(hb, NULL, 10);

    /* EDF+D annotation files (EVE/CSL) declare a data-record duration of 0
     * on real ResMed cards — every record carries its own time-keeping TAL.
     * Only waveform/summary files need a positive duration. */
    bool rdur_ok = e->rdur_s > 0 || (e->rdur_s == 0 && e->is_ann);
    if (e->n_sigs < 1 || e->n_sigs > IMP_MAX_SIGS ||
        hdr_bytes != 256L * (e->n_sigs + 1) || !rdur_ok) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    e->data_off = hdr_bytes;

    /* startdate "dd.mm.yy" + starttime "hh.mm.ss" → device epoch ms */
    {
        int dd = 0, mm = 0, yy = 0, hh = 0, mi = 0, ss = 0;
        if (sscanf(sdate, "%2d.%2d.%2d", &dd, &mm, &yy) == 3 &&
            sscanf(stime, "%2d.%2d.%2d", &hh, &mi, &ss) == 3) {
            int year = (yy >= 85) ? 1900 + yy : 2000 + yy;
            e->ts_dev_ms = imp_civil_to_dev_ms(year, mm, dd, hh, mi, ss);
        } else {
            e->ts_dev_ms = -1;
        }
    }

    /* Signal table — EDF stores the header field-major: every label first,
     * then every phys_min, etc. */
    uint8_t *blk = imp_malloc(256 * (size_t)e->n_sigs);
    if (!blk) { fclose(f); return ESP_ERR_NO_MEM; }
    if (fread(blk, 1, 256 * (size_t)e->n_sigs, f) != 256 * (size_t)e->n_sigs) {
        free(blk); fclose(f); return ESP_ERR_INVALID_SIZE;
    }
    const int S = e->n_sigs;
    e->rec_stride = 0;
    for (int i = 0; i < S; i++) {
        imp_sig_t *sg = &e->sigs[i];
        imp_field(blk + i * 16, sg->label, sizeof(sg->label));
        char pmn[9], pmx[9], dmn[9], dmx[9], spr[9];
        /* EDF signal table is field-major over all S signals:
         *  label 16B · transducer 80B · unit 8B · phys min/max 8B ·
         *  dig min/max 8B · prefilter 80B · samples/record 8B · rsvd 32B */
        imp_field(blk + 96 * S + i * 8, sg->unit, sizeof(sg->unit));
        imp_field(blk + 104 * S + i * 8, pmn, sizeof(pmn));
        imp_field(blk + 112 * S + i * 8, pmx, sizeof(pmx));
        imp_field(blk + 120 * S + i * 8, dmn, sizeof(dmn));
        imp_field(blk + 128 * S + i * 8, dmx, sizeof(dmx));
        imp_field(blk + 216 * S + i * 8, spr, sizeof(spr));
        sg->pmin = strtod(pmn, NULL);
        sg->pmax = strtod(pmx, NULL);
        sg->dmin = (int)strtol(dmn, NULL, 10);
        sg->dmax = (int)strtol(dmx, NULL, 10);
        sg->spr  = (int)strtol(spr, NULL, 10);
        sg->is_crc   = (strcmp(sg->label, "Crc16") == 0);
        if (sg->is_crc) e->crc_sig = i;
        sg->is_annot = (strcmp(sg->label, "EDF Annotations") == 0);
        sg->rec_off  = e->rec_stride;
        e->rec_stride += sg->spr * 2;
    }
    free(blk);
    fclose(f);

    if (e->rec_stride <= 0) return ESP_ERR_INVALID_SIZE;

    /* Clamp record count to whole records physically present (torn tail). */
    long avail = (long)st.st_size - e->data_off;
    int64_t have = avail / e->rec_stride;
    if (e->n_declared <= 0 || e->n_declared > have)
        e->n_records = have;
    else
        e->n_records = e->n_declared;
    if (e->n_records < 0) e->n_records = 0;
    return ESP_OK;
}

/* Read whole record `r` into buf (rec_stride bytes). */
static bool imp_edf_read_rec(const imp_edf_t *e, FILE *f, int64_t r,
                             uint8_t *buf)
{
    if (fseek(f, e->data_off + (long)r * e->rec_stride, SEEK_SET) != 0)
        return false;
    return fread(buf, 1, e->rec_stride, f) == (size_t)e->rec_stride;
}

/* Verify a record's CRC16-CCITT trailer (written by the ResMed writer as
 * the trailing "Crc16" signal, covering every byte before it).  Foreign
 * EDFs without the signal pass unconditionally. */
static bool imp_edf_rec_crc_ok(const imp_edf_t *e, const uint8_t *rec)
{
    if (e->crc_sig < 0) return true;
    const imp_sig_t *cs = &e->sigs[e->crc_sig];
    int16_t got;
    memcpy(&got, rec + cs->rec_off, 2);
    return got == (int16_t)imp_crc16(rec, (size_t)cs->rec_off);
}

/* Find the first signal whose label matches `prefix` (either exact or
 * followed by '.' rate suffix like "Flow.40ms"), in table order.
 * Skips Crc16/annotation signals. */
static int imp_sig_find(const imp_edf_t *e, const char *prefix)
{
    size_t n = strlen(prefix);
    bool prefix_has_dot = (memchr(prefix, '.', n) != NULL);
    for (int i = 0; i < e->n_sigs; i++) {
        if (e->sigs[i].is_crc || e->sigs[i].is_annot) continue;
        const char *lbl = e->sigs[i].label;
        if (strncasecmp(lbl, prefix, n) == 0 &&
            (lbl[n] == '\0' || (!prefix_has_dot && lbl[n] == '.')))
            return i;
    }
    return -1;
}

/* EDF digital → stored phys×100 int16 (the SNT v2 encoding).
 * Invalid/out-of-range digital values become SNT_MISSING when
 * mark_missing is set, or are scaled/clamped into range otherwise.
 * target_unit is the SNT channel's expected unit; a source declaring a
 * different unit is rescaled where we know how (L/min ↔ L/s). */
static int16_t imp_dig_to_snt(const imp_sig_t *sg, int16_t dig, bool mark_missing,
                              const char *target_unit)
{
    if (mark_missing && (dig < sg->dmin || dig > sg->dmax || dig == -1))
        return SNT_MISSING;
    double dspan = (double)sg->dmax - sg->dmin;
    if (dspan <= 0) return 0;
    double phys = sg->pmin + ((double)dig - sg->dmin) * (sg->pmax - sg->pmin) / dspan;
    if (strcmp(sg->unit, target_unit) != 0) {
        if (!strcmp(sg->unit, "L/min") && !strcmp(target_unit, "L/s"))
            phys /= 60.0;
        else if (!strcmp(sg->unit, "L/s") && !strcmp(target_unit, "L/min"))
            phys *= 60.0;
        /* other unit mismatches: store the physical value as-is */
    }
    long v = lrint(phys * 100.0);
    if (v > INT16_MAX) v = INT16_MAX;
    if (v <= INT16_MIN) v = INT16_MIN + 1;
    return (int16_t)v;
}

/* ── Source file model ──────────────────────────────────────────────── */

typedef enum {
    K_BRP = 0, K_PLD, K_SA2, K_EVE, K_CSL, K_OTHER
} imp_kind_t;

typedef struct {
    char        path[300];
    char        prefix[20];   /* "YYYYMMDD_HHMMSS" from the filename */
    int64_t     ts_dev_ms;    /* filename-derived device-domain start */
    imp_kind_t  kind;
} imp_file_t;

/* <prefix>_<KIND>.edf — case-insensitive suffix. */
static bool imp_classify(const char *dir_path, const char *name, imp_file_t *out)
{
    size_t nl = strlen(name);
    if (nl < 9) return false;
    const char *ext = name + nl - 4;
    if (strcasecmp(ext, ".edf") != 0) return false;
    if (nl < 20 || name[8] != '_') return false;

    char stem[16];
    memcpy(stem, name, 15);
    stem[15] = '\0';
    int64_t ts = imp_parse_prefix_ms(stem);
    if (ts < 0) return false;

    char kind[16];
    size_t kl = nl - 4 - 16;          /* between '<ts>_' and '.edf' */
    if (kl == 0 || kl >= sizeof(kind)) return false;
    memcpy(kind, name + 16, kl);
    kind[kl] = '\0';

    imp_kind_t k;
    if      (strcasecmp(kind, "BRP") == 0) k = K_BRP;
    else if (strcasecmp(kind, "PLD") == 0) k = K_PLD;
    else if (strcasecmp(kind, "SA2") == 0 || strcasecmp(kind, "SAD") == 0) k = K_SA2;
    else if (strcasecmp(kind, "EVE") == 0) k = K_EVE;
    else if (strcasecmp(kind, "CSL") == 0) k = K_CSL;
    else return false;

    memset(out, 0, sizeof(*out));
    snprintf(out->path, sizeof(out->path), "%s/%s", dir_path, name);
    memcpy(out->prefix, stem, 16);
    out->ts_dev_ms = ts;
    out->kind = k;
    return true;
}

/* ── SNT stream writers (staging .part → rename on commit) ──────────── */

typedef struct {
    FILE         *f;
    char          tmp_path[330];
    char          final_path[330];
    snt_header_t  hdr;
    uint32_t      written;
} snt_w_t;

static esp_err_t snt_w_open(snt_w_t *w, const char *final_path,
                            uint8_t n_channels, uint16_t hz_x10, uint8_t tier,
                            int64_t start_epoch_ms)
{
    memset(w, 0, sizeof(*w));
    snprintf(w->final_path, sizeof(w->final_path), "%s", final_path);
    snprintf(w->tmp_path, sizeof(w->tmp_path), "%s.part", final_path);
    w->f = fopen(w->tmp_path, "wb");
    if (!w->f) {
        ESP_LOGE(TAG, "import: cannot create %s: %s", w->tmp_path, strerror(errno));
        return ESP_FAIL;
    }
    w->hdr = (snt_header_t){
        .magic = SNT_MAGIC, .version = SNT_VERSION, .tier = tier,
        .n_channels = n_channels, .sample_bytes = 2,
        .sample_hz_x10 = hz_x10, .reserved = 0,
        .start_epoch_ms = start_epoch_ms,
        .sample_count = 0, .reserved2 = 0,
    };
    if (fwrite(&w->hdr, 1, sizeof(w->hdr), w->f) != sizeof(w->hdr)) {
        fclose(w->f);
        unlink(w->tmp_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Write n interleaved frames (n_channels each) and bump the count. */
static esp_err_t snt_w_frames(snt_w_t *w, const int16_t *frames, uint32_t n)
{
    size_t bytes = (size_t)n * w->hdr.n_channels * sizeof(int16_t);
    if (fwrite(frames, 1, bytes, w->f) != bytes) return ESP_FAIL;
    w->written += n;
    return ESP_OK;
}

/* Flush + fsync + back-patch the sample count; leaves the .part in place. */
static esp_err_t snt_w_flush(snt_w_t *w)
{
    if (!w->f) return ESP_OK;
    w->hdr.sample_count = w->written;
    if (fseek(w->f, 0, SEEK_SET) == 0)
        fwrite(&w->hdr, 1, sizeof(w->hdr), w->f);
    fseek(w->f, 0, SEEK_END);
    return imp_flush_close(w->f, w->tmp_path) == ESP_OK ? (w->f = NULL, ESP_OK) : ESP_FAIL;
}

static void snt_w_abort(snt_w_t *w)
{
    if (w->f) fclose(w->f), w->f = NULL;
    unlink(w->tmp_path);
}

/* ── Waveform converters ────────────────────────────────────────────── */

/* BRP.edf → _flow.snt (1ch 25Hz) + _press.snt (1ch 25Hz) + _flow_mm.snt
 * (2ch 1Hz min/max envelope, matching the live L1 fast path).
 * out_n/out_n_press/out_n_mm receive frame counts (press may be absent). */
static esp_err_t conv_brp(imp_edf_t *e, const char *dir, const char *id,
                          int64_t wav_ntp_ms, uint32_t *out_n,
                          uint32_t *out_n_press, uint32_t *out_n_mm)
{
    int fi = imp_sig_find(e, "Flow");
    if (fi < 0) fi = imp_sig_find(e, "BlowFlow");
    int pi = imp_sig_find(e, "Press");
    if (pi < 0) pi = imp_sig_find(e, "MaskPress");
    if (fi < 0) {
        ESP_LOGW(TAG, "import: %s has no Flow signal — skipped", e->path);
        return ESP_ERR_NOT_FOUND;
    }
    const imp_sig_t *fs = &e->sigs[fi];

    char p[400];
    snt_w_t wf, wp, wm;
    snprintf(p, sizeof(p), "%s/%s_flow.snt", dir, id);
    if (snt_w_open(&wf, p, 1, 250, SNT_TIER_RAW, wav_ntp_ms) != ESP_OK)
        return ESP_FAIL;
    snprintf(p, sizeof(p), "%s/%s_press.snt", dir, id);
    if (pi >= 0 && snt_w_open(&wp, p, 1, 250, SNT_TIER_RAW, wav_ntp_ms) != ESP_OK) {
        snt_w_abort(&wf); return ESP_FAIL;
    }
    snprintf(p, sizeof(p), "%s/%s_flow_mm.snt", dir, id);
    if (snt_w_open(&wm, p, 2, 10, SNT_TIER_MINMAX, wav_ntp_ms) != ESP_OK) {
        snt_w_abort(&wf); if (pi >= 0) snt_w_abort(&wp); return ESP_FAIL;
    }

    FILE *f = fopen(e->path, "rb");
    if (!f) { snt_w_abort(&wf); if (pi >= 0) snt_w_abort(&wp); snt_w_abort(&wm); return ESP_FAIL; }

    esp_err_t ret = ESP_FAIL;
    uint8_t *rec = imp_malloc((size_t)e->rec_stride);
    if (!rec) { fclose(f); goto out_abort; }

    {
        int16_t mm_min = INT16_MAX, mm_max = INT16_MIN + 1;
        int mm_fill = 0;
        for (int64_t r = 0; r < e->n_records; r++) {
            if (!imp_edf_read_rec(e, f, r, rec)) {
                ESP_LOGW(TAG, "import: %s torn at record %lld", e->path, (long long)r);
                break;
            }
            /* A record that fails its CRC trailer is positional noise
             * (torn tail or media rot) — keep its slot as missing. */
            bool ok = imp_edf_rec_crc_ok(e, rec);
            const uint8_t *fblk = rec + fs->rec_off;
            const uint8_t *pblk = (pi >= 0) ? rec + e->sigs[pi].rec_off : NULL;
            for (int s = 0; s < fs->spr; s++) {
                int16_t sv = SNT_MISSING;
                if (ok) {
                    int16_t fv;
                    memcpy(&fv, fblk + s * 2, 2);
                    sv = imp_dig_to_snt(fs, fv, false, "L/s");
                }
                if (snt_w_frames(&wf, &sv, 1) != ESP_OK) goto out_free;
                if (pblk) {
                    int16_t sp = SNT_MISSING;
                    if (ok) {
                        int16_t pv;
                        memcpy(&pv, pblk + s * 2, 2);
                        sp = imp_dig_to_snt(&e->sigs[pi], pv, false, "cmH2O");
                    }
                    if (snt_w_frames(&wp, &sp, 1) != ESP_OK) goto out_free;
                }
                if (sv == SNT_MISSING) { /* keep envelope clean */ }
                else {
                    if (sv < mm_min) mm_min = sv;
                    if (sv > mm_max) mm_max = sv;
                }
                if (++mm_fill == 25) {
                    int16_t mm[2] = {
                        (mm_min == INT16_MAX) ? SNT_MISSING : mm_min,
                        (mm_max == (int16_t)(INT16_MIN + 1)) ? SNT_MISSING : mm_max,
                    };
                    if (snt_w_frames(&wm, mm, 1) != ESP_OK) goto out_free;
                    mm_min = INT16_MAX; mm_max = INT16_MIN + 1; mm_fill = 0;
                }
            }
        }
    }
    if (snt_w_flush(&wf) != ESP_OK) goto out_free;
    if (pi >= 0 && snt_w_flush(&wp) != ESP_OK) goto out_free;
    if (snt_w_flush(&wm) != ESP_OK) goto out_free;
    *out_n = wf.written;
    *out_n_press = (pi >= 0) ? wp.written : 0;
    *out_n_mm = wm.written;
    ret = ESP_OK;

out_free:
    free(rec);
    fclose(f);
out_abort:
    if (ret != ESP_OK) {
        snt_w_abort(&wf); if (pi >= 0) snt_w_abort(&wp); snt_w_abort(&wm);
    }
    /* Success: the .part files are flushed+fsynced; publish_parts() renames
     * them together with the rest of the session's staged output. */
    return ret;
}

/* SA2.edf/SAD.edf → _sa2.snt (2ch 1Hz: Pulse, SpO2).  dig -1 and
 * out-of-range values become SNT_MISSING (SomnoStage treats them as
 * absent oximetry). */
static esp_err_t conv_sa2(imp_edf_t *e, const char *dir, const char *id,
                          int64_t wav_ntp_ms, uint32_t *out_n)
{
    int pi = imp_sig_find(e, "Pulse");
    int si = imp_sig_find(e, "SpO2");
    if (pi < 0 && si < 0) return ESP_ERR_NOT_FOUND;

    char p[400];
    snprintf(p, sizeof(p), "%s/%s_sa2.snt", dir, id);
    snt_w_t w;
    if (snt_w_open(&w, p, 2, 10, SNT_TIER_RAW, wav_ntp_ms) != ESP_OK)
        return ESP_FAIL;

    FILE *f = fopen(e->path, "rb");
    if (!f) { snt_w_abort(&w); return ESP_FAIL; }
    esp_err_t ret = ESP_FAIL;
    uint8_t *rec = imp_malloc((size_t)e->rec_stride);
    if (!rec) { fclose(f); snt_w_abort(&w); return ESP_FAIL; }

    for (int64_t r = 0; r < e->n_records; r++) {
        if (!imp_edf_read_rec(e, f, r, rec)) {
            ESP_LOGW(TAG, "import: %s torn at record %lld", e->path, (long long)r);
            break;
        }
        bool ok = imp_edf_rec_crc_ok(e, rec);
        int n = e->sigs[pi >= 0 ? pi : si].spr;
        for (int s = 0; s < n; s++) {
            int16_t fr[2] = { SNT_MISSING, SNT_MISSING };
            if (ok && pi >= 0) {
                int16_t d; memcpy(&d, rec + e->sigs[pi].rec_off + s * 2, 2);
                fr[0] = imp_dig_to_snt(&e->sigs[pi], d, true, "bpm");
            }
            if (ok && si >= 0) {
                int16_t d; memcpy(&d, rec + e->sigs[si].rec_off + s * 2, 2);
                fr[1] = imp_dig_to_snt(&e->sigs[si], d, true, "%");
            }
            if (snt_w_frames(&w, fr, 1) != ESP_OK) goto out;
        }
    }
    ret = snt_w_flush(&w);
out:
    free(rec);
    fclose(f);
    if (ret != ESP_OK) { snt_w_abort(&w); return ret; }
    *out_n = w.written;
    return ESP_OK;   /* staged .part — publish_parts() renames it */
}

/* Write an all-SNT_MISSING sa2.snt covering `secs` seconds — used when the
 * source had no SA2/SAD.edf (AS10 without oximeter module) so downstream
 * consumers see explicit absent data instead of a missing file. */
static esp_err_t write_missing_sa2(const char *dir, const char *id,
                                   int64_t start_ntp_ms, uint32_t secs,
                                   uint32_t *out_n)
{
    char p[400];
    snprintf(p, sizeof(p), "%s/%s_sa2.snt", dir, id);
    snt_w_t w;
    if (snt_w_open(&w, p, 2, 10, SNT_TIER_RAW, start_ntp_ms) != ESP_OK)
        return ESP_FAIL;
    int16_t fr[2] = { SNT_MISSING, SNT_MISSING };
    esp_err_t ret = ESP_OK;
    for (uint32_t i = 0; i < secs && ret == ESP_OK; i++)
        ret = snt_w_frames(&w, fr, 1);
    if (ret == ESP_OK) ret = snt_w_flush(&w);
    if (ret != ESP_OK) { snt_w_abort(&w); return ret; }
    *out_n = w.written;
    return ESP_OK;   /* staged .part */
}

/* PLD.edf → _pld.snt (12ch 0.5Hz).  Label→channel map mirrors the live
 * KEY_MASK_PRESSURE_2S ordering; channels absent from the source EDF are
 * written as 0 (same as the live writer emits for unpopulated channels).
 * Supports both AS11 labels and AS10 short aliases (RR, Vt, MV, TgMV, I:E, FL). */
static const char *const PLD_LABELS[12][2] = {
    { "MaskPress", NULL }, { "Press", NULL },     { "EprPress", NULL },
    { "Leak", NULL },      { "RespRate", "RR" },  { "TidVol", "Vt" },
    { "MinVent", "MV" },   { "TgtVent", "TgMV" }, { "IERatio", "I:E" },
    { "Snore", NULL },     { "FlowLim", "FL" },   { "Ti", NULL },
};
static const char *const PLD_UNITS[12] = {
    "cmH2O", "cmH2O", "cmH2O", "L/s", "bpm", "L",
    "L/min", "L/min", "%", "", "", "s",
};

static esp_err_t conv_pld(imp_edf_t *e, const char *dir, const char *id,
                          int64_t wav_ntp_ms, uint32_t *out_n)
{
    /* Resolve label→source-signal map (prefix match up to '\0' or '.',
     * in canonical order).  "Press" cannot match "MaskPress" or "TidVol"
     * match "Ti". */
    int map[12];
    bool taken[IMP_MAX_SIGS] = {0};
    for (int ch = 0; ch < 12; ch++) {
        map[ch] = -1;
        for (int a = 0; a < 2 && PLD_LABELS[ch][a] && map[ch] < 0; a++) {
            size_t n = strlen(PLD_LABELS[ch][a]);
            for (int i = 0; i < e->n_sigs; i++) {
                if (taken[i] || e->sigs[i].is_crc || e->sigs[i].is_annot) continue;
                const char *lbl = e->sigs[i].label;
                if (strncasecmp(lbl, PLD_LABELS[ch][a], n) == 0 &&
                    (lbl[n] == '\0' || lbl[n] == '.')) {
                    map[ch] = i;
                    taken[i] = true;
                    break;
                }
            }
        }
    }
    if (map[0] < 0 && map[1] < 0) return ESP_ERR_NOT_FOUND;  /* not a PLD */

    char p[400];
    snprintf(p, sizeof(p), "%s/%s_pld.snt", dir, id);
    snt_w_t w;
    if (snt_w_open(&w, p, 12, 5, SNT_TIER_RAW, wav_ntp_ms) != ESP_OK)
        return ESP_FAIL;

    FILE *f = fopen(e->path, "rb");
    if (!f) { snt_w_abort(&w); return ESP_FAIL; }
    esp_err_t ret = ESP_FAIL;
    uint8_t *rec = imp_malloc((size_t)e->rec_stride);
    if (!rec) { fclose(f); snt_w_abort(&w); return ESP_FAIL; }

    for (int64_t r = 0; r < e->n_records; r++) {
        if (!imp_edf_read_rec(e, f, r, rec)) {
            ESP_LOGW(TAG, "import: %s torn at record %lld", e->path, (long long)r);
            break;
        }
        bool ok = imp_edf_rec_crc_ok(e, rec);
        /* Sample count = min spr over matched signals (they should agree). */
        int n = INT16_MAX;
        for (int ch = 0; ch < 12; ch++)
            if (map[ch] >= 0 && e->sigs[map[ch]].spr < n) n = e->sigs[map[ch]].spr;
        if (n <= 0 || n > 600) break;
        for (int s = 0; s < n; s++) {
            int16_t fr[12] = {0};
            if (!ok) {
                for (int ch = 0; ch < 12; ch++)
                    if (map[ch] >= 0) fr[ch] = SNT_MISSING;
            } else {
                for (int ch = 0; ch < 12; ch++) {
                    if (map[ch] < 0) continue;   /* unpopulated channel → 0 */
                    int16_t d;
                    memcpy(&d, rec + e->sigs[map[ch]].rec_off + s * 2, 2);
                    fr[ch] = imp_dig_to_snt(&e->sigs[map[ch]], d, ch == 10,
                                            PLD_UNITS[ch]);
                }
            }
            if (snt_w_frames(&w, fr, 1) != ESP_OK) goto out;
        }
    }
    ret = snt_w_flush(&w);
out:
    free(rec);
    fclose(f);
    if (ret != ESP_OK) { snt_w_abort(&w); return ret; }
    *out_n = w.written;
    return ESP_OK;   /* staged .part */
}

/* ── Annotation (TAL) → _events.snt ─────────────────────────────────── */

typedef struct {
    int64_t onset_ms;       /* relative to the EDF file's own start */
    int64_t dur_ms;
    char    label[64];
} imp_ev_t;

typedef struct {
    imp_ev_t *ev;
    int      n, cap;
} imp_ev_list_t;

/* Parse one TAL: "+<onset>[\x15<dur>]\x14<label>\x14[<label>\x14]...\x00".
 * Onsets are relative to the owning record's start. */
static int tal_events(const uint8_t *p, const uint8_t *end, double rec_onset_s,
                      imp_ev_list_t *L, bool csl)
{
    int added = 0;
    /* TALs are individually NUL-terminated; the channel is NUL-padded, so
     * a '\0' means "next TAL", not end-of-record. */
    while (p < end) {
        if (*p != '+' && *p != '-') { p++; continue; }
        char *q;
        double onset = strtod((const char *)p, &q);
        if (q == (char *)p) { p++; continue; }
        p = (const uint8_t *)q;
        double dur = 0;
        if (p < end && *p == 0x15) {
            dur = strtod((const char *)p + 1, &q);
            if (q == (char *)p + 1) dur = 0;
            p = (const uint8_t *)q;
        }
        if (p >= end || *p != 0x14) continue;
        p++;
        /* annotation strings until NUL or non-printable */
        while (p < end && *p != '\0' && *p != 0x14) {
            const uint8_t *s = p;
            while (p < end && *p && *p != 0x14 && *p != 0x15 && *p >= 0x20) p++;
            size_t ln = (size_t)(p - s);
            if (ln > 0 && ln < sizeof(((imp_ev_t *)0)->label)) {
                if (L->n >= L->cap) {
                    int ncap = L->cap ? L->cap * 2 : 32;
                    imp_ev_t *t = imp_realloc(L->ev, (size_t)ncap * sizeof(imp_ev_t));
                    if (!t) return added;
                    L->ev = t; L->cap = ncap;
                }
                imp_ev_t *e = &L->ev[L->n];
                e->onset_ms = (int64_t)llround((rec_onset_s + onset) * 1000.0);
                e->dur_ms   = csl ? 0 : (int64_t)llround(dur * 1000.0);
                memcpy(e->label, s, ln);
                e->label[ln] = '\0';
                L->n++; added++;
            }
            if (p < end && *p == 0x14) p++;
        }
        while (p < end && *p == '\0') p++;
    }
    return added;
}

/* EDF+D annotation label → canonical AS11 JSON event name. */
static const char *imp_event_name(const char *label, bool csl)
{
    if (csl) {
        if (!strcmp(label, "CSR Start")) return "CsrStart";
        if (!strcmp(label, "CSR End"))   return "CsrEnd";
        return NULL;
    }
    if (!strcmp(label, "Obstructive Apnea"))   return "ObstructiveApneaEnd";
    if (!strcmp(label, "Central Apnea"))       return "CentralApneaEnd";
    if (!strcmp(label, "Hypopnea"))            return "HypopneaEnd";
    if (!strcmp(label, "Arousal"))             return "ReraEnd";
    if (!strcmp(label, "RERA"))                return "ReraEnd";
    if (!strcmp(label, "Apnea"))               return "ApneaEnd";
    if (!strcmp(label, "Unclassified Apnea"))  return "ApneaEnd";
    return NULL;   /* "Recording starts" and anything unknown */
}

static int imp_event_cmp(const void *a, const void *b)
{
    const imp_ev_t *x = a, *y = b;
    if (x->onset_ms != y->onset_ms) return x->onset_ms < y->onset_ms ? -1 : 1;
    return strcmp(x->label, y->label);
}

/* Collect all annotation events from an EDF+D file (EVE or CSL). */
static esp_err_t collect_tal(const char *path, imp_ev_list_t *L, bool csl)
{
    imp_edf_t *e = imp_malloc(sizeof(imp_edf_t));
    if (!e) return ESP_ERR_NO_MEM;
    if (imp_edf_open(e, path) != ESP_OK) { free(e); return ESP_FAIL; }

    FILE *f = fopen(path, "rb");
    if (!f) { free(e); return ESP_FAIL; }
    esp_err_t ret = ESP_OK;
    uint8_t *rec = imp_malloc((size_t)e->rec_stride);
    if (!rec) { fclose(f); free(e); return ESP_ERR_NO_MEM; }

    /* Locate the annotation channel */
    int ai = -1;
    for (int i = 0; i < e->n_sigs; i++)
        if (e->sigs[i].is_annot || e->sigs[i].spr > 1) { ai = i; break; }
    if (ai < 0) { ret = ESP_ERR_NOT_FOUND; goto out; }

    for (int64_t r = 0; r < e->n_records; r++) {
        if (!imp_edf_read_rec(e, f, r, rec)) {
            ESP_LOGW(TAG, "import: %s torn at record %lld", path, (long long)r);
            break;
        }
        if (!imp_edf_rec_crc_ok(e, rec)) {
            ESP_LOGW(TAG, "import: %s record %lld CRC fail — annotations skipped",
                     path, (long long)r);
            continue;   /* never parse garbage into phantom events */
        }
        const uint8_t *blk = rec + e->sigs[ai].rec_off;
        tal_events(blk, blk + e->sigs[ai].spr * 2,
                   (double)r * e->rdur_s, L, csl);
    }
out:
    free(rec);
    fclose(f);
    free(e);
    return ret;
}

/* End of the last annotation (onset + duration) relative to the file's own
 * start, in ms; 0 when the file has none or can't be read. */
static int64_t imp_ann_span_ms(const char *path, bool csl)
{
    imp_ev_list_t L = {0};
    int64_t end = 0;
    if (collect_tal(path, &L, csl) == ESP_OK)
        for (int i = 0; i < L.n; i++) {
            int64_t t = L.ev[i].onset_ms + L.ev[i].dur_ms;
            if (t > end) end = t;
        }
    free(L.ev);
    return end;
}

/* Emit one EventNotification JSON line per event (each well under the
 * 512/640-byte consumer line buffers).  fmt_event_line is the single
 * source for the line format so in-memory renders stay byte-identical
 * to file output. */
static int fmt_event_line(char *line, size_t cap, const char *data_id,
                          const char *event, int64_t dev_ms, int dur_s,
                          int backdate_s)
{
    char iso[32];
    imp_dev_to_iso(dev_ms, iso, sizeof(iso));
    int n;
    if (backdate_s > 0)
        n = snprintf(line, cap,
            "{\"method\":\"EventNotification\",\"params\":{\"dataId\":\"%s\","
            "\"events\":[{\"event\":\"%s\",\"reportTime\":\"%s\","
            "\"backdateSeconds\":%d}]}}\n",
            data_id, event, iso, backdate_s);
    else if (dur_s > 0)
        n = snprintf(line, cap,
            "{\"method\":\"EventNotification\",\"params\":{\"dataId\":\"%s\","
            "\"events\":[{\"event\":\"%s\",\"reportTime\":\"%s\","
            "\"durationSeconds\":%d}]}}\n",
            data_id, event, iso, dur_s);
    else
        n = snprintf(line, cap,
            "{\"method\":\"EventNotification\",\"params\":{\"dataId\":\"%s\","
            "\"events\":[{\"event\":\"%s\",\"reportTime\":\"%s\"}]}}\n",
            data_id, event, iso);
    return (n > 0 && n < (int)cap) ? n : -1;
}

static int emit_event_line(FILE *f, const char *data_id, const char *event,
                           int64_t dev_ms, int dur_s, int backdate_s)
{
    char line[512];
    int n = fmt_event_line(line, sizeof(line), data_id, event, dev_ms,
                           dur_s, backdate_s);
    if (n <= 0) return -1;
    if ((int)fwrite(line, 1, (size_t)n, f) != n) return -1;
    return n;
}

/* Emit sink: the events render below produces identical output whether it
 * lands in the staged file (commit path) or an in-memory line set (the
 * legacy-manifest coverage check in augment_session). */
typedef int (*imp_emit_fn)(void *ctx, const char *data_id,
                           const char *event, int64_t dev_ms, int dur_s,
                           int backdate_s);

static int emit_to_file(void *ctx, const char *data_id, const char *event,
                        int64_t dev_ms, int dur_s, int backdate_s)
{
    return emit_event_line((FILE *)ctx, data_id, event, dev_ms, dur_s,
                           backdate_s);
}

typedef struct {
    char **v;      /* each entry is a malloc'd NUL-free line incl '\n' */
    int     n, cap;
} imp_line_set_t;

static int emit_to_set(void *ctx, const char *data_id, const char *event,
                       int64_t dev_ms, int dur_s, int backdate_s)
{
    imp_line_set_t *S = (imp_line_set_t *)ctx;
    char line[512];
    int n = fmt_event_line(line, sizeof(line), data_id, event, dev_ms,
                           dur_s, backdate_s);
    if (n <= 0) return -1;
    if (S->n >= S->cap) {
        int ncap = S->cap ? S->cap * 2 : 64;
        char **t = imp_realloc(S->v, (size_t)ncap * sizeof(char *));
        if (!t) return -1;
        S->v = t; S->cap = ncap;
    }
    char *dup = imp_malloc((size_t)n + 1);
    if (!dup) return -1;
    memcpy(dup, line, (size_t)n + 1);
    S->v[S->n++] = dup;
    return n;
}

static void line_set_free(imp_line_set_t *S)
{
    for (int i = 0; i < S->n; i++) free(S->v[i]);
    free(S->v);
    S->v = NULL; S->n = S->cap = 0;
}

/* The full events.snt render: synthetic Therapy/Mask window (device
 * domain) then respiratory (EVE) + CSR (CSL) annotations, byte-identical
 * via emit.  t_eve_dev = annotation-cluster start (TherapyStart), -1
 * when absent; t_wav_dev = waveform-cluster start (MaskOn/_ZLE), -1 when
 * absent; session start = t_eve when present else t_wav.
 * reportTime is written in the device clock domain so edf_annotations'
 * (as11 + drift - session) inversion lands exactly on the source onset. */
static esp_err_t events_render(const imp_file_t *eve_f,
                               const imp_file_t *csl_f,
                               int64_t t_eve_dev, int64_t t_wav_dev,
                               int64_t end_dev_ms,
                               imp_emit_fn emit, void *ctx,
                               uint32_t *out_events)
{
    esp_err_t ret = ESP_OK;
    uint32_t n_ev = 0;

    int64_t sess_start = (t_eve_dev >= 0) ? t_eve_dev : t_wav_dev;

    /* UsageEvents: synthetic Therapy/Mask window in the device domain.
     * collect_session_mask_pairs() prefers MaskOn/MaskOff; TherapyStart/
     * TherapyStop are the fallback and mirror what the AS11 emits. */
    if (sess_start >= 0 &&
        emit(ctx, "UsageEvents-TherapyStatusEvents", "TherapyStart",
             sess_start, 0, 0) < 0) ret = ESP_FAIL;
    if (ret == ESP_OK && t_wav_dev >= 0 &&
        emit(ctx, "UsageEvents-TherapyStatusEvents", "MaskOn",
             t_wav_dev, 0, 0) < 0) ret = ESP_FAIL;
    if (ret == ESP_OK && t_wav_dev >= 0 && end_dev_ms > t_wav_dev &&
        emit(ctx, "UsageEvents-TherapyStatusEvents", "MaskOff",
             end_dev_ms, 0, 0) < 0) ret = ESP_FAIL;
    if (ret == ESP_OK && end_dev_ms > sess_start &&
        emit(ctx, "UsageEvents-TherapyStatusEvents", "TherapyStop",
             end_dev_ms, 0, 0) < 0) ret = ESP_FAIL;

    for (int pass = 0; pass < 2 && ret == ESP_OK; pass++) {
        const imp_file_t *src = pass ? csl_f : eve_f;
        if (!src) continue;
        bool csl = (pass == 1);
        imp_ev_list_t L = {0};
        esp_err_t cr = collect_tal(src->path, &L, csl);
        if (cr != ESP_OK)
            ESP_LOGW(TAG, "import: %s annotations unreadable (%s) — events skipped",
                     src->path, esp_err_to_name(cr));
        if (cr == ESP_OK && L.n > 0) {
            qsort(L.ev, L.n, sizeof(imp_ev_t), imp_event_cmp);
            for (int i = 0; i < L.n && ret == ESP_OK; i++) {
                const char *name = imp_event_name(L.ev[i].label, csl);
                if (!name) continue;
                if (emit(ctx, "TherapyEvents-RespiratoryEvents",
                         name, src->ts_dev_ms + L.ev[i].onset_ms,
                         (int)(L.ev[i].dur_ms / 1000), 0) < 0)
                    ret = ESP_FAIL;
                else
                    n_ev++;
            }
        }
        free(L.ev);
    }
    if (ret == ESP_OK) *out_events = n_ev;
    return ret;
}

/* Build <id>_events.snt for a session. */
static esp_err_t write_events_snt(const char *dir, const char *id,
                                  const imp_file_t *eve_f, const imp_file_t *csl_f,
                                  int64_t t_eve_dev, int64_t t_wav_dev,
                                  int64_t end_dev_ms, uint32_t *out_events)
{
    char path[400], tmp[408];
    snprintf(path, sizeof(path), "%s/%s_events.snt", dir, id);
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return ESP_FAIL;
    esp_err_t ret = events_render(eve_f, csl_f, t_eve_dev, t_wav_dev,
                                  end_dev_ms, emit_to_file, f, out_events);
    if (ret == ESP_OK) ret = imp_flush_close(f, tmp);
    else fclose(f);
    if (ret != ESP_OK) { unlink(tmp); return ret; }
    /* Leave the .part staged — publish_parts() renames it with the rest of
     * the session output. */
    return ESP_OK;
}

/* Collect the TherapyEvents lines the offered annotation files would
 * emit — the part of events.snt that EVE/CSL actually own.  The
 * synthetic UsageEvents block is session-anchored, not source-anchored,
 * so it is intentionally excluded here: an annotation-only re-offer
 * cannot reproduce wave-derived MaskOn/MaskOff times and must not be
 * judged on them. */
static esp_err_t collect_ann_lines(const imp_file_t *eve_f,
                                   const imp_file_t *csl_f,
                                   imp_line_set_t *S, uint32_t *out_events)
{
    esp_err_t ret = ESP_OK;
    uint32_t n_ev = 0;
    for (int pass = 0; pass < 2 && ret == ESP_OK; pass++) {
        const imp_file_t *src = pass ? csl_f : eve_f;
        if (!src) continue;
        bool csl = (pass == 1);
        imp_ev_list_t L = {0};
        esp_err_t cr = collect_tal(src->path, &L, csl);
        if (cr != ESP_OK) {
            ESP_LOGW(TAG, "import: %s annotations unreadable (%s)",
                     src->path, esp_err_to_name(cr));
            ret = ESP_FAIL;
        }
        if (ret == ESP_OK && L.n > 0) {
            qsort(L.ev, L.n, sizeof(imp_ev_t), imp_event_cmp);
            for (int i = 0; i < L.n && ret == ESP_OK; i++) {
                const char *name = imp_event_name(L.ev[i].label, csl);
                if (!name) continue;
                if (emit_to_set(S, "TherapyEvents-RespiratoryEvents",
                                name, src->ts_dev_ms + L.ev[i].onset_ms,
                                (int)(L.ev[i].dur_ms / 1000), 0) < 0)
                    ret = ESP_FAIL;
                else
                    n_ev++;
            }
        }
        free(L.ev);
    }
    if (ret == ESP_OK) *out_events = n_ev;
    return ret;
}

/* Legacy manifests carry no src_kinds, so recorded annotation coverage
 * cannot be read back from metadata.  Instead re-render the annotation
 * events the offered sources would produce and require every recorded
 * TherapyEvents line to be present in the render: the merge then can
 * only grow the events file, never shrink it — a true duplicate
 * verifies line-for-line while a genuinely late file still merges its
 * new events.  UsageEvents lines (session-anchored) are kept verbatim;
 * a missing/empty events file covers trivially. */
#define ANN_DATA_ID "\"dataId\":\"TherapyEvents-RespiratoryEvents\""

static bool events_file_covered(const char *dir, const char *id,
                                imp_line_set_t *cand, imp_line_set_t *kept)
{
    char ep[440];
    snprintf(ep, sizeof(ep), "%s/%s_events.snt", dir, id);
    FILE *f = fopen(ep, "r");
    if (!f) return errno == ENOENT;

    /* Linear membership scan: cand stays in canonical emit order
     * (EVE block then CSL block) for the merged write.  Sets are
     * small — a session has at most a few hundred recorded events. */
    bool covered = true;
    char buf[640];
    while (covered && fgets(buf, sizeof(buf), f)) {
        if (!strstr(buf, ANN_DATA_ID)) {
            /* Session-anchored line — carried into the merged file. */
            int n = (int)strlen(buf);
            if (kept->n >= kept->cap) {
                int ncap = kept->cap ? kept->cap * 2 : 16;
                char **t = imp_realloc(kept->v, (size_t)ncap * sizeof(char *));
                if (!t) { covered = false; break; }
                kept->v = t; kept->cap = ncap;
            }
            char *dup = imp_malloc((size_t)n + 1);
            if (!dup) { covered = false; break; }
            memcpy(dup, buf, (size_t)n + 1);
            kept->v[kept->n++] = dup;
            continue;
        }
        bool found = false;
        for (int i = 0; i < cand->n; i++)
            if (!strcmp(buf, cand->v[i])) { found = true; break; }
        if (!found) {
            covered = false;
            ESP_LOGI(TAG, "import: %s uncovered event line: %.120s", id, buf);
        }
    }
    covered = covered && !ferror(f);
    fclose(f);
    return covered;
}

/* ── Identification / settings resolution ───────────────────────────── */

/* Parse SRN=/MID=/VID= out of an EDF recording_id into a flat ident JSON. */
static cJSON *ident_from_rec_id(const char *rec_id)
{
    char srn[32] = "", mid[8] = "", vid[8] = "";
    const char *p;
    if ((p = strstr(rec_id, "SRN="))) sscanf(p + 4, "%31s", srn);
    if ((p = strstr(rec_id, "MID="))) sscanf(p + 4, "%7s", mid);
    if ((p = strstr(rec_id, "VID="))) sscanf(p + 4, "%7s", vid);
    /* Strip any trailing spaces the %s scan picked up */
    imp_trim(srn); imp_trim(mid); imp_trim(vid);

    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    if (srn[0]) cJSON_AddStringToObject(j, "SerialNumber", srn);
    if (mid[0]) {
        cJSON_AddNumberToObject(j, "PlatformIdentifier", atoi(mid));
        /* MID identifies the platform family: < 40 → AirSense 10 */
        cJSON_AddStringToObject(j, "ProductName",
                                atoi(mid) >= 40 ? "AirSense 11" : "AirSense 10");
    }
    if (vid[0]) cJSON_AddNumberToObject(j, "VariantIdentifier", atoi(vid));
    if (!srn[0] && !mid[0]) { cJSON_Delete(j); return NULL; }
    return j;
}

/* AS11 Identification.json: {"FlowGenerator":{"IdentificationProfiles":{
 * "Product":{...},"Hardware":{...},"Software":{...}}}} → flat ident. */
static cJSON *ident_from_ident_json(const cJSON *root)
{
    const cJSON *fg = cJSON_GetObjectItem(root, "FlowGenerator");
    const cJSON *ip = cJSON_GetObjectItem(fg ? fg : root, "IdentificationProfiles");
    if (!ip) return NULL;
    cJSON *out = cJSON_CreateObject();
    static const char *const secs[] = { "Product", "Hardware", "Software" };
    for (int s = 0; s < 3; s++) {
        const cJSON *sec = cJSON_GetObjectItem(ip, secs[s]);
        if (!sec) continue;
        cJSON *v = sec->child;
        while (v) {
            if (cJSON_IsString(v) || cJSON_IsNumber(v)) {
                cJSON *cp = cJSON_CreateString(v->valuestring ? v->valuestring : "");
                if (cJSON_IsNumber(v)) { cJSON_Delete(cp); cp = cJSON_CreateNumber(v->valuedouble); }
                cJSON_AddItemToObject(out, v->string, cp);
            }
            v = v->next;
        }
    }
    return out;
}

/* AS10 Identification.tgt: "#KEY VALUE" lines → flat ident.  Values are
 * written as hex by ResMed firmware (#MID 0024 → 0x24). */
static cJSON *ident_from_tgt(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    cJSON *j = cJSON_CreateObject();
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != '#') continue;
        char key[16], val[200];
        if (sscanf(line + 1, "%15s %199[^\n]", key, val) != 2) continue;
        imp_trim(val);
        if (!strcmp(key, "SRN"))      cJSON_AddStringToObject(j, "SerialNumber", val);
        else if (!strcmp(key, "PCD")) cJSON_AddStringToObject(j, "ProductCode", val);
        else if (!strcmp(key, "PNA")) {
            for (char *c = val; *c; c++) if (*c == '_') *c = ' ';
            cJSON_AddStringToObject(j, "ProductName", val);
        }
        else if (!strcmp(key, "MID")) cJSON_AddNumberToObject(j, "PlatformIdentifier", (int)strtol(val, NULL, 16));
        else if (!strcmp(key, "VID")) cJSON_AddNumberToObject(j, "VariantIdentifier", (int)strtol(val, NULL, 16));
        else if (!strcmp(key, "CID")) cJSON_AddStringToObject(j, "ConfigurationIdentifier", val);
        else if (!strcmp(key, "SID")) cJSON_AddStringToObject(j, "SoftwareIdentifier", val);
        else if (!strcmp(key, "RGN")) cJSON_AddStringToObject(j, "ProductGeographicIdentifier", val);
    }
    fclose(f);
    return j;
}

static cJSON *read_json_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 256 * 1024) { fclose(f); return NULL; }
    char *buf = imp_malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    buf[rd] = '\0';
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

static esp_err_t write_json_atomic(const char *path, const cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    if (!s) return ESP_ERR_NO_MEM;
    char tmp[340];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { free(s); return ESP_FAIL; }
    esp_err_t ret = (fputs(s, f) >= 0 && fputc('\n', f) == '\n')
                    ? imp_flush_close(f, tmp) : (fclose(f), ESP_FAIL);
    free(s);
    if (ret != ESP_OK) { unlink(tmp); return ret; }
    if (imp_rename_replace(tmp, path) != 0) { unlink(tmp); return ESP_FAIL; }
    return ESP_OK;
}

/* ── Protobuf spool encoder ─────────────────────────────────────────── */

typedef struct { uint8_t *buf; size_t len, cap; bool ovf; } pbuf_t;

static void pbuf_put(pbuf_t *b, const void *d, size_t n)
{
    if (b->ovf) return;
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n) nc *= 2;
        uint8_t *t = imp_realloc(b->buf, nc);
        if (!t) { b->ovf = true; return; }
        b->buf = t; b->cap = nc;
    }
    memcpy(b->buf + b->len, d, n);
    b->len += n;
}

static void pbuf_varint(pbuf_t *b, uint64_t v)
{
    uint8_t tmp[10]; int n = 0;
    do { tmp[n++] = (uint8_t)(v | 0x80); v >>= 7; } while (v);
    tmp[n - 1] &= 0x7F;
    pbuf_put(b, tmp, n);
}

static void pbuf_field_varint(pbuf_t *b, int field, int64_t v)
{
    pbuf_varint(b, (uint64_t)(field << 3));
    pbuf_varint(b, (uint64_t)v);   /* negative → 10-byte two's complement */
}

static void pbuf_field_bytes(pbuf_t *b, int field, const uint8_t *d, size_t n)
{
    pbuf_varint(b, ((uint64_t)field << 3) | 2);
    pbuf_varint(b, n);
    pbuf_put(b, d, n);
}

/* Field numbers — mirrors edf_summary.c's SUM_F_* / sub-field map. */
enum {
    SF_PERIOD_START = 2, SF_PERIOD_END = 3, SF_DURATION_MIN = 5,
    SF_SESSION_MODE = 6,
    SF_AHI = 7, SF_AI = 8, SF_HI = 9, SF_OAI = 10, SF_CAI = 11,
    SF_UAI = 12, SF_RIN = 13, SF_LEAK = 14, SF_INSP_PRESS = 15,
    SF_CSR = 16, SF_SAU = 17, SF_SPONT_TRIG = 18, SF_SPONT_CYC = 19,
    SF_EXP_PRESS = 20, SF_MEAN_MASK_PRESS = 21, SF_TIDAL_VOL = 22,
    SF_MIN_VENT = 23, SF_TGT_VENT = 24, SF_RESP_RATE = 25,
    SF_INSP_DUR = 26, SF_IE_RATIO = 27, SF_SPO2 = 28,
    SF_AMB_HUMID = 29, SF_HUM_TEMP = 30, SF_HTUBE_TEMP = 31,
    SF_HUM_POWER = 32, SF_HTUBE_POWER = 33,
    SF_HUM_CONNECTED = 34, SF_TUBE_CONNECTED = 35,
    SF_BLOWER_PRESS = 36, SF_RESP_FLOW = 37, SF_BLOWER_FLOW = 38,
    SF_SESSION_COUNT = 39, SF_CLOCK_B = 40,
};

/* A decoded STR record, ready to re-encode as a Summary protobuf day. */
typedef struct {
    int      day_number;                 /* "Date" signal (days since 1970) */
    int64_t  mask_on[20], mask_off[20];  /* minutes from local noon, -1 pad */
    int      n_pairs;
    int      duration_min;
    int      mode_dig;                   /* STR "Mode" digital value */
    /* scalars[field] holds the spool raw value; metric subs in metrics. */
    int64_t  scalars[64];
    bool     has_scalar[64];
    struct { int64_t v[6]; bool has[6]; } metrics[64];
} str_day_t;

/* STR label → (field, sub, num, den) inverting spool_to_edf/raw conventions.
 * sub==0 → plain scalar field; else metric submessage sub-field index.
 * raw = dig * den / num (inverse of spool_to_edf's raw*num/den). */
typedef struct { const char *label; int field, sub, num, den; } str_map_t;
static const str_map_t STR_MAP[] = {
    { "BlowPress.95",  SF_BLOWER_PRESS,     3, 1, 2 },
    { "BlowPress.5",   SF_BLOWER_PRESS,     1, 1, 2 },
    { "Flow.95",       SF_RESP_FLOW,        3, 5, 1 },
    { "Flow.5",        SF_RESP_FLOW,        1, 5, 1 },
    { "BlowFlow.50",   SF_BLOWER_FLOW,      2, 5, 1 },
    { "AmbHumidity.50",SF_AMB_HUMID,        2, 1, 10 },
    { "HumTemp.50",    SF_HUM_TEMP,         2, 1, 10 },
    { "HTubeTemp.50",  SF_HTUBE_TEMP,       2, 1, 10 },
    { "HTubePow.50",   SF_HTUBE_POWER,      2, 1, 10 },
    { "HumPow.50",     SF_HUM_POWER,        2, 1, 10 },
    { "SpO2.50",       SF_SPO2,             2, 1, 100 },
    { "SpO2.95",       SF_SPO2,             3, 1, 100 },
    { "SpO2.Max",      SF_SPO2,             4, 1, 100 },
    { "SpO2Thresh",    SF_SAU,              0, 1, 1 },
    { "SpontTrig%",    SF_SPONT_TRIG,       0, 1, 50 },
    { "SpontCyc%",     SF_SPONT_CYC,        0, 1, 50 },
    { "MaskPress.50",  SF_MEAN_MASK_PRESS,  2, 1, 2 },
    { "MaskPress.95",  SF_MEAN_MASK_PRESS,  3, 1, 2 },
    { "MaskPress.Max", SF_MEAN_MASK_PRESS,  4, 1, 2 },
    { "TgtIPAP.50",    SF_INSP_PRESS,       2, 1, 2 },
    { "TgtIPAP.95",    SF_INSP_PRESS,       3, 1, 2 },
    { "TgtIPAP.Max",   SF_INSP_PRESS,       4, 1, 2 },
    { "TgtEPAP.50",    SF_EXP_PRESS,        2, 1, 2 },
    { "TgtEPAP.95",    SF_EXP_PRESS,        3, 1, 2 },
    { "TgtEPAP.Max",   SF_EXP_PRESS,        4, 1, 2 },
    { "Leak.50",       SF_LEAK,             2, 1, 2 },
    { "Leak.95",       SF_LEAK,             4, 1, 2 },
    { "Leak.70",       SF_LEAK,             3, 1, 2 },
    { "Leak.Max",      SF_LEAK,             5, 1, 2 },
    { "MinVent.50",    SF_MIN_VENT,         2, 2, 25 },
    { "MinVent.95",    SF_MIN_VENT,         3, 2, 25 },
    { "MinVent.Max",   SF_MIN_VENT,         4, 2, 25 },
    { "RespRate.50",   SF_RESP_RATE,        2, 1, 20 },
    { "RespRate.95",   SF_RESP_RATE,        3, 1, 20 },
    { "RespRate.Max",  SF_RESP_RATE,        4, 1, 20 },
    { "TidVol.50",     SF_TIDAL_VOL,        2, 1, 2 },
    { "TidVol.95",     SF_TIDAL_VOL,        3, 1, 2 },
    { "TidVol.Max",    SF_TIDAL_VOL,        4, 1, 2 },
    { "TgtVent.50",    SF_TGT_VENT,         2, 2, 25 },
    { "TgtVent.95",    SF_TGT_VENT,         3, 2, 25 },
    { "TgtVent.Max",   SF_TGT_VENT,         4, 2, 25 },
    { "IERatio.50",    SF_IE_RATIO,         2, 1, 100 },
    { "IERatio.95",    SF_IE_RATIO,         3, 1, 100 },
    { "IERatio.Max",   SF_IE_RATIO,         4, 1, 100 },
    { "Ti.50",         SF_INSP_DUR,         2, 1, 20 },
    { "Ti.95",         SF_INSP_DUR,         3, 1, 20 },
    { "Ti.Max",        SF_INSP_DUR,         4, 1, 20 },
    { "AHI",           SF_AHI,              0, 1, 10 },
    { "HI",            SF_HI,               0, 1, 10 },
    { "AI",            SF_AI,               0, 1, 10 },
    { "OAI",           SF_OAI,              0, 1, 10 },
    { "CAI",           SF_CAI,              0, 1, 10 },
    { "UAI",           SF_UAI,              0, 1, 10 },
    { "RIN",           SF_RIN,              0, 1, 10 },
    { "CSR",           SF_CSR,              0, 1, 1 },
    { "HeatedTube",    SF_TUBE_CONNECTED,   0, 1, 1 },
    { "Humidifier",    SF_HUM_CONNECTED,    0, 1, 1 },
};

static int16_t str_dig_at(const uint8_t *rec, const imp_sig_t *sg, int sub)
{
    int16_t v = -1;
    if (sub < sg->spr)
        memcpy(&v, rec + sg->rec_off + sub * 2, 2);
    return v;
}

/* Decode one STR record into str_day_t (label-driven — works for AS10
 * variants whose field catalogue differs from AS11's). */
static void decode_str_record(const imp_edf_t *e, const uint8_t *rec,
                              str_day_t *d)
{
    memset(d, 0, sizeof(*d));
    d->day_number = -1;
    d->mode_dig = -1;
    for (int k = 0; k < 20; k++) d->mask_on[k] = d->mask_off[k] = -1;

    int i_date  = imp_sig_find(e, "Date");
    int i_on    = imp_sig_find(e, "MaskOn");
    int i_off   = imp_sig_find(e, "MaskOff");
    int i_dur   = imp_sig_find(e, "Duration");
    int i_mode  = imp_sig_find(e, "Mode");

    if (i_date >= 0) d->day_number = str_dig_at(rec, &e->sigs[i_date], 0);
    if (i_dur  >= 0) d->duration_min = str_dig_at(rec, &e->sigs[i_dur], 0);
    if (i_mode >= 0) d->mode_dig = str_dig_at(rec, &e->sigs[i_mode], 0);
    if (i_on >= 0 && i_off >= 0) {
        for (int k = 0; k < 20 && d->n_pairs < 20; k++) {
            int on  = str_dig_at(rec, &e->sigs[i_on], k);
            int off = str_dig_at(rec, &e->sigs[i_off], k);
            if (on < 0 && off < 0) continue;
            if (on < 0)  on = off;
            if (off < 0) off = on;
            if (off < on) off = on;
            d->mask_on[d->n_pairs]  = on;
            d->mask_off[d->n_pairs] = off;
            d->n_pairs++;
        }
    }

    for (size_t m = 0; m < sizeof(STR_MAP) / sizeof(STR_MAP[0]); m++) {
        const str_map_t *mp = &STR_MAP[m];
        int i = imp_sig_find(e, mp->label);
        if (i < 0) continue;
        int16_t dig = str_dig_at(rec, &e->sigs[i], 0);
        if (dig == -1 || dig == INT16_MIN) continue;
        int64_t raw = (int64_t)dig * mp->den / mp->num;
        if (mp->sub == 0) {
            d->scalars[mp->field] = raw;
            d->has_scalar[mp->field] = true;
        } else {
            d->metrics[mp->field].v[mp->sub] = raw;
            d->metrics[mp->field].has[mp->sub] = true;
        }
    }
}

/* Encode str_day_t as a Summary protobuf day record into *b. */
static void encode_spool_day(pbuf_t *b, const str_day_t *d,
                             int64_t period_start_ms, int64_t period_end_ms)
{
    pbuf_field_varint(b, SF_PERIOD_START, period_start_ms);
    pbuf_field_varint(b, SF_PERIOD_END, period_end_ms);
    pbuf_field_varint(b, SF_DURATION_MIN, d->duration_min);

    /* Field 6 — SessionModeEntries: one LD field holding repeated wrapper
     * submessages {sub1: maskon_ts_ms, sub2: duration_min}.  ts stays in
     * the raw device clock domain (drift applied by consumers). */
    if (d->n_pairs > 0) {
        pbuf_t inner = {0};
        for (int k = 0; k < d->n_pairs; k++) {
            pbuf_t ent = {0};
            pbuf_field_varint(&ent, 1, period_start_ms + d->mask_on[k] * 60000);
            pbuf_field_varint(&ent, 2, d->mask_off[k] - d->mask_on[k]);
            pbuf_field_bytes(&inner, 1, ent.buf, ent.len);
            free(ent.buf);
        }
        pbuf_field_bytes(b, SF_SESSION_MODE, inner.buf, inner.len);
        free(inner.buf);
    }

    for (int f = 0; f < 64; f++) {
        if (d->has_scalar[f]) {
            if (f == SF_PERIOD_START || f == SF_PERIOD_END ||
                f == SF_DURATION_MIN) continue;
            pbuf_field_varint(b, f, d->scalars[f]);
        }
        bool any = false;
        for (int s = 1; s < 6; s++) if (d->metrics[f].has[s]) any = true;
        if (any) {
            pbuf_t m = {0};
            for (int s = 1; s < 6; s++)
                if (d->metrics[f].has[s])
                    pbuf_field_varint(&m, s, d->metrics[f].v[s]);
            pbuf_field_bytes(b, f, m.buf, m.len);
            free(m.buf);
        }
    }
    pbuf_field_varint(b, SF_SESSION_COUNT, d->n_pairs);
    pbuf_field_varint(b, SF_CLOCK_B, period_end_ms);
}

/* ── STR.edf import → summaries/<day>.spool (+ <day>.settings.json) ─── */

struct edf_import_ctx {
    cJSON  *ident;          /* resolved flat identification tree */
    cJSON  *settings;       /* resolved SettingProfiles-ish tree */
    char    src_family[8];  /* "AS10"/"AS11" for provenance */
    char    src_serial[40];
    bool    root_done;
    bool    cleaned;
    bool    sweep_done;
    int     quiesce_s;      /* defer units modified within this window; 0 = off */
    imp_edf_t *scratch;     /* reusable header buffer */
};

/* STR Mode digital → ActiveTherapyProfile name (inverse of MODE_MAP). */
static const char *mode_to_profile(int dig, bool as10)
{
    /* AS11 MODE_MAP = {3,1,2,4,10,16,8,6,7,5,9}: profile index → dig. */
    static const struct { int dig; const char *name; } T[] = {
        {3, "CpapProfile"}, {1, "AutoSetProfile"}, {2, "AutoSetForHerProfile"},
        {4, "SpontProfile"}, {10, "STProfile"}, {16, "TimedProfile"},
        {8, "VAutoProfile"}, {6, "ASVProfile"}, {7, "ASVAutoProfile"},
        {5, "iVAPSProfile"}, {9, "PACProfile"},
    };
    (void)as10;   /* AS10 mode codes are label-matched the same way */
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        if (T[i].dig == dig) return T[i].name;
    return NULL;
}

/* Write summaries/<day>.spool (atomic) for one decoded STR day. */
static esp_err_t write_spool_for_day(const str_day_t *d, bool overwrite, bool *wrote)
{
    if (wrote) *wrote = false;
    /* Date is days since epoch → noon-day label = that civil date */
    if (d->day_number <= 0) return ESP_ERR_INVALID_ARG;
    int y, mo, dd;
    imp_civil_from_days(d->day_number, &y, &mo, &dd);
    char label[9];
    snprintf(label, sizeof(label), "%04d%02d%02d", y, mo, dd);

    char spool_path[330];
    snprintf(spool_path, sizeof(spool_path), "%s/%s.spool",
             SD_SUMMARIES_DIR, label);
    if (!overwrite) {
        struct stat st;
        if (stat(spool_path, &st) == 0) return ESP_OK;   /* live/imported wins */
    }

    /* PeriodStart = local noon of the label, in the device domain */
    int64_t noon_ms = as11_time_local_noon_epoch(label) * 1000;
    if (noon_ms <= 0) noon_ms = d->day_number * 86400000LL + 43200000LL;
    pbuf_t b = {0};
    encode_spool_day(&b, d, noon_ms, noon_ms + 86400000LL);
    esp_err_t ret = b.ovf ? ESP_ERR_NO_MEM : ESP_OK;
    if (ret == ESP_OK) {
        mkdir(SD_SESSIONS_DIR, 0775);
        mkdir(SD_SUMMARIES_DIR, 0775);
        char tmp[340];
        snprintf(tmp, sizeof(tmp), "%s.part", spool_path);
        FILE *f = fopen(tmp, "wb");
        if (!f) ret = ESP_FAIL;
        else {
            if (fwrite(b.buf, 1, b.len, f) != b.len) ret = (fclose(f), ESP_FAIL);
            else ret = imp_flush_close(f, tmp);
            if (ret == ESP_OK && imp_rename_replace(tmp, spool_path) != 0)
                ret = ESP_FAIL;
            if (ret != ESP_OK) unlink(tmp);
        }
    }
    free(b.buf);
    if (ret == ESP_OK && wrote) *wrote = true;
    return ret;
}

/* cJSON convenience helpers: get-or-create a child object, and
 * set-or-replace a scalar member (STR overlay may rewrite existing
 * CurrentSettings fields with that record's values). */
static cJSON *jget_or_add_obj(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsObject(v)) return v;
    cJSON *n = cJSON_CreateObject();
    if (n) cJSON_AddItemToObject(o, k, n);
    return n;
}

static void jset_num(cJSON *o, const char *k, double v)
{
    if (cJSON_GetObjectItem(o, k))
        cJSON_ReplaceItemInObject(o, k, cJSON_CreateNumber(v));
    else
        cJSON_AddNumberToObject(o, k, v);
}

static void jset_str(cJSON *o, const char *k, const char *v)
{
    if (cJSON_GetObjectItem(o, k))
        cJSON_ReplaceItemInObject(o, k, cJSON_CreateString(v));
    else
        cJSON_AddStringToObject(o, k, v);
}

/* STR "S.*" signal → SettingProfiles field, the exact inverse of
 * build_str_data_values() in edf_summary.c (spec/0002 §4.3.4).
 * div: numeric scale — JSON value = dig / div.
 * enums: NULL → numeric field; else raw index → JSON string.
 * off11: AS11 stores raw+off11 (1 for most enums, 2 for S.Mask);
 *        AS10 carries the raw 0-indexed enum directly. */
static const char *const EN_ON_OFF[]      = { "Off", "On", NULL };
static const char *const EN_ON_OFF_AUTO[] = { "Off", "On", "Auto", NULL };
static const char *const EN_EPR_TYPE[]    = { "RampOnly", "FullTime", NULL };
static const char *const EN_PATVIEW[]     = { "Advanced", "Basic", NULL };
static const char *const EN_YESNO[]       = { "No", "Yes", NULL };
static const char *const EN_MASK[] =
    { "Pillows", "FullFace", "Nasal", "Pediatric", NULL };
static const char *const EN_TUBE[] =
    { "SlimLine", "Standard", "15mmNonHeated", "19mmNonHeated", NULL };
static const char *const EN_CLIMATE[]     = { "Auto", "Manual", NULL };

typedef struct {
    const char *label;
    bool        feature;        /* false → TherapyProfiles, true → FeatureProfiles */
    const char *profile;
    const char *field;
    int16_t     div;
    const char *const *enums;
    int8_t      off11;
    bool        clamp;          /* out-of-range raw clamps to last enum (sparse maps) */
} str_set_map_t;

static const str_set_map_t STR_SETTINGS[] = {
    /* [6-13] CPAP / AutoSet / AFH pressures (cmH2O × 50) */
    { "S.C.StartPress",   false, "CpapProfile",          "StartPressure",   50, NULL, 0, false },
    { "S.C.Press",        false, "CpapProfile",          "SetPressure",     50, NULL, 0, false },
    { "S.A.StartPress",   false, "AutoSetProfile",       "StartPressure",   50, NULL, 0, false },
    { "S.A.MaxPress",     false, "AutoSetProfile",       "MaxPressure",     50, NULL, 0, false },
    { "S.A.MinPress",     false, "AutoSetProfile",       "MinPressure",     50, NULL, 0, false },
    { "S.AFH.StartPress", false, "AutoSetForHerProfile", "StartPressure",   50, NULL, 0, false },
    { "S.AFH.MaxPress",   false, "AutoSetForHerProfile", "MaxPressure",     50, NULL, 0, false },
    { "S.AFH.MinPress",   false, "AutoSetForHerProfile", "MinPressure",     50, NULL, 0, false },
    /* [14-21] VAuto */
    { "S.VA.StartPress",  false, "VAutoProfile", "StartPressure",          50, NULL, 0, false },
    { "S.VA.MaxIPAP",     false, "VAutoProfile", "MaxInspiratoryPressure", 50, NULL, 0, false },
    { "S.VA.MinEPAP",     false, "VAutoProfile", "MinExpiratoryPressure",  50, NULL, 0, false },
    { "S.VA.PS",          false, "VAutoProfile", "SetPressureSupport",     50, NULL, 0, false },
    { "S.VA.TiMax",       false, "VAutoProfile", "SetMaxInspiratoryTime",  50, NULL, 0, false },
    { "S.VA.TiMin",       false, "VAutoProfile", "SetMinInspiratoryTime",  50, NULL, 0, false },
    { "S.VA.Trigger",     false, "VAutoProfile", "TriggerSensitivity",      1, NULL, 1, false },
    { "S.VA.Cycle",       false, "VAutoProfile", "CycleSensitivity",        1, NULL, 1, false },
    /* [22-32] Spont (S) */
    { "S.S.StartPress",   false, "SpontProfile", "StartPressure",             50, NULL, 0, false },
    { "S.S.IPAP",         false, "SpontProfile", "TargetInspiratoryPressure",50, NULL, 0, false },
    { "S.S.EPAP",         false, "SpontProfile", "TargetExpiratoryPressure", 50, NULL, 0, false },
    { "S.S.EasyBreathe",  false, "SpontProfile", "EasyBreatheEnable",          1, EN_ON_OFF, 1, false },
    { "S.S.RespRateEn",   false, "SpontProfile", "RespiratoryRateEnable",      1, EN_ON_OFF, 1, true },
    { "S.S.TiMax",        false, "SpontProfile", "SetMaxInspiratoryTime",     50, NULL, 0, false },
    { "S.S.TiMin",        false, "SpontProfile", "SetMinInspiratoryTime",     50, NULL, 0, false },
    { "S.S.RiseEnable",   false, "SpontProfile", "RiseTimeEnable",             1, EN_ON_OFF, 1, false },
    { "S.S.RiseTime",     false, "SpontProfile", "RiseTime",                   1, NULL, 0, false },
    { "S.S.Trigger",      false, "SpontProfile", "TriggerSensitivity",         1, NULL, 1, false },
    { "S.S.Cycle",        false, "SpontProfile", "CycleSensitivity",           1, NULL, 1, false },
    /* [33-42] ST */
    { "S.ST.StartPress",  false, "STProfile", "StartPressure",             50, NULL, 0, false },
    { "S.ST.IPAP",        false, "STProfile", "TargetInspiratoryPressure", 50, NULL, 0, false },
    { "S.ST.EPAP",        false, "STProfile", "TargetExpiratoryPressure",  50, NULL, 0, false },
    { "S.ST.RespRate",    false, "STProfile", "SetRespiratoryRate",         5, NULL, 0, false },
    { "S.ST.TiMax",       false, "STProfile", "SetMaxInspiratoryTime",     50, NULL, 0, false },
    { "S.ST.TiMin",       false, "STProfile", "SetMinInspiratoryTime",     50, NULL, 0, false },
    { "S.ST.RiseEnable",  false, "STProfile", "RiseTimeEnable",             1, EN_ON_OFF, 1, false },
    { "S.ST.RiseTime",    false, "STProfile", "RiseTime",                   1, NULL, 0, false },
    { "S.ST.Trigger",     false, "STProfile", "TriggerSensitivity",         1, NULL, 1, false },
    { "S.ST.Cycle",       false, "STProfile", "CycleSensitivity",           1, NULL, 1, false },
    /* [43-49] Timed (T) */
    { "S.T.StartPress",   false, "TimedProfile", "StartPressure",             50, NULL, 0, false },
    { "S.T.IPAP",         false, "TimedProfile", "TargetInspiratoryPressure", 50, NULL, 0, false },
    { "S.T.EPAP",         false, "TimedProfile", "TargetExpiratoryPressure",  50, NULL, 0, false },
    { "S.T.RespRate",     false, "TimedProfile", "SetRespiratoryRate",         5, NULL, 0, false },
    { "S.T.Ti",           false, "TimedProfile", "SetInspiratoryTime",        50, NULL, 0, false },
    { "S.T.RiseEnable",   false, "TimedProfile", "RiseTimeEnable",             1, EN_ON_OFF, 1, false },
    { "S.T.RiseTime",     false, "TimedProfile", "RiseTime",                   1, NULL, 0, false },
    /* [50-58] ASV / ASVAuto */
    { "S.AV.StartPress",  false, "ASVProfile", "StartPressure",             50, NULL, 0, false },
    { "S.AV.EPAP",        false, "ASVProfile", "TargetExpiratoryPressure",  50, NULL, 0, false },
    { "S.AV.MaxPS",       false, "ASVProfile", "MaxPressureSupport",        50, NULL, 0, false },
    { "S.AV.MinPS",       false, "ASVProfile", "MinPressureSupport",        50, NULL, 0, false },
    { "S.AA.StartPress",  false, "ASVAutoProfile", "StartPressure",           50, NULL, 0, false },
    { "S.AA.MaxEPAP",     false, "ASVAutoProfile", "MaxExpiratoryPressure",   50, NULL, 0, false },
    { "S.AA.MinEPAP",     false, "ASVAutoProfile", "MinExpiratoryPressure",   50, NULL, 0, false },
    { "S.AA.MaxPS",       false, "ASVAutoProfile", "MaxPressureSupport",      50, NULL, 0, false },
    { "S.AA.MinPS",       false, "ASVAutoProfile", "MinPressureSupport",      50, NULL, 0, false },
    /* [59-75] Comfort / ramp / EPR / smart / circuit / climate features */
    { "S.AS.Comfort",     true, "ComfortFeature",        "AutoSetComfort",          1, EN_ON_OFF,      1, false },
    { "S.RampEnable",     true, "AutoRampFeature",       "RampEnable",              1, EN_ON_OFF_AUTO, 1, false },
    { "S.RampTime",       true, "AutoRampFeature",       "RampTime",                1, NULL,           0, false },
    { "S.EPR.ClinEnable", true, "EprFeature",            "EprEnablePatientAccess",  1, EN_ON_OFF,      1, false },
    { "S.EPR.EPREnable",  true, "EprFeature",            "EprEnable",               1, EN_ON_OFF,      1, false },
    { "S.EPR.Level",      true, "EprFeature",            "EprPressure",            50, NULL,           0, false },
    { "S.EPR.EPRType",    true, "EprFeature",            "EprType",                 1, EN_EPR_TYPE,    1, false },
    { "S.SmartStart",     true, "SmartStartStopFeature", "SmartStart",              1, EN_ON_OFF,      1, false },
    { "S.PtAccess",       true, "PatientViewFeature",    "PatientView",             1, EN_PATVIEW,     1, false },
    { "S.ABFilter",       true, "CircuitFeature",        "AntiBacterialFilter",     1, EN_YESNO,       1, false },
    { "S.Mask",           true, "CircuitFeature",        "MaskType",                1, EN_MASK,        2, false },
    { "S.Tube",           true, "CircuitFeature",        "TubeType",                1, EN_TUBE,        1, false },
    { "S.ClimateControl", true, "ClimateFeature",        "ClimateControl",          1, EN_CLIMATE,     1, false },
    { "S.HumEnable",      true, "ClimateFeature",        "HumidifierSettingEnable", 1, EN_ON_OFF,      1, false },
    { "S.HumLevel",       true, "ClimateFeature",        "HumidifierLevel",         1, NULL,           0, false },
    { "S.TempEnable",     true, "ClimateFeature",        "HeatedTubeSettingEnable", 1, EN_ON_OFF_AUTO, 1, false },
    { "S.Temp",           true, "ClimateFeature",        "HeatedTubeTemperature",  10, NULL,           0, false },
};

/* Overlay one STR record's S.* settings channels onto a SettingProfiles
 * tree (inverse of build_str_data_values).  Only channels the record
 * actually carries are written; absent channels keep whatever the tree
 * already had (e.g. the card's CurrentSettings).  Returns the number of
 * S.* fields applied — 0 means the tree gained no settings content. */
static int overlay_str_settings(cJSON *root, const imp_edf_t *e,
                                const uint8_t *rec, int mode_dig, bool as10)
{
    cJSON *sp = jget_or_add_obj(root, "SettingProfiles");
    if (!sp) return 0;
    int applied = 0;
    const char *prof = mode_to_profile(mode_dig, as10);
    if (prof) {
        cJSON *ap = jget_or_add_obj(sp, "ActiveProfiles");
        if (ap) { jset_str(ap, "TherapyProfile", prof); applied++; }
    }
    cJSON *tp = NULL, *fp = NULL;
    for (size_t i = 0; i < sizeof(STR_SETTINGS) / sizeof(STR_SETTINGS[0]); i++) {
        const str_set_map_t *mp = &STR_SETTINGS[i];
        int si = imp_sig_find(e, mp->label);
        if (si < 0) continue;
        int16_t dig = str_dig_at(rec, &e->sigs[si], 0);
        if (dig == -1 || dig == INT16_MIN) continue;
        cJSON *po;
        if (mp->feature) {
            if (!fp) fp = jget_or_add_obj(sp, "FeatureProfiles");
            po = fp ? jget_or_add_obj(fp, mp->profile) : NULL;
        } else {
            if (!tp) tp = jget_or_add_obj(sp, "TherapyProfiles");
            po = tp ? jget_or_add_obj(tp, mp->profile) : NULL;
        }
        if (!po) continue;
        if (mp->enums) {
            int raw = as10 ? (int)dig : (int)dig - mp->off11;
            if (raw < 0) continue;
            int last = 0;
            while (mp->enums[last]) last++;
            last--;
            if (raw > last) {
                if (!mp->clamp) continue;
                raw = last;
            }
            jset_str(po, mp->field, mp->enums[raw]);
        } else if (mp->off11) {
            /* Numeric enum index (Trigger/Cycle sensitivity, 0..6). */
            int raw = as10 ? (int)dig : (int)dig - mp->off11;
            if (raw < 0 || raw > 6) continue;
            jset_num(po, mp->field, raw);
        } else {
            jset_num(po, mp->field, (double)dig / (double)mp->div);
        }
        applied++;
    }
    return applied;
}

/* Import every record of an STR.edf into per-day spools + day settings.
 * summaries/<day>.settings.json = the card's CurrentSettings tree (when
 * present) overlaid with that record's S.* channels — never a stub that
 * shadows the real tree in edf_generate_str_edf's priority lookup.
 * With no CurrentSettings (AS10 cards), the newest record's S.*-only
 * reconstruction becomes ctx->settings so session sidecars still carry
 * a usable tree. */
static int import_str_edf(const char *path, edf_import_ctx_t *ctx,
                          edf_import_unit_t *u)
{
    imp_edf_t *e = imp_malloc(sizeof(imp_edf_t));
    if (!e) return 0;
    if (imp_edf_open(e, path) != ESP_OK) { free(e); return 0; }

    /* Platform family: Identification.* wins when resolved; else the
     * STR's own recording_id carries SRN=/MID=/VID= (MID < 40 → AS10,
     * which also selects 0-indexed enum decoding for S.* channels). */
    if (!ctx->src_family[0] || !ctx->ident) {
        cJSON *rid = ident_from_rec_id(e->rec_id);
        if (rid) {
            const cJSON *mid = cJSON_GetObjectItem(rid, "PlatformIdentifier");
            const cJSON *sn  = cJSON_GetObjectItem(rid, "SerialNumber");
            if (!ctx->src_family[0] && mid && cJSON_IsNumber(mid))
                snprintf(ctx->src_family, sizeof(ctx->src_family), "%s",
                         mid->valueint >= 40 ? "AS11" : "AS10");
            if (!ctx->src_serial[0] && sn && cJSON_IsString(sn))
                snprintf(ctx->src_serial, sizeof(ctx->src_serial), "%s",
                         sn->valuestring);
            if (!ctx->ident) ctx->ident = rid;
            else cJSON_Delete(rid);
        }
    }
    bool as10 = !strcmp(ctx->src_family, "AS10");

    FILE *f = fopen(path, "rb");
    if (!f) { free(e); return 0; }
    uint8_t *rec = imp_malloc((size_t)e->rec_stride);
    str_day_t *d = imp_calloc(1, sizeof(*d));
    if (!rec || !d) { free(rec); free(d); fclose(f); free(e); return 0; }

    int written = 0;
    cJSON *latest = NULL;   /* newest S.* reconstruction (no CurrentSettings) */
    int64_t n = e->n_records;
    if (n > IMP_MAX_STR_REC) n = IMP_MAX_STR_REC;
    for (int64_t r = 0; r < n; r++) {
        if (!imp_edf_read_rec(e, f, r, rec)) break;
        if (!imp_edf_rec_crc_ok(e, rec)) continue;   /* corrupt day → skip */
        decode_str_record(e, rec, d);
        if (d->day_number <= 0) continue;
        bool spool_new = false;
        if (write_spool_for_day(d, false, &spool_new) != ESP_OK) continue;
        /* Only a spool this pass created counts: re-importing a card whose
         * days already exist must not look like new data (the worker would
         * rebuild and re-offer the newest day for nothing). */
        if (spool_new) written++;

        int y, mo, dd;
        imp_civil_from_days(d->day_number, &y, &mo, &dd);
        char spath[340];
        snprintf(spath, sizeof(spath), "%s/%04d%02d%02d.settings.json",
                 SD_SUMMARIES_DIR, y, mo, dd);
        cJSON *root = ctx->settings ? cJSON_Duplicate(ctx->settings, true)
                                    : cJSON_CreateObject();
        if (!root) continue;
        int applied = overlay_str_settings(root, e, rec, d->mode_dig, as10);
        struct stat st;
        /* Only write when the file carries real settings — an empty stub
         * would shadow richer per-session settings.json files in the
         * edf_summary.c priority chain. */
        if ((ctx->settings || applied > 0) && stat(spath, &st) != 0) {
            mkdir(SD_SESSIONS_DIR, 0775);
            mkdir(SD_SUMMARIES_DIR, 0775);
            write_json_atomic(spath, root);
        }
        if (!ctx->settings) { cJSON_Delete(latest); latest = root; }
        else cJSON_Delete(root);
    }
    if (latest) ctx->settings = latest;
    free(d);
    free(rec);
    fclose(f);
    free(e);
    u->spools_written += written;
    return written;
}

/* Refresh device_metadata.json when the root pass resolved ident or
 * settings — mirrors save_device_metadata_snapshot's {"serial","ident",
 * "settings"} merge semantics (old settings carry over only for the same
 * machine serial). */
static void write_device_metadata(const edf_import_ctx_t *ctx)
{
    if (!ctx->ident && !ctx->settings) return;
    cJSON *existing = read_json_file(SD_DEVICE_METADATA_PATH);
    const cJSON *old_ident = existing ? cJSON_GetObjectItem(existing, "ident") : NULL;
    const cJSON *old_settings = existing ? cJSON_GetObjectItem(existing, "settings") : NULL;
    if (!cJSON_IsObject(old_ident)) old_ident = NULL;
    if (!cJSON_IsObject(old_settings)) old_settings = NULL;

    const cJSON *use_ident = ctx->ident ? ctx->ident : old_ident;
    if (!use_ident) { cJSON_Delete(existing); return; }
    const cJSON *sn = cJSON_GetObjectItem(use_ident, "SerialNumber");
    const char *serial = (sn && cJSON_IsString(sn) && sn->valuestring)
                         ? sn->valuestring : "";
    const char *old_serial = "";
    if (old_ident) {
        const cJSON *os = cJSON_GetObjectItem(old_ident, "SerialNumber");
        if (os && cJSON_IsString(os) && os->valuestring)
            old_serial = os->valuestring;
    }
    const cJSON *use_settings = ctx->settings;
    if (!use_settings && old_settings &&
        (!ctx->ident || !strcmp(serial, old_serial)))
        use_settings = old_settings;

    cJSON *snap = cJSON_CreateObject();
    if (snap) {
        cJSON_AddStringToObject(snap, "serial", serial);
        cJSON_AddItemToObject(snap, "ident",
                              cJSON_Duplicate((cJSON *)use_ident, true));
        if (use_settings)
            cJSON_AddItemToObject(snap, "settings",
                                  cJSON_Duplicate((cJSON *)use_settings, true));
        mkdir(SD_SESSIONS_DIR, 0775);
        write_json_atomic(SD_DEVICE_METADATA_PATH, snap);
        cJSON_Delete(snap);
    }
    cJSON_Delete(existing);
}

/* ── SomnoBridge drift sidecar (<day>/_bridge_drift.json) ───────────── */

typedef struct {
    int64_t  drift_ms;          /* effective drift actually applied (0 when !usable) */
    int64_t  half_ms;           /* accuracy bound of the chosen record, -1 absent */
    int64_t  measured_at_ms;    /* reference_at_ms from the sidecar, 0 when absent */
    bool     valid, usable;
    char     source[48];     /* clock_drift_source / import provenance */
    /* Source-device fixed zone: civil times inside the EDFs are the
     * device's own local civil — when the bridge confirms a fixed-offset
     * rule it wins over the ESP's own zone for device-domain parsing.
     * INT32_MIN = no confirmed rule (ESP-local interpretation kept). */
    int32_t  dev_tz_off_s;
    char     dev_tz_rule[40];
    cJSON   *raw;            /* full sidecar JSON, embedded in the manifest */
} imp_drift_t;

/* SomnoBridge serialises 64-bit values as decimal strings; accept strings
 * or plain numbers. */
static bool json_i64(const cJSON *o, const char *key, int64_t *out)
{
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    if (!v) return false;
    if (cJSON_IsNumber(v)) { *out = (int64_t)v->valuedouble; return true; }
    if (cJSON_IsString(v) && v->valuestring) {
        *out = (int64_t)strtoll(v->valuestring, NULL, 10);
        return true;
    }
    return false;
}

static bool json_bool(const cJSON *o, const char *key, bool dflt)
{
    const cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    return (v && cJSON_IsBool(v)) ? cJSON_IsTrue(v) : dflt;
}

/* A CPAP clock can plausibly be wrong by at most ~a timezone plus drift;
 * |drift| beyond 24 h is an epoch-conversion bug, not a measurement. */
#define IMP_DRIFT_MAX_MS     (24LL * 3600 * 1000)
#define IMP_DRIFT_USABLE_HALF 5000

/* schema-5 selection record (source_of_truth / best_available_offset):
 * drift = clock_drift_ms or the midpoint of interval_ms;
 * half = accuracy_half_ms (legacy half_ms accepted). */
static bool drift_sel_parse(const cJSON *r, int64_t *drift, int64_t *half,
                            int64_t *measured_at, const char **quality,
                            const char **view)
{
    int64_t v;
    if (!json_i64(r, "clock_drift_ms", &v)) {
        const cJSON *iv = cJSON_GetObjectItem(r, "interval_ms");
        const cJSON *lo = iv ? cJSON_GetArrayItem(iv, 0) : NULL;
        const cJSON *hi = iv ? cJSON_GetArrayItem(iv, 1) : NULL;
        if (!cJSON_IsNumber(lo) || !cJSON_IsNumber(hi)) return false;
        v = ((int64_t)lo->valuedouble + (int64_t)hi->valuedouble) / 2;
    }
    int64_t h = -1;
    if (!json_i64(r, "accuracy_half_ms", &h)) json_i64(r, "half_ms", &h);
    int64_t at = 0;
    if (!json_i64(r, "reference_at_ms", &at))
        json_i64(r, "computed_at_ms", &at);
    const cJSON *q = cJSON_GetObjectItem(r, "quality");
    const cJSON *vw = cJSON_GetObjectItem(r, "selected_view");
    if (!(vw && cJSON_IsString(vw)))
        vw = cJSON_GetObjectItem(r, "origin");
    *drift = v;
    *half = h;
    *measured_at = at;
    *quality = (q && cJSON_IsString(q)) ? q->valuestring : NULL;
    *view = (vw && cJSON_IsString(vw)) ? vw->valuestring : NULL;
    return true;
}

static bool estimate_state_ok(const cJSON *est)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(est,
                                                           "estimate_state"));
    return s && (!strcmp(s, "provisional") || !strcmp(s, "validated"));
}

/* Inverse of imp_civil_from_days (public-domain algorithm). */
static int64_t imp_days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int)doe - 719468;
}

/* The ESP local-time UTC offset (s) at an epoch instant — computed without
 * the non-portable tm_gmtoff, so the host test build sees the same math. */
static int64_t imp_esp_off_at(int64_t epoch_ms)
{
    time_t t = (time_t)(epoch_ms / 1000);
    struct tm lt;
    if (!localtime_r(&t, &lt)) return 0;
    int64_t naive = imp_days_from_civil(lt.tm_year + 1900, lt.tm_mon + 1,
                                        lt.tm_mday) * 86400
                  + (int64_t)lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
    return naive - (int64_t)t;
}

/* Parse a fixed-offset zone rule ("UTC-10", "AEST-10", "UTC+5:30") into
 * seconds EAST of UTC (local = utc + off).  POSIX sign is west-positive:
 * "UTC-10" means UTC+10, i.e. +36000.  Returns false for non-fixed or
 * unparseable rules (a DST tail means the rule isn't a fixed offset). */
static bool imp_parse_tz_offset(const char *rule, int32_t *off_s)
{
    if (!rule || !off_s) return false;
    const char *sign = NULL;
    for (const char *p = rule; *p; p++)
        if (*p == '+' || *p == '-') sign = p;
    if (!sign) return false;
    const char *q = sign + 1;
    int parts[3] = {0, 0, 0};
    int np = 0;
    while (np < 3 && *q >= '0' && *q <= '9') {
        char *end = NULL;
        long v = strtol(q, &end, 10);
        if (end == q) return false;
        parts[np++] = (int)v;
        q = (*end == ':') ? end + 1 : end;
        if (*end != ':') break;
    }
    if (*q != '\0' && *q != ',') return false;
    int64_t off = (int64_t)parts[0] * 3600 + parts[1] * 60 + parts[2];
    if (off > 14 * 3600) return false;
    *off_s = (int32_t)(*sign == '-' ? off : -off);
    return true;
}

/* Total shift applied at a device-domain instant: the measured drift plus
 * the zone delta between the source device's confirmed fixed rule and the
 * ESP's own zone — source civil times are device-local, and mktime()
 * reinterpreted them under the ESP zone, so the difference between the
 * two interpretations is the correction. */
static int64_t imp_shift_at(const imp_drift_t *d, int64_t t_dev_ms)
{
    int64_t s = d->drift_ms;
    if (d->dev_tz_off_s != INT32_MIN)
        s += (imp_esp_off_at(t_dev_ms) - (int64_t)d->dev_tz_off_s) * 1000;
    return s;
}

/* Selection order (schema-5 shapes; legacy field names accepted):
 *   1. source_of_truth            → fresh live qualified → valid+usable
 *   2. best_available_offset      → current or previous-session selection
 *   3. completed_session_fallback → eligible persisted record
 *   4. estimate                   → valid=false, usable iff half ≤ 5000
 *   5. none                       → drift 0, everything false
 * Every tier is bounded by |drift| ≤ 24 h — an epoch-conversion bug
 * produces values orders of magnitude beyond a real clock offset.
 * The whole sidecar is kept verbatim under "somnobridge_drift". */
static void parse_drift_sidecar(const char *path, imp_drift_t *d)
{
    memset(d, 0, sizeof(*d));
    d->dev_tz_off_s = INT32_MIN;
    d->half_ms = -1;
    snprintf(d->source, sizeof(d->source), "file_import");
    if (!path || !path[0]) return;

    d->raw = read_json_file(path);
    if (!d->raw) {
        ESP_LOGW(TAG, "import: %s unreadable — file_import drift", path);
        return;
    }

    const cJSON *est = cJSON_GetObjectItem(d->raw, "estimate");
    /* Device-zone rule applies independently of drift usability. */
    const char *rule = cJSON_GetStringValue(cJSON_GetObjectItem(est,
                                                              "timezone_rule"));
    if (rule && json_bool(est, "timezone_confirmed", false) &&
        imp_parse_tz_offset(rule, &d->dev_tz_off_s)) {
        snprintf(d->dev_tz_rule, sizeof(d->dev_tz_rule), "%s", rule);
    } else {
        d->dev_tz_off_s = INT32_MIN;
    }

    const cJSON *sot  = cJSON_GetObjectItem(d->raw, "source_of_truth");
    const cJSON *best = cJSON_GetObjectItem(d->raw, "best_available_offset");
    const cJSON *fb   = cJSON_GetObjectItem(d->raw,
                                          "completed_session_fallback");

    int64_t drift = 0, half = -1, at = 0, v = 0, h = -1;
    const char *src = NULL, *q = NULL, *view = NULL;
    bool valid = false, usable = false;

    if (cJSON_IsObject(sot) &&
        drift_sel_parse(sot, &v, &h, &at, &q, &view) &&
        (!q || strcmp(q, "none"))) {
        if (llabs(v) <= IMP_DRIFT_MAX_MS) {
            drift = v; half = h;
            valid = true; usable = (h < 0 || h <= IMP_DRIFT_USABLE_HALF);
            src = (q && q[0]) ? q : "sot";
        } else {
            ESP_LOGW(TAG, "import: %s sot drift %lld ms implausible",
                     path, (long long)v);
        }
    }
    if (!src && cJSON_IsObject(best) && json_bool(best, "valid", true) &&
        drift_sel_parse(best, &v, &h, &at, &q, &view) &&
        (!q || strcmp(q, "none"))) {
        if (llabs(v) <= IMP_DRIFT_MAX_MS) {
            drift = v; half = h;
            valid = true; usable = (h < 0 || h <= IMP_DRIFT_USABLE_HALF);
            src = (view && !strcmp(view, "previous_session_fallback"))
                ? "fallback"
                : (view && !strcmp(view, "current_session_estimate"))
                ? "current" : (q && q[0]) ? q : "best";
        } else {
            ESP_LOGW(TAG, "import: %s best drift %lld ms implausible",
                     path, (long long)v);
        }
    }
    if (!src && cJSON_IsObject(fb) &&
        json_bool(fb, "fallback_eligible", false)) {
        const cJSON *rec = cJSON_GetObjectItem(fb, "record");
        if (cJSON_IsObject(rec) && json_i64(rec, "clock_drift_ms", &v)) {
            if (llabs(v) <= IMP_DRIFT_MAX_MS) {
                if (!json_i64(fb, "adjusted_half_ms", &h))
                    json_i64(rec, "accuracy_half_ms", &h);
                if (!json_i64(rec, "reference_at_ms", &at))
                    json_i64(rec, "computed_at_ms", &at);
                drift = v; half = h;
                valid = true;
                usable = (h < 0 || h <= IMP_DRIFT_USABLE_HALF);
                src = "fallback";
            } else {
                ESP_LOGW(TAG, "import: %s fallback drift %lld ms implausible",
                         path, (long long)v);
            }
        }
    }
    if (!src && cJSON_IsObject(est) &&
        (json_bool(est, "has_drift", false) || estimate_state_ok(est)) &&
        json_i64(est, "clock_drift_ms", &v)) {
        if (llabs(v) <= IMP_DRIFT_MAX_MS) {
            if (!json_i64(est, "accuracy_half_ms", &h))
                json_i64(est, "half_ms", &h);
            if (!json_i64(est, "reference_at_ms", &at))
                json_i64(est, "computed_at_ms", &at);
            drift = v; half = h;
            valid = false;
            usable = (h >= 0 && h <= IMP_DRIFT_USABLE_HALF);
            src = "estimate";
        } else {
            ESP_LOGW(TAG, "import: %s estimate drift %lld ms implausible",
                     path, (long long)v);
        }
    }

    if (!src) {
        snprintf(d->source, sizeof(d->source), "somnobridge_none");
        return;
    }
    d->valid  = valid;
    d->usable = usable;
    d->half_ms = half;
    d->measured_at_ms = at;
    /* Only a usable drift may shift session timestamps (spec §4.2).  The
     * raw record stays inside somnobridge_drift for diagnostics. */
    d->drift_ms = d->usable ? drift : 0;
    snprintf(d->source, sizeof(d->source), "somnobridge_%s", src);
    ESP_LOGI(TAG, "import: %s drift=%lld ms, applied=%lld ms "
             "(%s, valid=%d usable=%d, half=%lld, tz=%s)",
             path, (long long)drift, (long long)d->drift_ms,
             d->source, (int)d->valid, (int)d->usable, (long long)half,
             d->dev_tz_off_s != INT32_MIN ? d->dev_tz_rule : "esp");
}

/* ── Session clustering ─────────────────────────────────────────────── */

typedef struct {
    const imp_file_t *brp, *pld, *sa2;
    const imp_file_t *eve, *csl;
    int64_t wav_ts;      /* waveform-cluster anchor (MaskOn domain), -1 none */
    int64_t eve_ts;      /* annotation-cluster anchor (TherapyStart), -1 none */
} imp_cluster_t;

static int file_ts_cmp(const void *a, const void *b)
{
    const imp_file_t *x = a, *y = b;
    if (x->ts_dev_ms != y->ts_dev_ms)
        return x->ts_dev_ms < y->ts_dev_ms ? -1 : 1;
    return strcmp(x->path, y->path);
}

static bool is_wave(imp_kind_t k) { return k == K_BRP || k == K_PLD || k == K_SA2; }
static bool is_ann(imp_kind_t k) { return k == K_EVE || k == K_CSL; }

/* Cluster a day's files into sessions.  `files` is sorted in place — the
 * returned cluster members point INTO it, so it must outlive `out`.
 * Wave files (BRP/PLD/SA2) sharing a timestamp ±2 s form a wave cluster;
 * annotation files (EVE/CSL) likewise form ann clusters.  Each ann cluster
 * pairs with the earliest wave cluster starting 0..60 s after it (MaskOn
 * follows TherapyStart); leftovers stand alone. */
static int cluster_files(imp_file_t *files, int n_files,
                         imp_cluster_t *out, int max_out)
{
    int n = n_files > IMP_MAX_FILES ? IMP_MAX_FILES : n_files;
    qsort(files, n, sizeof(imp_file_t), file_ts_cmp);

    int n_out = 0;
    /* 1:1 pairing by walking the sorted list: an ann file maps to the next
     * wave file within the match window; all wave files at one timestamp
     * share the wave anchor, all ann files at one timestamp share the ann
     * anchor. */
    typedef struct { int64_t ts; int idx; } grp_t;
    /* Zero-fill so cppcheck's uninitvar can see the initialized-prefix
     * invariant: reads are bounded by nw/na but the arrays are written
     * conditionally under `nw < IMP_MAX_SESSIONS` guards. */
    grp_t waves[IMP_MAX_SESSIONS] = {0}, anns[IMP_MAX_SESSIONS] = {0};
    int nw = 0, na = 0;

    for (int i = 0; i < n; i++) {
        const imp_file_t *f = &files[i];
        if (is_wave(f->kind)) {
            if (nw == 0 || f->ts_dev_ms - waves[nw - 1].ts > IMP_CLUSTER_MS)
                { if (nw < IMP_MAX_SESSIONS) waves[nw] = (grp_t){ f->ts_dev_ms, i }, nw++; }
        } else if (is_ann(f->kind)) {
            if (na == 0 || f->ts_dev_ms - anns[na - 1].ts > IMP_CLUSTER_MS)
                { if (na < IMP_MAX_SESSIONS) anns[na] = (grp_t){ f->ts_dev_ms, i }, na++; }
        }
    }

    /* Match ann clusters to wave clusters.  An unmatched ann cluster still
     * yields a session (mask-off run); unmatched wave clusters too. */
    int wi = 0;
    for (int ai = 0; ai < na && n_out < max_out; ai++) {
        int64_t eve_ts = anns[ai].ts;
        /* Wave clusters starting before this ann anchor (beyond tolerance)
         * belong to an earlier therapy — skip them. */
        while (wi < nw && waves[wi].ts < eve_ts - IMP_CLUSTER_MS)
            wi++;
        int match = -1;
        if (wi < nw) {
            int64_t delta = waves[wi].ts - eve_ts;
            if (delta >= -IMP_CLUSTER_MS && delta <= IMP_MATCH_MS)
                match = wi++;
        }
        imp_cluster_t *c = &out[n_out];
        memset(c, 0, sizeof(*c));
        c->eve_ts = eve_ts;
        c->wav_ts = (match >= 0) ? waves[match].ts : -1;
        n_out++;
    }
    for (; wi < nw && n_out < max_out; wi++) {
        imp_cluster_t *c = &out[n_out];
        memset(c, 0, sizeof(*c));
        c->eve_ts = -1;
        c->wav_ts = waves[wi].ts;
        n_out++;
    }

    /* Fill file slots per cluster. */
    for (int i = 0; i < n; i++) {
        const imp_file_t *f = &files[i];
        for (int c = 0; c < n_out; c++) {
            imp_cluster_t *cl = &out[c];
            int64_t anchor = is_wave(f->kind) ? cl->wav_ts : cl->eve_ts;
            if (anchor < 0 ||
                llabs(f->ts_dev_ms - anchor) > IMP_CLUSTER_MS) continue;
            switch (f->kind) {
                case K_BRP: if (!cl->brp) cl->brp = f; break;
                case K_PLD: if (!cl->pld) cl->pld = f; break;
                case K_SA2: if (!cl->sa2) cl->sa2 = f; break;
                case K_EVE: if (!cl->eve) cl->eve = f; break;
                case K_CSL: if (!cl->csl) cl->csl = f; break;
                default: break;
            }
            break;
        }
    }
    return n_out;
}

/* ── Commit protocol ────────────────────────────────────────────────── */

/* Marker written before any .part is staged: presence of
 * <id>_session.json.importing means "import in flight".  Orphan cleanup at
 * scan start removes it plus every <id>* file when no finished manifest
 * exists. */
static esp_err_t write_import_marker(const char *dir, const char *id)
{
    char p[448];
    snprintf(p, sizeof(p), "%s/%s_session.json.importing", dir, id);
    FILE *f = fopen(p, "w");
    if (!f) return ESP_FAIL;
    return imp_flush_close(f, p);
}

static void delete_import_marker(const char *dir, const char *id)
{
    char p[448];
    snprintf(p, sizeof(p), "%s/%s_session.json.importing", dir, id);
    unlink(p);
}

/* Rename every staged "<id>_*.part" in the streams dir to its final name. */
static esp_err_t publish_parts(const char *dir, const char *id)
{
    DIR *d = opendir(dir);
    if (!d) return ESP_FAIL;
    char stem[48];
    snprintf(stem, sizeof(stem), "%s_", id);
    size_t stem_len = strlen(stem);
    esp_err_t ret = ESP_OK;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && ret == ESP_OK) {
        const char *nm = ent->d_name;
        size_t nl = strlen(nm);
        if (nl <= stem_len + 5 || strncmp(nm, stem, stem_len) != 0) continue;
        if (strcmp(nm + nl - 5, ".part") != 0) continue;
        char from[640], to[640];
        snprintf(from, sizeof(from), "%s/%s", dir, nm);
        snprintf(to, sizeof(to), "%s/%.*s", dir, (int)(nl - 5), nm);
        if (imp_rename_replace(from, to) != 0) {
            ESP_LOGE(TAG, "import: publish %s: %s", nm, strerror(errno));
            ret = ESP_FAIL;
        }
    }
    closedir(d);
    return ret;
}

/* Boot/scan cleanup: drop incomplete imports (marker present, manifest
 * absent) and every stale *.part under streams/.  Public: main.c runs it
 * before session_writer_recover() so recovery never sees import residue. */
void edf_import_cleanup_residue(void)
{
    DIR *days = opendir(SD_STREAMS_DIR);
    if (!days) return;
    struct dirent *de;
    while ((de = readdir(days)) != NULL) {
        if (strlen(de->d_name) != 8) continue;
        char dpath[300];
        snprintf(dpath, sizeof(dpath), "%s/%.*s", SD_STREAMS_DIR, 8,
                 de->d_name);
        struct stat dst;
        if (stat(dpath, &dst) != 0 || !S_ISDIR(dst.st_mode)) continue;
        DIR *dd = opendir(dpath);
        if (!dd) continue;
        char dead[24][40];      /* ids whose .importing has no manifest */
        int n_dead = 0;
        struct dirent *fe;
        while ((fe = readdir(dd)) != NULL) {
            const char *nm = fe->d_name;
            size_t nl = strlen(nm);
            /* stale .part → importer residue, delete */
            if (nl > 5 && strcmp(nm + nl - 5, ".part") == 0) {
                char p[600];
                snprintf(p, sizeof(p), "%s/%s", dpath, nm);
                unlink(p);
                continue;
            }
            const char *mfx = "_session.json.importing";
            size_t ml = strlen(mfx);
            if (nl <= ml || strcmp(nm + nl - ml, mfx) != 0) continue;
            /* marker present: keep it only if the finished manifest is missing */
            char id[40], mp[600];
            size_t idl = nl - ml;
            if (idl >= sizeof(id)) continue;
            memcpy(id, nm, idl); id[idl] = '\0';
            snprintf(mp, sizeof(mp), "%s/%s_session.json", dpath, id);
            struct stat st;
            if (stat(mp, &st) == 0) {
                /* committed manifest exists → just drop the marker */
                snprintf(mp, sizeof(mp), "%s/%s", dpath, nm);
                unlink(mp);
            } else if (n_dead < 24) {
                memcpy(dead[n_dead++], id, idl + 1);
            }
        }
        closedir(dd);
        if (!n_dead) continue;
        dd = opendir(dpath);
        if (!dd) continue;
        while ((fe = readdir(dd)) != NULL) {
            for (int i = 0; i < n_dead; i++) {
                size_t l = strlen(dead[i]);
                /* "<id>_<ext>" belongs to this import; "<id>_<N>_..." is a
                 * collision-suffixed different session — don't touch it. */
                if (strncmp(fe->d_name, dead[i], l) == 0 &&
                    fe->d_name[l] == '_' &&
                    !(fe->d_name[l + 1] >= '0' && fe->d_name[l + 1] <= '9')) {
                    char p[600];
                    snprintf(p, sizeof(p), "%s/%s", dpath, fe->d_name);
                    unlink(p);
                    break;
                }
            }
        }
        closedir(dd);
    }
    closedir(days);
}

/* ── Session conversion + commit ────────────────────────────────────── */

typedef struct {
    const imp_cluster_t *cl;
    const imp_drift_t   *drift;
    const cJSON         *ident;
    const cJSON         *settings;
    const char          *import_src;   /* "file_import" | "somnobridge" */
    const char          *src_family;   /* "AS10" | "AS11" | "" */
    const char          *src_serial;
    char                 day[9];       /* streams noon-day label */
    char                 id[40];
    /* Bitmask over the cluster's source slots {brp,pld,sa2,eve,csl}
     * marking which sources the commit/augment path consumed (merged or
     * verified duplicate).  Sources whose bit is clear must NOT be
     * deleted — their coverage could not be verified. */
    uint8_t              src_consumed;
    uint32_t             n_flow, n_press, n_mm, n_sa2, n_pld, n_events;
} imp_work_t;

/* Duplicate/collision resolution: returns a free session id in out_id, or
 * ESP_ERR_INVALID_STATE when the session at this start time already exists
 * (out_id then receives the EXISTING id so the caller can augment it in
 * place instead of discarding a late-recovered file as a duplicate). */
static esp_err_t pick_session_id(const char *dir, int64_t start_ntp,
                                 char *out_id, size_t out_len)
{
    char base[20];
    as11_time_format_session_prefix(start_ntp, base, sizeof(base));
    for (int sfx = 0; sfx < 100; sfx++) {
        char id[40];
        if (sfx == 0) snprintf(id, sizeof(id), "%s", base);
        else          snprintf(id, sizeof(id), "%s_%d", base, sfx + 1);
        char mp[448];
        snprintf(mp, sizeof(mp), "%s/%s_session.json", dir, id);
        struct stat st;
        if (stat(mp, &st) != 0) {
            snprintf(out_id, out_len, "%s", id);
            return ESP_OK;
        }
        /* Existing manifest → duplicate check by start_epoch_ms */
        cJSON *mj = read_json_file(mp);
        int64_t stt = 0;
        if (mj) {
            json_i64(mj, "start_epoch_ms", &stt);
            cJSON_Delete(mj);
        }
        if (stt && llabs(stt - start_ntp) <= (int64_t)IMP_SESS_DUP_MS) {
            snprintf(out_id, out_len, "%s", id);
            return ESP_ERR_INVALID_STATE;
        }
        /* different session sharing the same second → next suffix */
    }
    return ESP_ERR_INVALID_SIZE;
}

/* ── Drift sidecar placement ────────────────────────────────────────────
 * <id>_session.json is read by pre-existing consumers that silently SKIP a
 * manifest above a fixed size: session_graph.c and the net_provision.c
 * "recreate" path at 4096 B (a skipped session vanishes from the web graph
 * and from recreate), time_sync.c at 8192 B, edf_gen's day rebuild and
 * session_writer_recover() at 16384 B (the latter then treats the session
 * as interrupted).  A SomnoBridge GET_DRIFT record can be up to 8 KiB, so
 * the sidecar is embedded verbatim only while it is small.  Larger ones put
 * a compact scalar projection in the manifest and keep the verbatim record
 * next to it as <id>_bridge_drift.json. */
#define IMP_DRIFT_EMBED_MAX   2048        /* embed verbatim up to this many bytes */
#define IMP_DRIFT_FILE_MAX    (64 * 1024) /* relocated verbatim copy cap */
#define IMP_MANIFEST_MAX      3584        /* hard cap, under the 4096 B reader limit */

static bool imp_json_scalar(const cJSON *j)
{
    return cJSON_IsNumber(j) || cJSON_IsString(j) || cJSON_IsBool(j) ||
           cJSON_IsNull(j);
}

/* Scalar members of the sidecar and of each tier record (one level down);
 * arrays and deeper objects are dropped. */
static cJSON *drift_compact(const cJSON *raw)
{
    cJSON *out = cJSON_CreateObject();
    if (!out) return NULL;
    for (const cJSON *m = raw->child; m; m = m->next) {
        if (!m->string) continue;
        if (imp_json_scalar(m)) {
            cJSON_AddItemToObject(out, m->string, cJSON_Duplicate(m, false));
        } else if (cJSON_IsObject(m)) {
            cJSON *o = cJSON_AddObjectToObject(out, m->string);
            if (!o) continue;
            for (const cJSON *c = m->child; c; c = c->next)
                if (c->string && imp_json_scalar(c))
                    cJSON_AddItemToObject(o, c->string, cJSON_Duplicate(c, false));
        }
    }
    return out;
}

/* Stage the verbatim sidecar as <id>_bridge_drift.json.part; publish_parts()
 * renames it with the rest of the session, residue cleanup removes it on
 * failure. */
static esp_err_t stage_drift_copy(const char *dir, const char *id, const char *txt)
{
    char p[448];
    snprintf(p, sizeof(p), "%s/%s_bridge_drift.json.part", dir, id);
    FILE *f = fopen(p, "w");
    if (!f) return ESP_FAIL;
    if (fputs(txt, f) < 0 || fputc('\n', f) != '\n') {
        fclose(f);
        unlink(p);
        return ESP_FAIL;
    }
    esp_err_t r = imp_flush_close(f, p);
    if (r != ESP_OK) unlink(p);
    return r;
}

/* Add the sidecar to the manifest: verbatim when small, else the compact
 * projection (+ pointer to the relocated copy when one was staged). */
static void manifest_add_drift(cJSON *r, const imp_drift_t *d, bool big,
                               const char *id, bool filed)
{
    if (!d->raw) return;
    if (!big) {
        cJSON_AddItemToObject(r, "somnobridge_drift", cJSON_Duplicate(d->raw, true));
        return;
    }
    cJSON *c = drift_compact(d->raw);
    if (c) cJSON_AddItemToObject(r, "somnobridge_drift", c);
    cJSON_AddBoolToObject(r, "somnobridge_drift_truncated", true);
    if (filed) {
        char nm[64];
        snprintf(nm, sizeof(nm), "%s_bridge_drift.json", id);
        cJSON_AddStringToObject(r, "somnobridge_drift_file", nm);
    }
}

/* Last line of defence: even a compact projection can blow the budget (e.g.
 * a huge scalar string) — drop the embed rather than the session. */
static void manifest_enforce_cap(cJSON *r)
{
    char *txt = cJSON_PrintUnformatted(r);
    bool over = txt && strlen(txt) + 1 > IMP_MANIFEST_MAX;
    free(txt);
    if (!over || !cJSON_GetObjectItem(r, "somnobridge_drift")) return;
    ESP_LOGW(TAG, "import: manifest over %d B — dropping embedded drift record",
             IMP_MANIFEST_MAX);
    cJSON_DeleteItemFromObject(r, "somnobridge_drift");
    if (!cJSON_GetObjectItem(r, "somnobridge_drift_truncated"))
        cJSON_AddBoolToObject(r, "somnobridge_drift_truncated", true);
}

static uint32_t manifest_u32(const cJSON *m, const char *key)
{
    const cJSON *j = cJSON_GetObjectItem((cJSON *)m, key);
    return (cJSON_IsNumber(j) && j->valuedouble > 0)
               ? (uint32_t)j->valuedouble : 0;
}

static bool manifest_has_kind(const cJSON *kinds, const char *k)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, kinds)
        if (cJSON_IsString(it) && it->valuestring &&
            !strcmp(it->valuestring, k))
            return true;
    return false;
}

static void manifest_add_kind(cJSON *mj, const char *k)
{
    cJSON *kinds = cJSON_GetObjectItem(mj, "src_kinds");
    if (!cJSON_IsArray(kinds)) kinds = cJSON_AddArrayToObject(mj, "src_kinds");
    if (!kinds || manifest_has_kind(kinds, k)) return;
    cJSON_AddItemToArray(kinds, cJSON_CreateString(k));
}

static void manifest_set_num(cJSON *mj, const char *key, double v)
{
    cJSON *j = cJSON_GetObjectItem(mj, key);
    if (cJSON_IsNumber(j)) cJSON_SetNumberValue(j, v);
    else cJSON_AddNumberToObject(mj, key, v);
}

/* Merge newly recovered stream kinds into an already-committed session.
 * A file that failed to transfer during the original sync (or arrived
 * later than its siblings) completes the session in place here instead
 * of being deleted as a duplicate — the hazard that silently discarded
 * recovered BRPs.  Coverage is proven by the manifest's sample counts
 * (wave kinds) and src_kinds list (annotation kinds): a kind already
 * recorded is a duplicate; a kind that cannot be verified is left in
 * place, never deleted on a guess.
 *
 * Event merging regenerates <id>_events.snt from THIS cluster, so it is
 * only done when every already-recorded annotation kind is re-emittable
 * from the cluster — otherwise recorded events would be lost.
 *
 * Returns ESP_OK when every offered kind was merged or confirmed
 * duplicate, ESP_ERR_INVALID_STATE when nothing new was offered (the
 * all-duplicate fast path), ESP_ERR_NOT_SUPPORTED when unverifiable
 * sources had to be left (w->src_consumed flags the deletable bits), or
 * ESP_FAIL on conversion/write failure. */
static esp_err_t augment_session(const char *dir, const char *id,
                                 imp_work_t *w, const imp_cluster_t *cl,
                                 int64_t wav_ntp, int64_t wav_dev,
                                 int64_t end_dev, int64_t end_ntp)
{
    char mp[448];
    snprintf(mp, sizeof(mp), "%s/%s_session.json", dir, id);
    cJSON *mj = read_json_file(mp);
    if (!mj) {
        ESP_LOGW(TAG, "import: %s session manifest unreadable — sources kept",
                 id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const uint32_t m_flow = manifest_u32(mj, "brp_samples");
    const uint32_t m_sa2  = manifest_u32(mj, "sa2_samples");
    const uint32_t m_pld  = manifest_u32(mj, "pld_samples");
    const cJSON *kinds = cJSON_GetObjectItem(mj, "src_kinds");
    const bool kinds_known = cJSON_IsArray(kinds);
    const bool rec_eve = kinds_known && manifest_has_kind(kinds, "eve");
    const bool rec_csl = kinds_known && manifest_has_kind(kinds, "csl");

    esp_err_t ret = ESP_OK;
    uint32_t nf = 0, np = 0, nm = 0, ns2 = 0, npl = 0, nev = 0;
    bool m_brp = false, m_sa2m = false, m_pldm = false, m_ev = false;
    /* Consumption decision (bits: 0 brp, 1 pld, 2 sa2, 3 eve, 4 csl) —
     * declared up top so the goto-out path never sees them uninitialised. */
    uint8_t consumed = 0;
    const uint8_t offered = (uint8_t)((cl->brp ? 0x01 : 0) |
                                     (cl->pld ? 0x02 : 0) |
                                     (cl->sa2 ? 0x04 : 0) |
                                     (cl->eve ? 0x08 : 0) |
                                     (cl->csl ? 0x10 : 0));
    bool merged = false;

    /* Wave kinds merge when the manifest records no samples for them —
     * either the file arrived late, or the original commit found no data
     * (an empty stream converts as "absent" and stays a duplicate). */
    if (cl->brp && m_flow == 0) {
        imp_edf_t *e = imp_malloc(sizeof(imp_edf_t));
        esp_err_t r = e ? imp_edf_open(e, cl->brp->path) : ESP_ERR_NO_MEM;
        if (r == ESP_OK) {
            r = conv_brp(e, dir, id, wav_ntp, &nf, &np, &nm);
            if (r == ESP_ERR_NOT_FOUND) r = ESP_OK;
        }
        free(e);
        if (r != ESP_OK) { ret = ESP_FAIL; goto out; }
        m_brp = true;
        merged = true;
        ESP_LOGI(TAG, "import: %s augmented with BRP "
                 "(flow=%lu press=%lu mm=%lu)", id, (unsigned long)nf,
                 (unsigned long)np, (unsigned long)nm);
    }
    if (cl->sa2 && m_sa2 == 0) {
        imp_edf_t *e = imp_malloc(sizeof(imp_edf_t));
        esp_err_t r = e ? imp_edf_open(e, cl->sa2->path) : ESP_ERR_NO_MEM;
        if (r == ESP_OK) {
            r = conv_sa2(e, dir, id, wav_ntp, &ns2);
            if (r == ESP_ERR_NOT_FOUND) r = ESP_OK;
        }
        free(e);
        if (r != ESP_OK) { ret = ESP_FAIL; goto out; }
        m_sa2m = true;
        merged = true;
        ESP_LOGI(TAG, "import: %s augmented with SA2 (%lu)", id,
                 (unsigned long)ns2);
    }
    if (cl->pld && m_pld == 0) {
        imp_edf_t *e = imp_malloc(sizeof(imp_edf_t));
        esp_err_t r = e ? imp_edf_open(e, cl->pld->path) : ESP_ERR_NO_MEM;
        if (r == ESP_OK) {
            r = conv_pld(e, dir, id, wav_ntp, &npl);
            if (r == ESP_ERR_NOT_FOUND) r = ESP_OK;
        }
        free(e);
        if (r != ESP_OK) { ret = ESP_FAIL; goto out; }
        m_pldm = true;
        merged = true;
        ESP_LOGI(TAG, "import: %s augmented with PLD (%lu)", id,
                 (unsigned long)npl);
    }

    /* Annotation kinds: regenerate the whole events file, so it is only
     * safe when every recorded ann kind is re-emittable from this
     * cluster.  Without src_kinds (legacy manifests) nothing about the
     * stored events can be read back from metadata — instead re-render
     * the offered sources and require every recorded event to be
     * covered; the merge then can only grow the events file. */
    const bool missing_ann = (cl->eve && !rec_eve) || (cl->csl && !rec_csl);
    const bool regenerable = (!rec_eve || cl->eve) && (!rec_csl || cl->csl);
    if (kinds_known && missing_ann && regenerable && (cl->eve || cl->csl)) {
        if (write_events_snt(dir, id, cl->eve, cl->csl, cl->eve_ts,
                             wav_dev, end_dev, &nev) != ESP_OK) {
            ret = ESP_FAIL;
            goto out;
        }
        m_ev = true;
        merged = true;
        ESP_LOGI(TAG, "import: %s events regenerated (+%lu events)", id,
                 (unsigned long)nev);
    } else if (!kinds_known && (cl->eve || cl->csl)) {
        imp_line_set_t cand = {0}, kept = {0};
        if (collect_ann_lines(cl->eve, cl->csl, &cand, &nev) != ESP_OK) {
            line_set_free(&cand);
            ret = ESP_FAIL;
            goto out;
        }
        if (events_file_covered(dir, id, &cand, &kept)) {
            char path[400], tmp[408];
            snprintf(path, sizeof(path), "%s/%s_events.snt", dir, id);
            snprintf(tmp, sizeof(tmp), "%s.part", path);
            FILE *ef = fopen(tmp, "w");
            bool wr = ef != NULL;
            for (int i = 0; wr && i < kept.n; i++)
                wr = fputs(kept.v[i], ef) >= 0;
            for (int i = 0; wr && i < cand.n; i++)
                wr = fputs(cand.v[i], ef) >= 0;
            if (wr)
                wr = imp_flush_close(ef, tmp) == ESP_OK;
            else if (ef)
                fclose(ef);
            if (!wr) {
                unlink(tmp);
                line_set_free(&cand);
                line_set_free(&kept);
                ret = ESP_FAIL;
                goto out;
            }
            m_ev = true;
            merged = true;
            ESP_LOGI(TAG, "import: %s events merged by coverage "
                     "(+%lu rendered)", id, (unsigned long)nev);
        } else {
            ESP_LOGI(TAG, "import: %s annotation sources not verifiable "
                     "against recorded events — kept", id);
        }
        line_set_free(&cand);
        line_set_free(&kept);
    }

    /* Publish + manifest update only when something actually merged. */
    if (merged) {
        if (publish_parts(dir, id) != ESP_OK) { ret = ESP_FAIL; goto out; }
        if (m_brp) {
            manifest_set_num(mj, "brp_samples", (double)nf);
            manifest_set_num(mj, "brp_mm_samples", (double)nm);
            manifest_set_num(mj, "press_samples", (double)np);
            manifest_add_kind(mj, "brp");
        }
        if (m_sa2m) {
            manifest_set_num(mj, "sa2_samples", (double)ns2);
            manifest_add_kind(mj, "sa2");
        }
        if (m_pldm) {
            manifest_set_num(mj, "pld_samples", (double)npl);
            manifest_add_kind(mj, "pld");
        }
        if (m_ev) {
            if (cl->eve) manifest_add_kind(mj, "eve");
            if (cl->csl) manifest_add_kind(mj, "csl");
        }
        /* A merged wave file can extend the session's true end beyond
         * the annotation-only estimate it was committed with. */
        int64_t m_end = 0;
        json_i64(mj, "end_epoch_ms", &m_end);
        if (end_ntp > m_end) {
            manifest_set_num(mj, "end_epoch_ms", (double)end_ntp);
            char iso[32];
            imp_epoch_to_local_iso(end_ntp, iso, sizeof(iso));
            cJSON_ReplaceItemInObject(mj, "end_iso",
                                    cJSON_CreateString(iso));
        }
        /* A repeated augment replaces the marker rather than adding a
         * duplicate key. */
        if (cJSON_GetObjectItem(mj, "augmented"))
            cJSON_ReplaceItemInObject(mj, "augmented", cJSON_CreateBool(true));
        else
            cJSON_AddBoolToObject(mj, "augmented", true);
        manifest_set_num(mj, "augmented_at_ms", (double)time(NULL) * 1000);
        if (write_json_atomic(mp, mj) != ESP_OK) { ret = ESP_FAIL; goto out; }
    }

    /* A kind is consumable when the manifest proves coverage (samples or
     * src_kinds) or when it was just merged — anything unverifiable keeps
     * its bit clear and is left in place by the caller. */
    if (cl->brp && (m_flow > 0 || m_brp))   consumed |= 0x01;
    if (cl->pld && (m_pld  > 0 || m_pldm))  consumed |= 0x02;
    if (cl->sa2 && (m_sa2  > 0 || m_sa2m))  consumed |= 0x04;
    if (cl->eve && (rec_eve || m_ev))       consumed |= 0x08;
    if (cl->csl && (rec_csl || m_ev))       consumed |= 0x10;
    w->src_consumed = consumed;

out:
    cJSON_Delete(mj);
    if (ret != ESP_OK) return ret;
    const bool all_done = ((consumed & offered) == offered);
    if (!merged) return all_done ? ESP_ERR_INVALID_STATE
                                : ESP_ERR_NOT_SUPPORTED;
    ESP_LOGI(TAG, "import: %s session augmented in place", id);
    return all_done ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t commit_session(imp_work_t *w, const imp_cluster_t *cl,
                                const imp_drift_t *drift)
{
    /* Session anchors (device domain).  The applied shift is the measured
     * drift plus, when the sidecar confirms a fixed zone for the source
     * device, the delta to the ESP's own zone — evaluated per anchor so
     * a session straddling an ESP DST boundary still lands exactly. */
    int64_t anchor_dev = (cl->eve_ts >= 0) ? cl->eve_ts : cl->wav_ts;
    int64_t wav_off_ms = (cl->eve_ts >= 0 && cl->wav_ts >= 0)
                         ? cl->wav_ts - cl->eve_ts : 0;
    int64_t wav_dev = (cl->wav_ts >= 0) ? cl->wav_ts : anchor_dev;
    const int64_t shift = imp_shift_at(drift, anchor_dev);
    int64_t start_ntp = anchor_dev + shift;
    int64_t wav_ntp   = wav_dev + imp_shift_at(drift, wav_dev);
    int64_t end_dev   = anchor_dev;

    /* Longest member file span ends the session */
    const imp_file_t *memb[] = { cl->brp, cl->pld, cl->sa2, cl->eve, cl->csl };
    const bool ann_only = !cl->brp && !cl->pld && !cl->sa2;
    for (int i = 0; i < 5; i++) {
        if (!memb[i]) continue;
        imp_edf_t *e = imp_malloc(sizeof(imp_edf_t));
        if (e && imp_edf_open(e, memb[i]->path) == ESP_OK) {
            int64_t span = (int64_t)(e->rdur_s * 1000.0) * e->n_records;
            /* Annotation files declare 0 s records, so a mask-less session's
             * span is its last annotation's end instead.  With waveforms the
             * waveform files alone define the end. */
            if (ann_only && e->is_ann) {
                int64_t a = imp_ann_span_ms(memb[i]->path, i == 4);
                if (a > span) span = a;
            }
            int64_t fend = e->ts_dev_ms >= 0 ? e->ts_dev_ms + span
                                             : memb[i]->ts_dev_ms;
            if (fend > end_dev) end_dev = fend;
        }
        free(e);
    }
    /* A mask-less session still needs end > start: the day spool and
     * SessionModeEntries ignore zero-length windows, and the AS11 itself
     * records MaskOff = MaskOn for 0-duration sessions. */
    if (ann_only && end_dev <= anchor_dev)
        end_dev = anchor_dev + 1000;
    int64_t end_ntp = end_dev + imp_shift_at(drift, end_dev);

    /* Noon-day stream folder — raw device time, never drifted.  Create the
     * full parent chain defensively: a fresh card whose first content is an
     * IMPORT/ drop may not have the app tree yet. */
    as11_time_noon_day(anchor_dev, w->day, sizeof(w->day));
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/%s", SD_STREAMS_DIR, w->day);
    mkdir(SD_APP_DIR, 0775);
    mkdir(SD_SESSIONS_DIR, 0775);
    mkdir(SD_STREAMS_DIR, 0775);
    mkdir(dir, 0775);

    esp_err_t sel = pick_session_id(dir, start_ntp, w->id, sizeof(w->id));
    if (sel == ESP_ERR_INVALID_STATE) {
        /* The session already exists — merge whatever this cluster can
         * still offer instead of deleting a late-recovered file as a
         * duplicate (augment returns INVALID_STATE for a true dup). */
        return augment_session(dir, w->id, w, cl, wav_ntp, wav_dev,
                               end_dev, end_ntp);
    }
    if (sel != ESP_OK) return sel;

    ESP_LOGI(TAG, "import: session %s (day %s, wav+%lld ms, %s)",
             w->id, w->day, (long long)wav_off_ms, drift->source);

    if (write_import_marker(dir, w->id) != ESP_OK) return ESP_FAIL;
    esp_err_t ret = ESP_FAIL;
    uint32_t n_flow = 0, n_press = 0, n_mm = 0, n_sa2 = 0, n_pld = 0, n_ev = 0;
    bool drift_big = false, drift_filed = false;

    /* ── Waveforms → staged .part ──
     * A source file that opens but carries none of its expected signals
     * (NOT_FOUND) degrades to the "absent" equivalent instead of failing
     * the session. */
    bool have_brp = false, have_sa2 = false, have_pld = false;
    if (cl->brp) {
        imp_edf_t *e = imp_malloc(sizeof(imp_edf_t));
        esp_err_t r = e ? imp_edf_open(e, cl->brp->path) : ESP_ERR_NO_MEM;
        if (r == ESP_OK) {
            r = conv_brp(e, dir, w->id, wav_ntp, &n_flow, &n_press, &n_mm);
            if (r == ESP_ERR_NOT_FOUND) r = ESP_OK;   /* no Flow → absent */
        }
        free(e);
        if (r != ESP_OK) goto out;
        have_brp = (n_flow > 0);
    }
    if (cl->sa2) {
        imp_edf_t *e = imp_malloc(sizeof(imp_edf_t));
        esp_err_t r = e ? imp_edf_open(e, cl->sa2->path) : ESP_ERR_NO_MEM;
        if (r == ESP_OK) {
            r = conv_sa2(e, dir, w->id, wav_ntp, &n_sa2);
            if (r == ESP_ERR_NOT_FOUND) r = ESP_OK;
        }
        free(e);
        if (r != ESP_OK) goto out;
        have_sa2 = (n_sa2 > 0);
    }
    if (cl->pld) {
        imp_edf_t *e = imp_malloc(sizeof(imp_edf_t));
        esp_err_t r = e ? imp_edf_open(e, cl->pld->path) : ESP_ERR_NO_MEM;
        if (r == ESP_OK) {
            r = conv_pld(e, dir, w->id, wav_ntp, &n_pld);
            if (r == ESP_ERR_NOT_FOUND) r = ESP_OK;
        }
        free(e);
        if (r != ESP_OK) goto out;
        have_pld = (n_pld > 0);
    }
    /* No oximeter stream on the source card → write an all-missing sa2.snt
     * for the session duration (spec §3.3) so SomnoStage sees explicit
     * absent data instead of a missing file. */
    if (cl->brp && !have_sa2) {
        int64_t wav_secs = (end_dev - wav_dev) / 1000;
        if (wav_secs < 0) wav_secs = 0;
        if (wav_secs > 24 * 3600) wav_secs = 24 * 3600;
        if (write_missing_sa2(dir, w->id, wav_ntp, (uint32_t)wav_secs,
                              &n_sa2) != ESP_OK) goto out;
    }
    /* Empty streams for kinds the source didn't carry → header-only EDFs
     * on re-export (matches sub-60 s session behaviour).  Staged as .part
     * like everything else; publish_parts() renames them.  Each stub is
     * gated on its own stream being absent — writing _sa2.snt here when a
     * real sa2.part was just staged would clobber it. */
    if (!have_brp) {
        struct { const char *sfx; uint8_t ch; uint16_t hz; uint8_t tier; } T[] = {
            { "_flow.snt",    1, 250, SNT_TIER_RAW },
            { "_press.snt",   1, 250, SNT_TIER_RAW },
            { "_flow_mm.snt", 2, 10,  SNT_TIER_MINMAX },
        };
        for (int i = 0; i < 3; i++) {
            char p[400];
            snt_w_t t;
            snprintf(p, sizeof(p), "%s/%s%s", dir, w->id, T[i].sfx);
            if (snt_w_open(&t, p, T[i].ch, T[i].hz, T[i].tier, wav_ntp) == ESP_OK)
                snt_w_flush(&t);
        }
    }
    if (n_sa2 == 0) {
        char p[400];
        snt_w_t t;
        snprintf(p, sizeof(p), "%s/%s_sa2.snt", dir, w->id);
        if (snt_w_open(&t, p, 2, 10, SNT_TIER_RAW, wav_ntp) == ESP_OK)
            snt_w_flush(&t);
    }
    if (!have_pld) {
        char p[400];
        snt_w_t t;
        snprintf(p, sizeof(p), "%s/%s_pld.snt", dir, w->id);
        if (snt_w_open(&t, p, 12, 5, SNT_TIER_RAW, wav_ntp) == ESP_OK)
            snt_w_flush(&t);
    }

    /* ── Events → staged .part ── */
    if (write_events_snt(dir, w->id, cl->eve, cl->csl,
                         cl->eve_ts, wav_dev, end_dev, &n_ev) != ESP_OK)
        goto out;

    /* ── Drift sidecar: embed verbatim, or project + relocate (see above) ── */
    if (drift->raw) {
        char *dtxt = cJSON_PrintUnformatted(drift->raw);
        if (dtxt) {
            size_t dl = strlen(dtxt);
            drift_big = dl > IMP_DRIFT_EMBED_MAX;
            if (drift_big && dl <= IMP_DRIFT_FILE_MAX)
                drift_filed = (stage_drift_copy(dir, w->id, dtxt) == ESP_OK);
            free(dtxt);
        }
    }

    /* ── Publish all staged parts, then the manifest (commit point) ── */
    if (publish_parts(dir, w->id) != ESP_OK) goto out;
    {
        cJSON *r = cJSON_CreateObject();
        char iso[32];
        imp_epoch_to_local_iso(start_ntp, iso, sizeof(iso));
        cJSON_AddStringToObject(r, "id", w->id);
        cJSON_AddNumberToObject(r, "start_epoch_ms", (double)start_ntp);
        cJSON_AddNumberToObject(r, "end_epoch_ms", (double)end_ntp);
        cJSON_AddStringToObject(r, "start_iso", iso);
        imp_epoch_to_local_iso(end_ntp, iso, sizeof(iso));
        cJSON_AddStringToObject(r, "end_iso", iso);
        cJSON_AddStringToObject(r, "state", "completed");
        cJSON_AddNumberToObject(r, "fmt", 2);
        cJSON_AddBoolToObject(r, "pre_gated", true);
        cJSON_AddNumberToObject(r, "waveform_offset_ms", (double)wav_off_ms);
        cJSON_AddNumberToObject(r, "brp_samples", (double)n_flow);
        cJSON_AddNumberToObject(r, "brp_mm_samples", (double)n_mm);
        cJSON_AddNumberToObject(r, "press_samples", (double)n_press);
        cJSON_AddNumberToObject(r, "sa2_samples", (double)n_sa2);
        cJSON_AddNumberToObject(r, "pld_samples", (double)n_pld);
        /* Which source kinds were offered at commit: the augment path
         * relies on it to merge a late-recovered file without ever
         * discarding verifiable content. */
        cJSON *kinds = cJSON_AddArrayToObject(r, "src_kinds");
        if (kinds) {
            if (cl->brp) cJSON_AddItemToArray(kinds, cJSON_CreateString("brp"));
            if (cl->pld) cJSON_AddItemToArray(kinds, cJSON_CreateString("pld"));
            if (cl->sa2) cJSON_AddItemToArray(kinds, cJSON_CreateString("sa2"));
            if (cl->eve) cJSON_AddItemToArray(kinds, cJSON_CreateString("eve"));
            if (cl->csl) cJSON_AddItemToArray(kinds, cJSON_CreateString("csl"));
        }
        cJSON_AddNumberToObject(r, "stream_notifications", 0);
        cJSON_AddNumberToObject(r, "gap_events", 0);
        cJSON_AddNumberToObject(r, "gap_missing", 0);
        /* The applied session shift: measured drift + device-zone delta
         * at the session anchor.  Downstream readers reconstruct device
         * time as start_epoch_ms - clock_drift_ms, so the total applied
         * shift is what must be recorded here; the decomposition stays
         * visible in device_tz_* and somnobridge_drift. */
        cJSON_AddNumberToObject(r, "clock_drift_ms", (double)shift);
        cJSON_AddBoolToObject(r, "clock_drift_valid", drift->valid);
        cJSON_AddBoolToObject(r, "clock_drift_usable", drift->usable);
        cJSON_AddStringToObject(r, "clock_drift_source", drift->source);
        if (drift->measured_at_ms > 0)
            cJSON_AddNumberToObject(r, "clock_drift_measured_at_ms",
                                    (double)drift->measured_at_ms);
        if (drift->half_ms >= 0)
            cJSON_AddNumberToObject(r, "clock_drift_half_ms",
                                    (double)drift->half_ms);
        if (drift->dev_tz_off_s != INT32_MIN) {
            cJSON_AddNumberToObject(r, "device_tz_offset_s",
                                    (double)drift->dev_tz_off_s);
            cJSON_AddNumberToObject(r, "device_tz_delta_ms",
                                    (double)(shift - drift->drift_ms));
            if (drift->dev_tz_rule[0])
                cJSON_AddStringToObject(r, "device_tz_rule",
                                        drift->dev_tz_rule);
        }
        cJSON_AddStringToObject(r, "time_source",
                                drift->valid ? "ntp" : "file_import");
        cJSON_AddStringToObject(r, "import_source", w->import_src);
        cJSON_AddNumberToObject(r, "imported_at_ms", (double)time(NULL) * 1000);
        if (w->src_family && w->src_family[0])
            cJSON_AddStringToObject(r, "source_device_family", w->src_family);
        if (w->src_serial && w->src_serial[0])
            cJSON_AddStringToObject(r, "source_serial", w->src_serial);
        manifest_add_drift(r, drift, drift_big, w->id, drift_filed);
        manifest_enforce_cap(r);
        char mp[448];
        snprintf(mp, sizeof(mp), "%s/%s_session.json", dir, w->id);
        esp_err_t wr = write_json_atomic(mp, r);
        cJSON_Delete(r);
        if (wr != ESP_OK) goto out;
    }
    delete_import_marker(dir, w->id);

    /* Auxiliary sidecars — written after the commit point (they are
     * recoverable through the existing ident/settings fallback chain). */
    if (w->ident) {
        char p[448];
        snprintf(p, sizeof(p), "%s/%s_ident.json", dir, w->id);
        write_json_atomic(p, w->ident);
    }
    if (w->settings) {
        char p[448];
        snprintf(p, sizeof(p), "%s/%s_settings.json", dir, w->id);
        write_json_atomic(p, w->settings);
    }

    w->n_flow = n_flow; w->n_press = n_press; w->n_mm = n_mm;
    w->n_sa2 = n_sa2; w->n_pld = n_pld; w->n_events = n_ev;
    ret = ESP_OK;
out:
    if (ret != ESP_OK) {
        /* leave the marker — next scan's residue cleanup removes all
         * <id>* files (manifest absent ⇒ incomplete) */
        ESP_LOGE(TAG, "import: session %s failed", w->id);
    }
    return ret;
}

/* ── Group discovery ────────────────────────────────────────────────── */

#define IMP_IMPORT_DIR   SD_IMPORT_DIR
#define IMP_DATALOG_DIR  SD_IMPORT_DIR "/DATALOG"
#define IMP_MAX_DAYS     64

typedef struct {
    char        src_dir[300];       /* import-side dir holding the files */
    char        key[9];             /* sort key — always an 8-char day label */
    bool        is_flat;            /* files lived directly under IMPORT/ */
    bool        has_part;           /* .part/.tmp present → still transferring */
    char        drift_path[320];    /* _bridge_drift.json, "" when absent */
    int         n_files;
    imp_file_t  files[IMP_MAX_FILES];
} imp_group_t;

static bool name_is_dir8(const char *n)
{
    if (strlen(n) != 8) return false;
    for (int i = 0; i < 8; i++)
        if (n[i] < '0' || n[i] > '9') return false;
    return true;
}

/* Scan one directory for session EDFs + a drift sidecar. */
static void scan_group_dir(const char *dir, imp_group_t *g)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        size_t nl = strlen(nm);
        if (nl > 5 &&
            (!strcasecmp(nm + nl - 5, ".part") ||
             !strcasecmp(nm + nl - 5, ".tmp"))) {
            g->has_part = true;
            continue;
        }
        if (!strcmp(nm, IMP_DRIFT_FILE)) {
            snprintf(g->drift_path, sizeof(g->drift_path), "%s/%s", dir,
                     IMP_DRIFT_FILE);
            continue;
        }
        imp_file_t f;
        if (g->n_files < IMP_MAX_FILES && imp_classify(dir, nm, &f))
            g->files[g->n_files++] = f;
    }
    closedir(d);
}

/* Enumerate pending groups; chooses the chronologically earliest viable
 * one (in-flight .part/.tmp transfers defer their group to a later call).
 * Sources: IMPORT/DATALOG/<day>/, IMPORT/<day>/, and flat IMPORT .edf
 * files (grouped by their own noon-day label).  Root files are handled by
 * the root pass, not here.  Returns true when *g was filled. */
static bool next_group(imp_group_t *g)
{
    memset(g, 0, sizeof(*g));
    bool have = false;

    /* ~65 KiB scratch group — heap (PSRAM on target), never the 14 KiB
     * worker stack. */
    imp_group_t *t = imp_calloc(1, sizeof(*t));
    if (!t) return false;

    /* Day-dir candidates: IMPORT/DATALOG/<8dig>/ and IMPORT/<8dig>/. */
    const char *bases[] = { IMP_DATALOG_DIR, IMP_IMPORT_DIR };
    for (int b = 0; b < 2; b++) {
        DIR *d = opendir(bases[b]);
        if (!d) continue;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (!name_is_dir8(ent->d_name)) continue;
            memset(t, 0, sizeof(*t));
            snprintf(t->src_dir, sizeof(t->src_dir), "%s/%.*s",
                     bases[b], 8, ent->d_name);
            struct stat st;
            if (stat(t->src_dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            memcpy(t->key, ent->d_name, 9);   /* name_is_dir8 ⇒ exactly 8 + NUL */
            scan_group_dir(t->src_dir, t);
            if (t->has_part) continue;          /* transfer in flight */
            if (t->n_files == 0 && !t->drift_path[0]) {
                /* Nothing importable in here: an already-drained husk, or a
                 * folder holding only leftovers (AS10 *.crc before the
                 * sidecar fix, quarantined *.bad, a user's own files).
                 * Remove it when it is empty and SKIP it — a candidate we
                 * cannot consume must never abort the scan, or it shadows
                 * every later day forever (issue #338). */
                if (!t->is_flat && rmdir(t->src_dir) != 0)
                    ESP_LOGI(TAG, "import: %s holds no session EDFs — skipped",
                             t->src_dir);
                continue;
            }
            if (!have || strcmp(t->key, g->key) < 0) {
                memcpy(g, t, sizeof(*g));
                have = true;
            }
        }
        closedir(d);
    }
    free(t);

    /* Flat IMPORT .edf — one group per noon-day label.  A stray .part at
     * the import root means a copy is in flight → all flat groups defer. */
    {
        DIR *d = opendir(IMP_IMPORT_DIR);
        if (d) {
            bool blocked = false;
            char flat_key[9] = "";
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                const char *nm = ent->d_name;
                size_t nl = strlen(nm);
                if (nl > 5 &&
                    (!strcasecmp(nm + nl - 5, ".part") ||
                     !strcasecmp(nm + nl - 5, ".tmp")))
                    blocked = true;
                imp_file_t f;
                if (!imp_classify(IMP_IMPORT_DIR, nm, &f)) continue;
                char nd[9];
                as11_time_noon_day(f.ts_dev_ms, nd, sizeof(nd));
                if (!flat_key[0] || strcmp(nd, flat_key) < 0)
                    memcpy(flat_key, nd, sizeof(flat_key));   /* nd is 8 + NUL */
            }
            if (flat_key[0] && !blocked &&
                (!have || strcmp(flat_key, g->key) < 0)) {
                /* The flat noon-day group wins only when strictly earlier
                 * than the best dir group — clear *g first so a previously
                 * selected dir group's files don't leak into it. */
                memset(g, 0, sizeof(*g));
                have = true;
                g->is_flat = true;
                memcpy(g->key, flat_key, sizeof(g->key));
                snprintf(g->src_dir, sizeof(g->src_dir), "%s",
                         IMP_IMPORT_DIR);
                rewinddir(d);
                while ((ent = readdir(d)) != NULL) {
                    imp_file_t f;
                    if (!imp_classify(IMP_IMPORT_DIR, ent->d_name, &f))
                        continue;
                    char nd[9];
                    as11_time_noon_day(f.ts_dev_ms, nd, sizeof(nd));
                    if (!strcmp(nd, flat_key) &&
                        g->n_files < IMP_MAX_FILES)
                        g->files[g->n_files++] = f;
                }
                /* root-level drift sidecar, if present */
                char dp[320];
                snprintf(dp, sizeof(dp), "%s/%s",
                         IMP_IMPORT_DIR, IMP_DRIFT_FILE);
                struct stat st;
                if (stat(dp, &st) == 0)
                    snprintf(g->drift_path, sizeof(g->drift_path),
                             "%s", dp);
            }
            closedir(d);
        }
    }

    return have;
}

/* ── Root-files pass: ident + settings + STR → spools ───────────────── */

static bool root_files_pending(void)
{
    static const char *const paths[] = {
        IMP_IMPORT_DIR "/STR.edf",
        IMP_IMPORT_DIR "/Identification.json",
        IMP_IMPORT_DIR "/Identification.tgt",
        IMP_IMPORT_DIR "/SETTINGS/CurrentSettings.json",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        struct stat st;
        if (stat(paths[i], &st) == 0) return true;
    }
    return false;
}

/* Resolve identification: IMPORT/Identification.json →
 * IMPORT/Identification.tgt → NULL (session-level fallback uses the EDF
 * recording_id instead, resolved inside commit_session). */
static void resolve_ident(edf_import_ctx_t *ctx)
{
    cJSON *j = read_json_file(IMP_IMPORT_DIR "/Identification.json");
    if (j) {
        ctx->ident = ident_from_ident_json(j);
        cJSON_Delete(j);
        if (ctx->ident) {
            snprintf(ctx->src_family, sizeof(ctx->src_family), "AS11");
            const cJSON *s = cJSON_GetObjectItem(ctx->ident, "SerialNumber");
            if (s && cJSON_IsString(s))
                snprintf(ctx->src_serial, sizeof(ctx->src_serial),
                         "%s", s->valuestring);
            return;
        }
    }
    ctx->ident = ident_from_tgt(IMP_IMPORT_DIR "/Identification.tgt");
    if (ctx->ident) {
        snprintf(ctx->src_family, sizeof(ctx->src_family), "AS10");
        const cJSON *s = cJSON_GetObjectItem(ctx->ident, "SerialNumber");
        if (s && cJSON_IsString(s))
            snprintf(ctx->src_serial, sizeof(ctx->src_serial),
                     "%s", s->valuestring);
    }
}

/* SETTINGS/CurrentSettings.json — {"FlowGenerator":{"SettingProfiles":…}}
 * → keep the inner FlowGenerator contents (or the whole doc when already
 * flat). */
static void resolve_settings(edf_import_ctx_t *ctx)
{
    cJSON *j = read_json_file(IMP_IMPORT_DIR "/SETTINGS/CurrentSettings.json");
    if (!j) return;
    const cJSON *fg = cJSON_GetObjectItem(j, "FlowGenerator");
    ctx->settings = cJSON_Duplicate(fg ? fg : j, true);
    cJSON_Delete(j);
}

static esp_err_t process_root_pass(edf_import_ctx_t *ctx, edf_import_unit_t *u)
{
    u->root_pass = true;
    resolve_ident(ctx);
    resolve_settings(ctx);

    struct stat st;
    if (stat(IMP_IMPORT_DIR "/STR.edf", &st) == 0)
        import_str_edf(IMP_IMPORT_DIR "/STR.edf", ctx, u);

    /* Resolved ident/settings become the device_metadata.json snapshot
     * so sessions lacking sidecars still export correctly. */
    write_device_metadata(ctx);

    /* Consume the sources — after this point the resolved trees live in
     * ctx and per-session sidecars; EDF recording_id remains as a
     * fallback for days imported in later runs. */
    unlink(IMP_IMPORT_DIR "/STR.edf");
    unlink(IMP_IMPORT_DIR "/Identification.json");
    unlink(IMP_IMPORT_DIR "/Identification.tgt");
    unlink(IMP_IMPORT_DIR "/SETTINGS/CurrentSettings.json");
    unlink(IMP_IMPORT_DIR "/SETTINGS/CurrentSettings.crc");
    rmdir(IMP_IMPORT_DIR "/SETTINGS");   /* only succeeds when empty */
    return ESP_OK;
}

/* ── Synthesized spool for days with no STR coverage ────────────────── */

/* Percentile helper over a fixed-bin histogram. */
static int32_t hist_pct(const uint32_t *hist, int bins, int32_t lo,
                        int32_t step_milli, uint32_t total, int pct)
{
    if (!total) return -1;
    uint32_t want = (uint32_t)(((uint64_t)total * pct) / 100);
    if (!want) want = 1;
    uint32_t acc = 0;
    for (int i = 0; i < bins; i++) {
        acc += hist[i];
        if (acc >= want) return lo + (int32_t)((int64_t)i * step_milli / 1000);
    }
    return lo + (int32_t)((int64_t)(bins - 1) * step_milli / 1000);
}

/* Build a spool day record from the imported sessions of streams/<day>.
 * Fills SessionModeEntries from manifests and event indices by counting
 * *End events in each _events.snt; computes PLD/SpO2 percentiles with a
 * bounded two-pass histogram per channel. */
static esp_err_t synthesize_day_spool(const char *day, bool *out_wrote)
{
    if (out_wrote) *out_wrote = false;
    char spool_path[300];
    snprintf(spool_path, sizeof(spool_path), "%s/%s.spool", SD_SUMMARIES_DIR, day);
    struct stat st;
    if (stat(spool_path, &st) == 0) return ESP_OK;   /* live/STR coverage wins */

    char dir[300];
    snprintf(dir, sizeof(dir), "%s/%s", SD_STREAMS_DIR, day);
    DIR *d = opendir(dir);
    if (!d) return ESP_ERR_NOT_FOUND;

    /* ~4.5 KiB spool record — heap (PSRAM on target), never the stack. */
    str_day_t *sd = imp_calloc(1, sizeof(*sd));
    if (!sd) { closedir(d); return ESP_ERR_NO_MEM; }

    /* Pass 1: session windows + event counts */
    int n_sess = 0;
    int64_t dur_min_total = 0;
    int cnt_oai = 0, cnt_cai = 0, cnt_uai = 0, cnt_hyp = 0, cnt_rera = 0;
    int64_t csr_min = 0;
    sd->day_number = -1;
    for (int k = 0; k < 20; k++) sd->mask_on[k] = sd->mask_off[k] = -1;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        size_t nl = strlen(nm);
        if (nl < 14 || strcmp(nm + nl - 13, "_session.json")) continue;
        char mp[600];
        snprintf(mp, sizeof(mp), "%s/%s", dir, nm);
        cJSON *m = read_json_file(mp);
        if (!m) continue;
        int64_t s = 0, e = 0, drift = 0, off = 0;
        json_i64(m, "start_epoch_ms", &s);
        json_i64(m, "end_epoch_ms", &e);
        json_i64(m, "clock_drift_ms", &drift);
        json_i64(m, "waveform_offset_ms", &off);
        cJSON_Delete(m);
        /* Every completed session in the day counts — live or imported —
         * so mixed days still get correct SessionModeEntries. */
        if (!s || e <= s) continue;

        int64_t wav_dev = s - drift + off;
        int64_t dur_min = (e - s - off) / 60000;
        if (dur_min < 0) dur_min = 0;
        if (n_sess < 20) {
            sd->mask_on[n_sess]  = (wav_dev - (as11_time_local_noon_epoch(day) * 1000)) / 60000;
            sd->mask_off[n_sess] = sd->mask_on[n_sess] + dur_min;
            sd->n_pairs++;
        }
        dur_min_total += dur_min;
        n_sess++;

        /* events → indices */
        char ep[360];
        snprintf(ep, sizeof(ep), "%s/%.*s_events.snt", dir, (int)(nl - 13), nm);
        FILE *ef = fopen(ep, "r");
        if (ef) {
            char line[640];
            int64_t csr_t0 = -1;
            while (fgets(line, sizeof(line), ef)) {
                if (strstr(line, "\"ObstructiveApneaEnd\"")) cnt_oai++;
                else if (strstr(line, "\"CentralApneaEnd\"")) cnt_cai++;
                else if (strstr(line, "\"ApneaEnd\"")) cnt_uai++;
                else if (strstr(line, "\"HypopneaEnd\"")) cnt_hyp++;
                else if (strstr(line, "\"ReraEnd\"")) cnt_rera++;
                else if (strstr(line, "\"CsrStart\"")) {
                    const char *rt = strstr(line, "\"reportTime\":\"");
                    if (rt) csr_t0 = as11_time_parse_iso8601_ms(rt + 14);
                } else if (strstr(line, "\"CsrEnd\"") && csr_t0 >= 0) {
                    const char *rt = strstr(line, "\"reportTime\":\"");
                    if (rt) {
                        int64_t t = as11_time_parse_iso8601_ms(rt + 14);
                        if (t > csr_t0) csr_min += (t - csr_t0) / 60000;
                    }
                    csr_t0 = -1;
                }
            }
            fclose(ef);
        }
    }

    if (n_sess == 0) { closedir(d); free(sd); return ESP_ERR_NOT_FOUND; }
    closedir(d);

    /* readdir() order is arbitrary — SessionModeEntries must be
     * chronological (insertion sort; n_pairs <= 20). */
    for (int a = 1; a < sd->n_pairs; a++) {
        int64_t on = sd->mask_on[a], off = sd->mask_off[a];
        int b = a - 1;
        while (b >= 0 && sd->mask_on[b] > on) {
            sd->mask_on[b + 1] = sd->mask_on[b];
            sd->mask_off[b + 1] = sd->mask_off[b];
            b--;
        }
        sd->mask_on[b + 1] = on;
        sd->mask_off[b + 1] = off;
    }

    sd->duration_min = (int)dur_min_total;
    double hours = dur_min_total / 60.0;
    int ai = cnt_oai + cnt_cai + cnt_uai;
    int ahi = ai + cnt_hyp;
    #define IDX10(c) (hours > 0 ? (int64_t)llround((double)(c) * 10.0 / hours) : 0)
    sd->has_scalar[SF_AHI] = true; sd->scalars[SF_AHI] = IDX10(ahi);
    sd->has_scalar[SF_AI]  = true; sd->scalars[SF_AI]  = IDX10(ai);
    sd->has_scalar[SF_HI]  = true; sd->scalars[SF_HI]  = IDX10(cnt_hyp);
    sd->has_scalar[SF_OAI] = true; sd->scalars[SF_OAI] = IDX10(cnt_oai);
    sd->has_scalar[SF_CAI] = true; sd->scalars[SF_CAI] = IDX10(cnt_cai);
    sd->has_scalar[SF_UAI] = true; sd->scalars[SF_UAI] = IDX10(cnt_uai);
    sd->has_scalar[SF_RIN] = true; sd->scalars[SF_RIN] = IDX10(cnt_rera);
    if (csr_min > 0) {
        sd->has_scalar[SF_CSR] = true; sd->scalars[SF_CSR] = csr_min;
    }
    #undef IDX10

    /* Pass 2: PLD + SpO2 percentiles via bounded histograms.
     * channels: 0 MaskPress, 3 Leak, 4 RespRate, 5 TidVol, 6 MinVent →
     * fields 21/14/25/22/23; sa2 ch1 SpO2 → field 28.
     * Pass A streams each file for [min,max,count]; pass B bins over that
     * range (≤2048 bins/channel).  Raw spool values are phys×100 — the
     * same units the .snt stores, so no rescaling is needed. */
    static const int PC_CH[6] = { 0, 3, 4, 5, 6, -2 };  /* -2 → sa2 ch1 */
    static const int PC_FD[6] = {
        SF_MEAN_MASK_PRESS, SF_LEAK, SF_RESP_RATE, SF_TIDAL_VOL,
        SF_MIN_VENT, SF_SPO2
    };
    int32_t lo[6] = {0}, hi[6] = {0};
    uint32_t cnt[6] = {0};
    uint32_t (*hist)[2048] = NULL;

    /* scan A: ranges */
    d = opendir(dir);
    while (d && (ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        size_t nl = strlen(nm);
        bool is_pld = nl > 8 && !strcmp(nm + nl - 8, "_pld.snt");
        bool is_sa2 = nl > 8 && !strcmp(nm + nl - 8, "_sa2.snt");
        if (!is_pld && !is_sa2) continue;
        char fp[600];
        snprintf(fp, sizeof(fp), "%s/%s", dir, nm);
        FILE *f = fopen(fp, "rb");
        if (!f) continue;
        snt_header_t h;
        if (fread(&h, 1, sizeof(h), f) != sizeof(h) ||
            h.magic != SNT_MAGIC || h.n_channels == 0) { fclose(f); continue; }
        int16_t buf[512];
        size_t nf;
        while ((nf = fread(buf, (size_t)h.n_channels * 2,
                          512 / h.n_channels, f)) > 0) {
            for (size_t i = 0; i < nf; i++) {
                const int16_t *fr = buf + i * h.n_channels;
                for (int c = 0; c < 6; c++) {
                    int ch = PC_CH[c];
                    if (is_sa2 != (ch == -2)) continue;
                    if (ch == -2) ch = 1;
                    if (ch >= (int)h.n_channels) continue;
                    int32_t v = fr[ch];
                    if (v == SNT_MISSING) continue;
                    if (!cnt[c] || v < lo[c]) lo[c] = v;
                    if (!cnt[c] || v > hi[c]) hi[c] = v;
                    cnt[c]++;
                }
            }
        }
        fclose(f);
    }
    if (d) closedir(d);

    hist = imp_calloc(6 * 2048, sizeof(uint32_t));
    if (hist) {
        /* scan B: fill bins */
        d = opendir(dir);
        while (d && (ent = readdir(d)) != NULL) {
            const char *nm = ent->d_name;
            size_t nl = strlen(nm);
            bool is_pld = nl > 8 && !strcmp(nm + nl - 8, "_pld.snt");
            bool is_sa2 = nl > 8 && !strcmp(nm + nl - 8, "_sa2.snt");
            if (!is_pld && !is_sa2) continue;
            char fp[600];
            snprintf(fp, sizeof(fp), "%s/%s", dir, nm);
            FILE *f = fopen(fp, "rb");
            if (!f) continue;
            snt_header_t h;
            if (fread(&h, 1, sizeof(h), f) != sizeof(h) ||
                h.magic != SNT_MAGIC || h.n_channels == 0) { fclose(f); continue; }
            int16_t buf[512];
            size_t nf;
            while ((nf = fread(buf, (size_t)h.n_channels * 2,
                              512 / h.n_channels, f)) > 0) {
                for (size_t i = 0; i < nf; i++) {
                    const int16_t *fr = buf + i * h.n_channels;
                    for (int c = 0; c < 6; c++) {
                        int ch = PC_CH[c];
                        if (is_sa2 != (ch == -2)) continue;
                        if (!cnt[c] || hi[c] <= lo[c]) continue;
                        if (ch == -2) ch = 1;
                        if (ch >= (int)h.n_channels) continue;
                        int32_t v = fr[ch];
                        if (v == SNT_MISSING) continue;
                        int b = (int)(((int64_t)(v - lo[c]) * 2047) / (hi[c] - lo[c]));
                        if (b < 0) b = 0;
                        if (b > 2047) b = 2047;
                        hist[c][b]++;
                    }
                }
            }
            fclose(f);
        }
        if (d) closedir(d);

        for (int c = 0; c < 6; c++) {
            if (!cnt[c]) continue;
            int32_t span = hi[c] - lo[c];
            int32_t step_milli = (int32_t)(((int64_t)span * 1000) / 2047);
            int p50 = hist_pct(hist[c], 2048, lo[c], step_milli, cnt[c], 50);
            int p70 = hist_pct(hist[c], 2048, lo[c], step_milli, cnt[c], 70);
            int p95 = hist_pct(hist[c], 2048, lo[c], step_milli, cnt[c], 95);
            int f = PC_FD[c];
            sd->metrics[f].v[2] = p50; sd->metrics[f].has[2] = true;
            if (f == SF_LEAK) { sd->metrics[f].v[3] = p70; sd->metrics[f].has[3] = true; }
            sd->metrics[f].v[f == SF_LEAK ? 4 : 3] = p95;
            sd->metrics[f].has[f == SF_LEAK ? 4 : 3] = true;
            sd->metrics[f].v[f == SF_LEAK ? 5 : 4] = hi[c];
            sd->metrics[f].has[f == SF_LEAK ? 5 : 4] = true;
        }
        free(hist);
    }

    int64_t noon_ms = as11_time_local_noon_epoch(day) * 1000;
    if (noon_ms <= 0) noon_ms = 0;
    pbuf_t b = {0};
    encode_spool_day(&b, sd, noon_ms, noon_ms + 86400000LL);
    free(sd);
    esp_err_t ret = b.ovf ? ESP_ERR_NO_MEM : ESP_OK;
    if (ret == ESP_OK) {
        mkdir(SD_SESSIONS_DIR, 0775);
        mkdir(SD_SUMMARIES_DIR, 0775);
        char tmp[340];
        snprintf(tmp, sizeof(tmp), "%s.part", spool_path);
        FILE *f = fopen(tmp, "wb");
        if (!f) ret = ESP_FAIL;
        else {
            if (fwrite(b.buf, 1, b.len, f) != b.len) ret = (fclose(f), ESP_FAIL);
            else ret = imp_flush_close(f, tmp);
            if (ret == ESP_OK && imp_rename_replace(tmp, spool_path) != 0)
                ret = ESP_FAIL;
            if (ret != ESP_OK) unlink(tmp);
        }
        if (ret == ESP_OK) {
            if (out_wrote) *out_wrote = true;
            ESP_LOGI(TAG, "import: synthesized %s (%d sessions)",
                     spool_path, n_sess);
        }
    }
    free(b.buf);
    return ret;
}

/* ── Per-group processing ───────────────────────────────────────────── */

/* ResMed AS10 cards write a <name>.crc checksum beside every EDF in a
 * DATALOG day folder (AS11 cards do not).  It covers the file we are about
 * to consume, so it goes with it — otherwise the day dir can never become
 * empty and its husk shadows every later day (issue #338). */
static void delete_sidecar_crc(const char *edf_path)
{
    char crc[400];
    size_t n = strlen(edf_path);
    /* AS10 replaces the extension (<name>.edf → <name>.crc); a few tools
     * append instead (<name>.edf.crc) — try both. */
    if (n > 4 && !strcasecmp(edf_path + n - 4, ".edf")) {
        snprintf(crc, sizeof(crc), "%.*s.crc", (int)(n - 4), edf_path);
        unlink(crc);
    }
    snprintf(crc, sizeof(crc), "%s.crc", edf_path);
    unlink(crc);                 /* ENOENT is the normal AS11 case */
}

static void delete_source(const imp_file_t *f)
{
    delete_sidecar_crc(f->path);
    if (unlink(f->path) == 0) return;
    ESP_LOGW(TAG, "import: cannot delete %s: %s", f->path, strerror(errno));
}

/* Process one day-group: cluster → convert → commit → delete sources.
 * Sessions can land on a streams noon-day that differs from the group's
 * calendar-date key (00:00–11:59 starts belong to the previous noon-day);
 * the unit reports up to two touched days. */
static void process_group(edf_import_ctx_t *ctx, imp_group_t *g,
                          edf_import_unit_t *u)
{
    if (g->has_part) {
        ESP_LOGI(TAG, "import: %s has in-flight transfers — deferred",
                 g->src_dir);
        return;
    }
    memcpy(u->day_label, g->key, sizeof(u->day_label));

    imp_drift_t drift;
    parse_drift_sidecar(g->drift_path[0] ? g->drift_path : NULL, &drift);
    const char *import_src = g->drift_path[0] ? "somnobridge" : "file_import";

    imp_cluster_t cl[IMP_MAX_SESSIONS];
    int n_sess = cluster_files(g->files, g->n_files, cl, IMP_MAX_SESSIONS);
    ESP_LOGI(TAG, "import: %s → %d session cluster(s) from %d file(s)",
             g->src_dir, n_sess, g->n_files);

    /* The streams noon-day of a committed session can differ from the
     * group's calendar-date key — sessions starting 00:00–11:59 belong to
     * the previous noon-day.  Track the days actually touched so spool
     * synthesis and the rebuild trigger cover them. */
    char days[2][9] = { {0}, {0} };
    int  n_days = 0;

    for (int i = 0; i < n_sess; i++) {
        imp_work_t w;
        memset(&w, 0, sizeof(w));
        w.cl = &cl[i];
        w.drift = &drift;
        w.ident = ctx->ident;
        w.settings = ctx->settings;
        w.import_src = import_src;
        w.src_family = ctx->src_family;
        w.src_serial = ctx->src_serial;

        /* No root ident → fall back to the EDF recording_id (always
         * present on ResMed files; keeps ident available for AS10 cards
         * imported without Identification.tgt). */
        cJSON *rec_ident = NULL;
        if (!ctx->ident) {
            const imp_file_t *any = cl[i].brp ? cl[i].brp :
                cl[i].pld ? cl[i].pld : cl[i].sa2 ? cl[i].sa2 :
                cl[i].eve ? cl[i].eve : cl[i].csl;
            if (any) {
                imp_edf_t *e = imp_malloc(sizeof(imp_edf_t));
                if (e && imp_edf_open(e, any->path) == ESP_OK)
                    rec_ident = ident_from_rec_id(e->rec_id);
                free(e);
                if (rec_ident) w.ident = rec_ident;
            }
        }

        esp_err_t r = commit_session(&w, &cl[i], &drift);
        cJSON_Delete(rec_ident);

        const imp_file_t *srcs[] = { cl[i].brp, cl[i].pld, cl[i].sa2,
                                     cl[i].eve, cl[i].csl };
        if (r == ESP_OK) {
            u->sessions_imported++;
            for (int s = 0; s < 5; s++)
                if (srcs[s]) { delete_source(srcs[s]); u->files_consumed++; }
            if (w.day[0] &&
                (n_days == 0 ||
                 (strcmp(w.day, days[0]) &&
                  (n_days < 2 || strcmp(w.day, days[1]))))) {
                if (n_days < 2) {
                    memcpy(days[n_days], w.day, sizeof(days[0]));
                    n_days++;
                } else {
                    ESP_LOGW(TAG,
                             "import: third streams day %s in %s — "
                             "its rebuild waits for a later trigger",
                             w.day, g->src_dir);
                }
            }
        } else if (r == ESP_ERR_INVALID_STATE || r == ESP_ERR_NOT_SUPPORTED) {
            /* INVALID_STATE: true duplicate — every offered kind proved
             * covered.  NOT_FINISHED: augment merged what it safely could
             * and flagged the consumable sources in w.src_consumed; the
             * rest stay in place — unverifiable, never deleted on a
             * guess, retried on a later pass. */
            u->sessions_skipped++;
            for (int s = 0; s < 5; s++)
                if (srcs[s] && (w.src_consumed & (1u << s))) {
                    delete_source(srcs[s]);
                    u->files_consumed++;
                }
            if (r == ESP_ERR_NOT_SUPPORTED)
                ESP_LOGW(TAG, "import: %s session %s — unverifiable "
                         "source(s) kept for a later pass",
                         g->src_dir, w.id);
        } else {
            u->sessions_failed++;
            /* Move unconvertible sources aside as *.bad — never deleted,
             * but out of the scan path so a deterministic failure can't
             * pin the inbox forever.  The user can rename them back to
             * retry. */
            for (int s = 0; s < 5; s++) {
                if (!srcs[s]) continue;
                char bad[340];
                snprintf(bad, sizeof(bad), "%s.bad", srcs[s]->path);
                /* imp_rename_replace unlinks a stale <name>.bad first —
                 * without it a repeated failure wedges the source in
                 * IMPORT/ on FATFS (rename-to-existing fails). */
                if (imp_rename_replace(srcs[s]->path, bad) == 0) {
                    delete_sidecar_crc(srcs[s]->path);
                    u->files_rejected++;
                }
            }
        }
    }

    /* Orphan files in the group that never joined a cluster (unknown
     * kinds are already filtered out by classify). */
    for (int i = 0; i < g->n_files; i++) {
        bool used = false;
        for (int c = 0; c < n_sess && !used; c++) {
            used = (cl[c].brp == &g->files[i] || cl[c].pld == &g->files[i] ||
                    cl[c].sa2 == &g->files[i] || cl[c].eve == &g->files[i] ||
                    cl[c].csl == &g->files[i]);
        }
        if (!used && g->files[i].kind != K_OTHER) {
            /* A stray .edf that matched no cluster (e.g. >60 s TherapyStart
             * → MaskOn gap) — leave it for the user rather than guessing. */
            u->files_rejected++;
            ESP_LOGW(TAG, "import: %s joined no session — left in place",
                     g->files[i].path);
        }
    }

    /* The drift sidecar is consumed with the group (its contents live on
     * in each session's manifest). */
    if (g->drift_path[0]) unlink(g->drift_path);

    /* Report the streams days actually touched (the group key is the
     * source dir's calendar-date name, which can differ). */
    memcpy(u->day_label, n_days ? days[0] : g->key, sizeof(u->day_label));
    if (n_days > 1) memcpy(u->day2_label, days[1], sizeof(u->day2_label));

    /* Synthesize each touched day's spool when neither live capture nor
     * STR.edf covered it — must precede the day's EDF rebuild for correct
     * STR.  On failure, drop a <day>.needs_spool flag so the final sweep
     * retries it on this or the next scan. */
    for (int di = 0; di < n_days; di++) {
        bool wrote = false;
        if (synthesize_day_spool(days[di], &wrote) == ESP_OK) {
            if (wrote) u->spools_written++;
        } else {
            char flag[340];
            snprintf(flag, sizeof(flag), "%s/%s.needs_spool",
                     SD_SUMMARIES_DIR, days[di]);
            FILE *ff = fopen(flag, "w");
            if (ff) fclose(ff);
        }
    }

    /* Drained day-dir husks are removed; rmdir is a no-op while anything
     * (e.g. quarantined *.bad) remains.  Flat groups share IMPORT/ itself —
     * never remove that. */
    if (!g->is_flat) rmdir(g->src_dir);
    rmdir(IMP_DATALOG_DIR);

    if (drift.raw) cJSON_Delete(drift.raw);
}

/* ── Public API ─────────────────────────────────────────────────────── */

/* Quiescence window default.  Host tests create fixtures "just now", so the
 * host default is off; the firmware waits for a copy to go quiet. */
#ifdef EDF_IMPORT_HOST
#define IMP_QUIESCE_DEFAULT_S  0
#else
#define IMP_QUIESCE_DEFAULT_S  15
#endif

edf_import_ctx_t *edf_import_ctx_create(void)
{
    edf_import_ctx_t *c = imp_calloc(1, sizeof(*c));
    if (c) c->quiesce_s = IMP_QUIESCE_DEFAULT_S;
    return c;
}

void edf_import_ctx_set_quiesce(edf_import_ctx_t *ctx, int seconds)
{
    if (ctx) ctx->quiesce_s = seconds < 0 ? 0 : seconds;
}

void edf_import_ctx_destroy(edf_import_ctx_t *ctx)
{
    if (!ctx) return;
    cJSON_Delete(ctx->ident);
    cJSON_Delete(ctx->settings);
    free(ctx->scratch);
    free(ctx);
}

/* True when a directory tree still contains something consumable — a
 * *.edf or a subdirectory holding one, to depth `depth`.  Empty day-dir
 * husks, *.bad quarantine, in-flight *.part and stray sidecars don't
 * count: pending() must go false once everything usable was consumed so
 * the worker actually stops rescheduling itself. */
static bool dir_has_pending(const char *path, int depth)
{
    DIR *d = opendir(path);
    if (!d) return false;
    bool any = false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        size_t nl = strlen(nm);
        if (nl >= 1 && nm[0] == '.') continue;
        if (nl > 4 && !strcasecmp(nm + nl - 4, ".edf")) { any = true; break; }
        if (depth <= 0) continue;
        char full[400];
        snprintf(full, sizeof(full), "%s/%s", path, nm);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode) &&
            dir_has_pending(full, depth - 1)) { any = true; break; }
    }
    closedir(d);
    return any;
}

/* Deferred spool synthesis flags (<day>.needs_spool in summaries/) are
 * import work too — keep the worker alive until the sweep clears them. */
static bool needs_spool_pending(void)
{
    DIR *d = opendir(SD_SUMMARIES_DIR);
    if (!d) return false;
    struct dirent *ent;
    bool any = false;
    while ((ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        if (strlen(nm) == 20 && strcmp(nm + 8, ".needs_spool") == 0) {
            any = true;
            break;
        }
    }
    closedir(d);
    return any;
}

bool edf_import_pending(void)
{
    if (root_files_pending()) return true;
    if (dir_has_pending(IMP_IMPORT_DIR, 2)) return true;
    return needs_spool_pending();
}

#ifndef EDF_IMPORT_HOST
/* Newest streams day "YYYYMMDD" (for the STR-only root-pass rebuild —
 * regenerating any day also refreshes the cumulative STR.edf + root
 * artifacts, since edf_generate_str_edf scans every summaries spool). */
static bool find_newest_streams_day(char *out, size_t len)
{
    DIR *d = opendir(SD_STREAMS_DIR);
    if (!d) return false;
    char best[9] = "";
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        if (strlen(nm) != 8 || strcmp(nm, best) <= 0) continue;
        char p[300];
        snprintf(p, sizeof(p), "%s/%s", SD_STREAMS_DIR, nm);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode))
            memcpy(best, nm, 9);
    }
    closedir(d);
    if (!best[0]) return false;
    snprintf(out, len, "%s", best);
    return true;
}
#endif /* EDF_IMPORT_HOST */

/* Final sweep: days the run imported but whose spool synthesis never
 * landed (e.g. interrupted mid-unit) are flagged <day>.needs_spool in
 * summaries/ so this stays O(flags), not O(all days). */
static void final_needs_spool_sweep(void)
{
    DIR *d = opendir(SD_SUMMARIES_DIR);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        size_t nl = strlen(nm);
        if (nl != 20 || strcmp(nm + 8, ".needs_spool") != 0) continue;
        char day[9];
        memcpy(day, nm, 8); day[8] = '\0';
        char flag[400];
        snprintf(flag, sizeof(flag), "%s/%s", SD_SUMMARIES_DIR, nm);
        bool wrote = false;
        if (synthesize_day_spool(day, &wrote) == ESP_OK)
            unlink(flag);
        /* leave the flag on failure — retried next scan */
    }
    closedir(d);
}

/* ── Quiescence ─────────────────────────────────────────────────────────
 * A file modified moments ago may still be mid-copy.  FATFS only refreshes a
 * file's directory entry (size/mtime) on close/sync, so stat() alone cannot
 * prove an FTP upload is finished — the worker additionally holds off while
 * the FTP server is receiving (see edf_import_task); this check covers the
 * tail after the last close and any other writer.  A file dated in the
 * future (clock not yet set, or a PC with a fast clock) says nothing about
 * recency and never defers. */
static bool imp_recent(const char *path, int quiesce_s)
{
    if (quiesce_s <= 0) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;
    time_t now = time(NULL);
    if (st.st_mtime > now + 2) return false;
    return (now - st.st_mtime) < quiesce_s;
}

static bool root_files_recent(int quiesce_s)
{
    static const char *const paths[] = {
        IMP_IMPORT_DIR "/STR.edf",
        IMP_IMPORT_DIR "/Identification.json",
        IMP_IMPORT_DIR "/Identification.tgt",
        IMP_IMPORT_DIR "/SETTINGS/CurrentSettings.json",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
        if (imp_recent(paths[i], quiesce_s)) return true;
    return false;
}

static bool group_recent(const imp_group_t *g, int quiesce_s)
{
    if (g->drift_path[0] && imp_recent(g->drift_path, quiesce_s)) return true;
    for (int i = 0; i < g->n_files; i++)
        if (imp_recent(g->files[i].path, quiesce_s)) return true;
    return false;
}

esp_err_t edf_import_process_next(edf_import_ctx_t *ctx, edf_import_unit_t *out)
{
    if (!ctx || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    if (!ctx->cleaned) {
        edf_import_cleanup_residue();
        ctx->cleaned = true;
    }

    if (!ctx->root_done) {
        if (root_files_pending()) {
            /* Root files come first — sessions use the resolved ident and
             * settings — so a root copy still in flight holds everything. */
            if (root_files_recent(ctx->quiesce_s)) {
                ESP_LOGI(TAG, "import: root files modified within %d s — deferred",
                         ctx->quiesce_s);
                out->root_pass = true;
                out->deferred = true;
                return ESP_OK;
            }
            ctx->root_done = true;
            return process_root_pass(ctx, out);
        }
        ctx->root_done = true;
        /* fall through — no root files, look for day groups */
    }

    imp_group_t *g = imp_calloc(1, sizeof(imp_group_t));
    if (!g) return ESP_ERR_NO_MEM;
    bool have = next_group(g);
    if (have && group_recent(g, ctx->quiesce_s)) {
        ESP_LOGI(TAG, "import: %s modified within %d s — deferred",
                 g->src_dir, ctx->quiesce_s);
        out->deferred = true;
        free(g);
        return ESP_OK;
    }
    if (have) {
        process_group(ctx, g, out);
        free(g);
        /* A group deferred for in-flight transfers is not an error —
         * report the unit so the worker knows work remains pending. */
        return ESP_OK;
    }
    free(g);

    if (!ctx->sweep_done) {
        ctx->sweep_done = true;
        final_needs_spool_sweep();
    }
    /* EDFs are visible to the cheap pending check but no group could be
     * built from them — say so rather than reporting a clean empty inbox
     * (that silence is what made #338 hard to diagnose). */
    if (dir_has_pending(IMP_IMPORT_DIR, 2))
        ESP_LOGW(TAG, "import: EDFs remain in %s but no importable day group "
                 "was found (unsupported names or a folder with no session?)",
                 IMP_IMPORT_DIR);
    return ESP_ERR_NOT_FOUND;
}

/* ── ESP worker ─────────────────────────────────────────────────────── */

#ifndef EDF_IMPORT_HOST

#define IMP_WORKER_STACK   (20 * 1024)   /* PSRAM — cJSON_Parse recursion + inlined converters */
#define IMP_WORKER_PRIO    4
#define IMP_SETTLE_MS      15000   /* let SD + FTP quieten before scanning */
#define IMP_DEFER_POLL_MS  3000    /* re-check cadence while a copy is in flight */
#define IMP_DEFER_MAX_MS   (30 * 60 * 1000)  /* give up on a copy that never settles */

static volatile bool s_worker_running;

static void edf_import_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(IMP_SETTLE_MS));

    edf_import_ctx_t *ctx = edf_import_ctx_create();
    bool any_imported = false;
    int idle = 0;
    int defer_ms = 0;
    if (ctx) {
        for (;;) {
            if (!sd_storage_is_ready() || sd_storage_recording_active()) break;

            /* An FTP client writing into IMPORT/ is mid-copy by definition,
             * and FATFS only refreshes size/mtime on close — so stat() can't
             * tell.  Wait it out (no lease held), bounded. */
            if ((ftp_getstate() & 0xFF) == E_FTP_STE_CONTINUE_FILE_RX) {
                defer_ms += IMP_DEFER_POLL_MS;
                if (defer_ms > IMP_DEFER_MAX_MS) {
                    ESP_LOGW(TAG, "import: FTP still receiving after %d min — giving up until next trigger",
                             IMP_DEFER_MAX_MS / 60000);
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(IMP_DEFER_POLL_MS));
                continue;
            }
            if (!edf_import_pending()) {
                /* Empty inbox: still sweep stale residue once (the boot
                 * cleanup normally did this, but the worker can also be
                 * started later) — then exit WITHOUT ever blocking the
                 * export path. */
                if (!ctx->cleaned) {
                    edf_import_cleanup_residue();
                    ctx->cleaned = true;
                }
                break;
            }

            /* sd_storage_lease_acquire returns bool — false on timeout.
             * (A previous esp_err_t comparison here kept the lease
             * forever and deadlocked every export/upload on device.) */
            if (!sd_storage_lease_acquire(SD_LEASE_EXPORT, 30000)) {
                ESP_LOGW(TAG, "import: EXPORT lease busy — retrying");
                vTaskDelay(pdMS_TO_TICKS(30000));
                continue;
            }
            edf_import_unit_t u;
            esp_err_t r = edf_import_process_next(ctx, &u);
            sd_storage_lease_release(SD_LEASE_EXPORT);

            if (r == ESP_OK && u.deferred) {
                defer_ms += IMP_DEFER_POLL_MS;
                if (defer_ms > IMP_DEFER_MAX_MS) {
                    ESP_LOGW(TAG, "import: inbox still changing after %d min — giving up until next trigger",
                             IMP_DEFER_MAX_MS / 60000);
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(IMP_DEFER_POLL_MS));
                continue;
            }
            defer_ms = 0;
            if (r == ESP_ERR_NOT_FOUND) {
                if (++idle >= 3) break;
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            idle = 0;
            if (u.root_pass)
                ESP_LOGI(TAG, "import: root pass — ident %s settings %s, %d spool(s)",
                         ctx->ident ? "ok" : "none",
                         ctx->settings ? "ok" : "none", u.spools_written);
            else if (u.day_label[0])
                ESP_LOGI(TAG, "import: day %s — %d imported, %d skipped, %d failed, %d files",
                         u.day_label, u.sessions_imported, u.sessions_skipped,
                         u.sessions_failed, u.files_consumed);

            /* Trigger the standard pipeline for each imported day —
             * identical to live captures: rebuild the day's export, then
             * let uploaders pick up what changed.  Uploaders are only
             * nudged when the rebuild actually published. */
            if (u.day_label[0] && u.sessions_imported > 0) {
                any_imported = true;
                if (edf_gen_rebuild_day(u.day_label) == ESP_OK)
                    uploader_on_export_complete(u.day_label);
                if (u.day2_label[0] &&
                    edf_gen_rebuild_day(u.day2_label) == ESP_OK)
                    uploader_on_export_complete(u.day2_label);
            } else if (u.root_pass && u.spools_written > 0) {
                /* STR-only import: one rebuild refreshes the cumulative
                 * STR.edf + Identification/SETTINGS artifacts (the STR
                 * pass scans every summaries spool). */
                char day[9];
                if (find_newest_streams_day(day, sizeof(day)) &&
                    edf_gen_rebuild_day(day) == ESP_OK)
                    uploader_on_export_complete(day);
            }
            vTaskDelay(pdMS_TO_TICKS(200));   /* yield between units */
        }
        edf_import_ctx_destroy(ctx);
    }

    /* Score the newly imported _sa2.snt oximetry into .sst sidecars —
     * the reconcile pass is a no-op for days already scored. */
    if (any_imported) somno_ml_reconcile();

    ESP_LOGI(TAG, "import: inbox drained (worker stack: %u of %u bytes never used)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)IMP_WORKER_STACK);
    s_worker_running = false;
    vTaskDelete(NULL);
}

void edf_import_kick_now(void)
{
    esp_err_t err = edf_import_start_worker();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "import kick: %s", esp_err_to_name(err));
}

esp_err_t edf_import_start_worker(void)
{
    if (s_worker_running) return ESP_OK;
    s_worker_running = true;
    /* PSRAM stack — keeps ~14 KiB of internal SRAM free for Wi-Fi/TLS
     * (xTaskCreatePinnedToCore would carve it out of internal RAM). */
    if (!psram_task_create(edf_import_task, "edf_import", IMP_WORKER_STACK,
                          NULL, IMP_WORKER_PRIO, 1, NULL, NULL)) {
        s_worker_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#endif /* EDF_IMPORT_HOST */


