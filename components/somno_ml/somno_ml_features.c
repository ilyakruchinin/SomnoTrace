/*
 * SomnoTrace - Causal feature engine for the SomnoStage model
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

/* Faithful C11 port of model/inference/features.py + signal_processing.py
 * (contract ring_tabular_v4_1). Every statistic is trailing-only and matches
 * the pandas/numpy reference semantics:
 *   - per-sample trailing median-5 filter (valid samples only)
 *   - per-epoch population std (np.std, ddof=0) on filtered values that are
 *     source-valid, finite and in range
 *   - rolling mean (min_periods=1) / sample std (pandas ddof=1, min_periods=2),
 *     NaN-skipping, invalid epochs contribute NaN rows
 *   - rolling median/quantile via linear interpolation (pandas default)
 *   - np.percentile(...,90) linear for the 10-minute SpO2 baseline
 *   - NaN-propagating diff() deltas; fixed 0.5-min-per-epoch elapsed time
 *   - spo2_age_epochs = 0-based position inside a run of invalid epochs
 */

#include "somno_ml_features.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define SOMNO_CALLOC(n, sz) heap_caps_calloc((n), (sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define SOMNO_FREE        heap_caps_free
#else
#define SOMNO_CALLOC(n, sz) calloc((n), (sz))
#define SOMNO_FREE        free
#endif

#define MED_K        5
#define BASE_CAP     600
#define HIST_CAP     121
#define MAX_SPE      30

#define SPO2_MIN     50.0
#define SPO2_MAX     100.0
#define PR_MIN       30.0
#define PR_MAX       220.0
#define MOTION_MIN   0.0
#define MOTION_MAX   255.0
#define BASELINE_DEF 96.0

/* rolling windows in epochs: 1m 2m 5m 15m 30m 60m */
static const int k_windows[6] = {3, 5, 11, 31, 61, 121};
#define N_WINDOWS 6
#define N_ROLL_COLS 5   /* spo2_mean, pr_mean, rmssd, desat_depth, motion_mean */

/* epoch history columns */
enum {
    HC_SPO2_MEAN = 0,
    HC_PR_MEAN,
    HC_RMSSD,
    HC_DESAT_DEPTH,
    HC_MOTION_MEAN,
    HC_DESAT_FLAG3,
    HC_TRAIL_BASE,
    HC_N
};

/* contract feature indices (ring_tabular_v4_1 order) */
enum {
    F_SPO2_STD = 0, F_DESAT_DEPTH, F_DESAT_DROP_VEL, F_DESAT_AREA90,
    F_DESAT_AREA_BASE, F_DESAT_FLAG3, F_DESAT_FLAG4, F_PR_STD, F_SDNN,
    F_RMSSD, F_PNN50, F_MOTION_MEAN, F_MOTION_MAX, F_MOTION_STD,
    F_SPO2_VF, F_PR_VF, F_MOTION_VF,
    F_ROLL = 17,                /* +window*10 + col*2 (+0 mean, +1 std) */
    F_DELTA = 17 + N_WINDOWS * N_ROLL_COLS * 2,   /* 77: +col*2 +period */
    F_DESAT_EV_5M = F_DELTA + 8,     /* 85 */
    F_DESAT_EV_30M,                /* 86 */
    F_PR_JUMP,                     /* 87 */
    F_ELAPSED,                     /* 88 */
    F_SPO2_REL_BASE,               /* 89 */
    F_PR_REL_MED,                  /* 90 */
    F_SPO2_CZ,                     /* 91 */
    F_PR_CZ,                       /* 92 */
    F_PRV_RATIO,                   /* 93 */
    F_SPO2_AGE,                    /* 94 */
};

typedef struct {
    double col[HC_N];
    uint8_t valid;
} ep_row_t;

struct somno_feat {
    double dt;
    int spe;                 /* samples per epoch = round(30/dt) */
    int base_n;              /* round(600/dt) */
    int has_motion;
    int min_required;        /* ceil(0.25 * spe) */

    /* per-channel trailing median-5 rings */
    double mval[3][MED_K];
    uint8_t mok[3][MED_K];
    int mfill;

    /* baseline ring: filtered spo2 + original validity, last base_n samples */
    double bval[BASE_CAP];
    double baseline_scratch[BASE_CAP];
    uint8_t bok[BASE_CAP];
    int bhead, bcount;

    /* current-epoch accumulators (filtered values + original valid flags) */
    double ev[3][MAX_SPE];
    uint8_t evv[3][MAX_SPE];
    int efill;

    ep_row_t hist[HIST_CAP];
    int hcount;              /* == epochs emitted (<= HIST_CAP window) */
    int epoch_idx;
    int invalid_run;
    int last_valid;
};

static int dbl_cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* pandas/numpy linear-interpolated quantile over a sorted array. */
static double quantile_sorted(const double *s, int n, double q)
{
    if (n <= 0) return NAN;
    if (n == 1) return s[0];
    double pos = q * (n - 1);
    int lo = (int)floor(pos);
    double frac = pos - lo;
    if (lo + 1 >= n) return s[lo];
    return s[lo] + (s[lo + 1] - s[lo]) * frac;
}

static double median_of(const double *vals, int n)
{
    double tmp[MED_K];
    memcpy(tmp, vals, (size_t)n * sizeof(double));
    qsort(tmp, (size_t)n, sizeof(double), dbl_cmp);
    if (n & 1) return tmp[n / 2];
    return (tmp[n / 2 - 1] + tmp[n / 2]) / 2.0;
}

/* trailing median over the last MED_K samples of one channel */
static double trailing_median(struct somno_feat *f, int ch)
{
    double vals[MED_K];
    int n = 0;
    for (int i = 0; i < f->mfill && i < MED_K; i++) {
        int idx = (f->mfill - 1 - i + 5 * MED_K) % MED_K; /* newest backwards */
        if (f->mok[ch][idx])
            vals[n++] = f->mval[ch][idx];
    }
    return n ? median_of(vals, n) : NAN;
}

somno_feat_t *somno_feat_create(double dt, int has_motion)
{
    somno_feat_t *f = SOMNO_CALLOC(1, sizeof(*f));
    if (!f) return NULL;
    f->dt = dt;
    f->spe = (int)(30.0 / dt + 0.5);
    if (f->spe < 1 || f->spe > MAX_SPE) { SOMNO_FREE(f); return NULL; }
    f->base_n = (int)(600.0 / dt + 0.5);
    if (f->base_n > BASE_CAP) f->base_n = BASE_CAP;
    f->has_motion = has_motion;
    f->min_required = (int)ceil(0.25 * f->spe);
    if (f->min_required < 2) f->min_required = 2;
    return f;
}

void somno_feat_destroy(somno_feat_t *f) { SOMNO_FREE(f); }

int somno_feat_epochs(const somno_feat_t *f) { return f->epoch_idx; }
int somno_feat_last_valid(const somno_feat_t *f) { return f->last_valid; }

/* ---- epoch base features (port of _epoch_base_features) ---- */

static void epoch_base(somno_feat_t *f, double rolling_baseline, ep_row_t *row,
                       float out[SOMNO_ML_N_FEATURES])
{
    int n_spo2 = 0, n_pr = 0;
    double ssum = 0, smin = INFINITY, smax = -INFINITY;
    double psum = 0;
    double rr[MAX_SPE];
    int n_rr = 0;
    double msum = 0, mmax = -INFINITY; int n_mot = 0;
    double mvals[MAX_SPE];

    for (int i = 0; i < f->spe; i++) {
        double v = f->ev[0][i];
        if (f->evv[0][i] && isfinite(v) && v >= SPO2_MIN && v <= SPO2_MAX) {
            n_spo2++; ssum += v;
            if (v < smin) smin = v;
            if (v > smax) smax = v;
            f->ev[0][i] = v; /* keep */
        } else {
            f->ev[0][i] = NAN;
        }
        v = f->ev[1][i];
        if (f->evv[1][i] && isfinite(v) && v >= PR_MIN && v <= PR_MAX) {
            n_pr++; psum += v;
            rr[n_rr++] = 60000.0 / v;
        } else {
            f->ev[1][i] = NAN;
        }
        if (f->has_motion) {
            v = f->ev[2][i];
            if (f->evv[2][i] && isfinite(v) && v >= MOTION_MIN && v <= MOTION_MAX) {
                mvals[n_mot++] = v;
                msum += v;
                if (v > mmax) mmax = v;
            }
        }
    }

    double spo2_frac = (double)n_spo2 / f->spe;
    double pr_frac = (double)n_pr / f->spe;

    memset(row, 0, sizeof(*row));
    for (int c = 0; c < HC_N; c++) row->col[c] = NAN;
    row->col[HC_TRAIL_BASE] = rolling_baseline;
    out[F_SPO2_VF] = (float)spo2_frac;
    out[F_PR_VF] = (float)pr_frac;
    out[F_MOTION_VF] = 0.0f;   /* invalid-epoch default (matches reference) */

    if (n_spo2 < f->min_required || n_pr < f->min_required) {
        row->valid = 0;
        return;
    }
    row->valid = 1;
    out[F_MOTION_VF] = NAN;    /* no-motion source default for valid epochs */

    double spo2_mean = ssum / n_spo2;
    double pr_mean = psum / n_pr;
    /* population std (np.std, ddof=0) */
    double acc = 0;
    for (int i = 0; i < f->spe; i++) {
        double v = f->ev[0][i];
        if (isfinite(v)) { double d = v - spo2_mean; acc += d * d; }
    }
    double spo2_std = sqrt(acc / n_spo2);
    acc = 0;
    double pmin = INFINITY, pmax = -INFINITY;
    for (int i = 0; i < f->spe; i++) {
        double v = f->ev[1][i];
        if (isfinite(v)) {
            double d = v - pr_mean; acc += d * d;
            if (v < pmin) pmin = v;
            if (v > pmax) pmax = v;
        }
    }
    double pr_std = sqrt(acc / n_pr);

    /* rr statistics on the valid pr sequence (order preserved) */
    double rr_mean = 0;
    for (int i = 0; i < n_rr; i++) rr_mean += rr[i];
    rr_mean /= n_rr;
    acc = 0;
    for (int i = 0; i < n_rr; i++) { double d = rr[i] - rr_mean; acc += d * d; }
    double sdnn = sqrt(acc / n_rr);
    double rmssd = NAN, pnn50 = NAN;
    if (n_rr > 1) {
        acc = 0; int n50 = 0;
        for (int i = 0; i < n_rr - 1; i++) {
            double d = rr[i + 1] - rr[i];
            acc += d * d;
            if (fabs(d) > 50.0) n50++;
        }
        rmssd = sqrt(acc / (n_rr - 1)) * sqrt(1.0 / f->dt);
        pnn50 = (double)n50 / (n_rr - 1) * 100.0;
    }

    double desat_depth = rolling_baseline - smin;
    if (desat_depth < 0) desat_depth = 0;
    double area90 = 0, areabase = 0;
    for (int i = 0; i < f->spe; i++) {
        double v = f->ev[0][i];
        if (isfinite(v)) {
            if (v < 90.0) area90 += 90.0 - v;
            if (v < rolling_baseline) areabase += rolling_baseline - v;
        }
    }
    area90 *= f->dt;
    areabase *= f->dt;

    out[F_SPO2_STD] = (float)spo2_std;
    out[F_DESAT_DEPTH] = (float)desat_depth;
    out[F_DESAT_DROP_VEL] = (float)(fmax(0.0, smax - smin) / 30.0);
    out[F_DESAT_AREA90] = (float)area90;
    out[F_DESAT_AREA_BASE] = (float)areabase;
    out[F_DESAT_FLAG3] = desat_depth >= 3.0 ? 1.0f : 0.0f;
    out[F_DESAT_FLAG4] = desat_depth >= 4.0 ? 1.0f : 0.0f;
    out[F_PR_STD] = (float)pr_std;
    out[F_SDNN] = (float)sdnn;
    out[F_RMSSD] = (float)rmssd;
    out[F_PNN50] = (float)pnn50;

    row->col[HC_SPO2_MEAN] = spo2_mean;
    row->col[HC_PR_MEAN] = pr_mean;
    row->col[HC_RMSSD] = rmssd;
    row->col[HC_DESAT_DEPTH] = desat_depth;
    row->col[HC_DESAT_FLAG3] = desat_depth >= 3.0 ? 1.0 : 0.0;

    if (f->has_motion) {
        /* fraction of samples whose filtered motion value was valid/in-range */
        int mv_cnt = 0;
        for (int i = 0; i < f->spe; i++) {
            double v = f->ev[2][i];
            if (f->evv[2][i] && isfinite(v) && v >= MOTION_MIN && v <= MOTION_MAX)
                mv_cnt++;
        }
        out[F_MOTION_VF] = (float)((double)mv_cnt / f->spe);
        if (n_mot) {
            double mm = msum / n_mot;
            acc = 0;
            for (int i = 0; i < n_mot; i++) { double d = mvals[i] - mm; acc += d * d; }
            out[F_MOTION_MEAN] = (float)mm;
            out[F_MOTION_MAX] = (float)mmax;
            out[F_MOTION_STD] = (float)sqrt(acc / n_mot);
            row->col[HC_MOTION_MEAN] = mm;
        } else {
            out[F_MOTION_MEAN] = out[F_MOTION_MAX] = out[F_MOTION_STD] = NAN;
        }
    } else {
        out[F_MOTION_MEAN] = out[F_MOTION_MAX] = out[F_MOTION_STD] = NAN;
    }
}

/* ---- rolling/derived features over the epoch history ring ---- */

static int hist_idx(const somno_feat_t *f, int back)
{
    /* back=0 -> most recent row */
    int pos = f->hcount - 1 - back;
    return pos >= 0 ? pos : -1;
}

static double roll_mean_std(const somno_feat_t *f, int col, int window,
                            double *std_out)
{
    double buf[HIST_CAP];
    int n = 0;
    for (int b = 0; b < window; b++) {
        int i = hist_idx(f, b);
        if (i < 0) break;
        double v = f->hist[i].col[col];
        if (isfinite(v)) buf[n++] = v;
    }
    double mean = NAN, std = NAN;
    if (n >= 1) {
        double s = 0;
        for (int i = 0; i < n; i++) s += buf[i];
        mean = s / n;
    }
    if (n >= 2) {
        double s = 0;
        for (int i = 0; i < n; i++) { double d = buf[i] - mean; s += d * d; }
        std = sqrt(s / (n - 1));
    }
    *std_out = std;
    return mean;
}

static double roll_sum(const somno_feat_t *f, int col, int window)
{
    double s = 0; int n = 0;
    for (int b = 0; b < window; b++) {
        int i = hist_idx(f, b);
        if (i < 0) break;
        double v = f->hist[i].col[col];
        if (isfinite(v)) { s += v; n++; }
    }
    return n ? s : NAN;
}

static void derived_features(somno_feat_t *f, float out[SOMNO_ML_N_FEATURES])
{
    /* rolling mean/std blocks */
    int fi = F_ROLL;
    for (int w = 0; w < N_WINDOWS; w++) {
        for (int c = 0; c < N_ROLL_COLS; c++) {
            double std;
            double mean = roll_mean_std(f, c, k_windows[w], &std);
            out[fi++] = (float)mean;
            out[fi++] = (float)std;
        }
    }
    /* deltas: col - col[back], NaN propagates */
    static const int delta_cols[4] = {HC_SPO2_MEAN, HC_PR_MEAN, HC_RMSSD, HC_MOTION_MEAN};
    static const int delta_periods[2] = {2, 4};
    fi = F_DELTA;
    for (int c = 0; c < 4; c++) {
        for (int p = 0; p < 2; p++) {
            double v = NAN;
            int i0 = hist_idx(f, 0), i1 = hist_idx(f, delta_periods[p]);
            if (i0 >= 0 && i1 >= 0)
                v = f->hist[i0].col[delta_cols[c]] - f->hist[i1].col[delta_cols[c]];
            out[fi++] = (float)v;
        }
    }
    out[F_DESAT_EV_5M] = (float)roll_sum(f, HC_DESAT_FLAG3, 11);
    out[F_DESAT_EV_30M] = (float)roll_sum(f, HC_DESAT_FLAG3, 61);

    /* pr_jump_1m = max |pm[j]-pm[j-1]| over j in {e-2,e-1,e} */
    {
        double mx = NAN;
        for (int b = 0; b < 3; b++) {
            int j = hist_idx(f, b), j1 = hist_idx(f, b + 1);
            if (j < 0 || j1 < 0) continue;
            double d = fabs(f->hist[j].col[HC_PR_MEAN] - f->hist[j1].col[HC_PR_MEAN]);
            if (isfinite(d) && (!isfinite(mx) || d > mx)) mx = d;
        }
        out[F_PR_JUMP] = (float)mx;
    }

    out[F_ELAPSED] = (float)fmin(f->epoch_idx * 0.5, 720.0);

    int e = hist_idx(f, 0);
    double spo2_mean = f->hist[e].col[HC_SPO2_MEAN];
    double pr_mean = f->hist[e].col[HC_PR_MEAN];

    out[F_SPO2_REL_BASE] = (float)(spo2_mean - f->hist[e].col[HC_TRAIL_BASE]);

    {
        /* pr_rel_med_30m: rolling median window 60, min_periods 20 */
        double buf[HIST_CAP]; int n = 0;
        for (int b = 0; b < 60; b++) {
            int i = hist_idx(f, b);
            if (i < 0) break;
            double v = f->hist[i].col[HC_PR_MEAN];
            if (isfinite(v)) buf[n++] = v;
        }
        double med = NAN;
        if (n >= 20) {
            qsort(buf, (size_t)n, sizeof(double), dbl_cmp);
            med = quantile_sorted(buf, n, 0.5);
        }
        out[F_PR_REL_MED] = (float)(pr_mean - med);
    }

    /* causal z: (x - med120) / iqr120, min_periods 40, iqr==0 -> NaN */
    for (int c = 0; c < 2; c++) {
        int col = c == 0 ? HC_SPO2_MEAN : HC_PR_MEAN;
        double x = f->hist[e].col[col];
        double buf[HIST_CAP]; int n = 0;
        for (int b = 0; b < 120; b++) {
            int i = hist_idx(f, b);
            if (i < 0) break;
            double v = f->hist[i].col[col];
            if (isfinite(v)) buf[n++] = v;
        }
        double z = NAN;
        if (n >= 40) {
            qsort(buf, (size_t)n, sizeof(double), dbl_cmp);
            double med = quantile_sorted(buf, n, 0.5);
            double q75 = quantile_sorted(buf, n, 0.75);
            double q25 = quantile_sorted(buf, n, 0.25);
            double iqr = q75 - q25;
            if (iqr != 0.0) z = (x - med) / iqr;
        }
        out[c == 0 ? F_SPO2_CZ : F_PR_CZ] = (float)z;
    }

    /* prv_ratio_5m_30m = mean11(min_periods=3) / mean121(min_periods=40);
     * denominator 0 -> NaN */
    {
        double buf[HIST_CAP]; int n = 0;
        for (int b = 0; b < 11; b++) {
            int i = hist_idx(f, b);
            if (i < 0) break;
            double v = f->hist[i].col[HC_RMSSD];
            if (isfinite(v)) buf[n++] = v;
        }
        double num = NAN;
        if (n >= 3) { double s = 0; for (int i = 0; i < n; i++) s += buf[i]; num = s / n; }
        n = 0;
        for (int b = 0; b < 121; b++) {
            int i = hist_idx(f, b);
            if (i < 0) break;
            double v = f->hist[i].col[HC_RMSSD];
            if (isfinite(v)) buf[n++] = v;
        }
        double den = NAN;
        if (n >= 40) { double s = 0; for (int i = 0; i < n; i++) s += buf[i]; den = s / n; }
        out[F_PRV_RATIO] = (den != 0.0) ? (float)(num / den) : NAN;
    }

    out[F_SPO2_AGE] = (float)f->invalid_run;
}

int somno_feat_push(somno_feat_t *f, double spo2, double pr, double motion,
                    int spo2_valid, int pr_valid, int motion_valid,
                    float feats[SOMNO_ML_N_FEATURES])
{
    /* push into per-channel median-5 rings */
    int slot = f->mfill % MED_K;
    f->mval[0][slot] = spo2;    f->mok[0][slot] = spo2_valid && isfinite(spo2);
    f->mval[1][slot] = pr;      f->mok[1][slot] = pr_valid && isfinite(pr);
    f->mval[2][slot] = motion;  f->mok[2][slot] = motion_valid && isfinite(motion);
    f->mfill++;

    double fs = trailing_median(f, 0);
    double fp = trailing_median(f, 1);
    double fm = f->has_motion ? trailing_median(f, 2) : NAN;

    /* baseline ring: filtered spo2 + original validity */
    int bslot = (f->bhead + f->bcount) % f->base_n;
    if (f->bcount == f->base_n) { f->bhead = (f->bhead + 1) % f->base_n; f->bcount--; bslot = (f->bhead + f->bcount) % f->base_n; }
    f->bval[bslot] = fs;
    f->bok[bslot] = spo2_valid;
    f->bcount++;

    int e = f->efill;
    f->ev[0][e] = fs;  f->evv[0][e] = spo2_valid;
    f->ev[1][e] = fp;  f->evv[1][e] = pr_valid;
    f->ev[2][e] = fm;  f->evv[2][e] = f->has_motion ? motion_valid : 0;
    f->efill++;
    if (f->efill < f->spe) return 0;
    f->efill = 0;

    for (int i = 0; i < SOMNO_ML_N_FEATURES; i++) feats[i] = NAN;

    /* 10-minute baseline p90 ending at this epoch's last sample */
    double *bbuf = f->baseline_scratch;
    int bn = 0;
    for (int i = 0; i < f->bcount; i++) {
        int idx = (f->bhead + i) % f->base_n;
        double v = f->bval[idx];
        if (f->bok[idx] && isfinite(v) && v >= 80.0 && v <= 100.0)
            bbuf[bn++] = v;
    }
    double rolling_baseline = BASELINE_DEF;
    if (bn >= 10) {
        qsort(bbuf, (size_t)bn, sizeof(double), dbl_cmp);
        rolling_baseline = quantile_sorted(bbuf, bn, 0.9);
        if (!isfinite(rolling_baseline)) rolling_baseline = BASELINE_DEF;
    }

    /* append the new epoch row: shift left when the ring is full BEFORE
     * writing, so hist[hcount-1] is always the freshest row */
    ep_row_t *row;
    if (f->hcount < HIST_CAP) {
        row = &f->hist[f->hcount++];
    } else {
        memmove(f->hist, f->hist + 1, sizeof(ep_row_t) * (HIST_CAP - 1));
        row = &f->hist[HIST_CAP - 1];
    }
    epoch_base(f, rolling_baseline, row, feats);
    f->last_valid = row->valid;
    if (row->valid)
        f->invalid_run = 0;

    /* invalid_run = count of consecutive invalid epochs BEFORE this one, so
     * the first invalid epoch of a run gets age 0 (matches cumcount*invalid) */
    derived_features(f, feats);
    if (!row->valid)
        f->invalid_run++;

    f->epoch_idx++;
    return 1;
}
