/*
 * SomnoTrace - Canonical EDF signal definitions and data dictionary
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

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Spool → EDF fixed-point conversion ──────────────────────────────── */

/* Convert a Summary-spool fixed-point integer to its EDF digital value.
 * `raw == -1` is the missing sentinel and passes through untouched.
 * Rounding is SYMMETRIC (half away from zero) — truncating here silently
 * biased every STR settings value low, which is what issue #192 reported.
 *
 * Lives in this header rather than in edf_summary.c so the host property
 * tests bind to the same definition the firmware uses. A test carrying its
 * own copy stays green when this one changes. */
static inline int16_t spool_to_edf(int16_t raw, int num, int den)
{
    if (raw == -1 || den <= 0) return -1;
    int32_t prod = (int32_t)raw * num;
    int32_t half = den / 2;
    int32_t rounded = (prod >= 0) ? (prod + half) / den : (prod - half) / den;
    if (rounded > INT16_MAX) return INT16_MAX;
    if (rounded < INT16_MIN) return INT16_MIN;
    return (int16_t)rounded;
}

/* ── Signal definition structure ─────────────────────────────────────── */

typedef struct {
    const char *label;              /* 16 chars max */
    const char *transducer;         /* 80 chars max */
    const char *unit;               /* 8 chars max  */
    double phys_min;
    double phys_max;
    int dig_min;
    int dig_max;
    const char *prefilter;          /* 80 chars max */
    int samples_per_record;
    /* When true, a raw sample equal to the missing sentinel is written to EDF
     * as digital -1 directly, bypassing physical->digital scaling. */
    bool invalid_passthrough;
} edf_signal_def_t;

/* ── Signal Counts ─────────────────────────────────────────────────── */

#define EDF_BRP_SIGNAL_COUNT     2
#define EDF_SA2_SIGNAL_COUNT     2
#define EDF_PLD_SIGNAL_COUNT     9
#define EDF_PLD_RAW_CHANNELS    12
#define EDF_STR_SIGNAL_COUNT   133
#define STR_DATA_COUNT         EDF_STR_SIGNAL_COUNT
#define STR_SIGNAL_COUNT       134   /* includes Crc16 */

/* ── BRP Signals (25 Hz Breath Waveform) ───────────────────────────── */

static const edf_signal_def_t g_brp_signals[EDF_BRP_SIGNAL_COUNT] = {
    { .label = "Flow.40ms", .transducer = "",
      .unit = "L/s", .phys_min = -2.0, .phys_max = 3.0,
      .dig_min = -1000, .dig_max = 1500,
      .prefilter = "", .samples_per_record = 1500,
      .invalid_passthrough = false },
    { .label = "Press.40ms", .transducer = "",
      .unit = "cmH2O", .phys_min = 0.0, .phys_max = 40.0,
      .dig_min = 0, .dig_max = 2000,
      .prefilter = "", .samples_per_record = 1500,
      .invalid_passthrough = false },
};

/* ── SA2 Signals (1 Hz Oximetry Waveform) ──────────────────────────── */

static const edf_signal_def_t g_sa2_signals[EDF_SA2_SIGNAL_COUNT] = {
    { .label = "Pulse.1s", .transducer = "",
      .unit = "bpm", .phys_min = 0.0, .phys_max = 300.0,
      .dig_min = 0, .dig_max = 300,
      .prefilter = "", .samples_per_record = 60,
      .invalid_passthrough = true },
    { .label = "SpO2.1s", .transducer = "",
      .unit = "%", .phys_min = 0.0, .phys_max = 100.0,
      .dig_min = 0, .dig_max = 100,
      .prefilter = "", .samples_per_record = 60,
      .invalid_passthrough = true },
};

/* ── PLD Signals (0.5 Hz Per-Breath Statistics) ────────────────────── */

static const edf_signal_def_t g_pld_signals[EDF_PLD_SIGNAL_COUNT] = {
    { .label = "MaskPress.2s", .transducer = "",
      .unit = "cmH2O", .phys_min = 0.0, .phys_max = 40.0,
      .dig_min = 0, .dig_max = 2000,
      .prefilter = "", .samples_per_record = 30,
      .invalid_passthrough = false },
    { .label = "Press.2s", .transducer = "",
      .unit = "cmH2O", .phys_min = 0.0, .phys_max = 50.0,
      .dig_min = 0, .dig_max = 2500,
      .prefilter = "", .samples_per_record = 30,
      .invalid_passthrough = false },
    { .label = "EprPress.2s", .transducer = "",
      .unit = "cmH2O", .phys_min = 0.0, .phys_max = 30.0,
      .dig_min = 0, .dig_max = 1500,
      .prefilter = "", .samples_per_record = 30,
      .invalid_passthrough = false },
    { .label = "Leak.2s", .transducer = "",
      .unit = "L/s", .phys_min = 0.0, .phys_max = 2.0,
      .dig_min = 0, .dig_max = 100,
      .prefilter = "", .samples_per_record = 30,
      .invalid_passthrough = false },
    { .label = "RespRate.2s", .transducer = "",
      .unit = "bpm", .phys_min = 0.0, .phys_max = 90.0,
      .dig_min = 0, .dig_max = 450,
      .prefilter = "", .samples_per_record = 30,
      .invalid_passthrough = false },
    { .label = "TidVol.2s", .transducer = "",
      .unit = "L", .phys_min = 0.0, .phys_max = 4.0,
      .dig_min = 0, .dig_max = 200,
      .prefilter = "", .samples_per_record = 30,
      .invalid_passthrough = false },
    { .label = "MinVent.2s", .transducer = "",
      .unit = "L/min", .phys_min = 0.0, .phys_max = 30.0,
      .dig_min = 0, .dig_max = 240,
      .prefilter = "", .samples_per_record = 30,
      .invalid_passthrough = false },
    { .label = "Snore.2s", .transducer = "",
      .unit = "", .phys_min = 0.0, .phys_max = 5.0,
      .dig_min = 0, .dig_max = 250,
      .prefilter = "", .samples_per_record = 30,
      .invalid_passthrough = false },
    { .label = "FlowLim.2s", .transducer = "",
      .unit = "", .phys_min = 0.0, .phys_max = 1.0,
      .dig_min = 0, .dig_max = 100,
      .prefilter = "", .samples_per_record = 30,
      .invalid_passthrough = true },
};

/* PLD channel mapping: maps 9 EDF channels from 12 raw SNT channels */
static const int g_pld_ch_map[EDF_PLD_SIGNAL_COUNT] = {
    0, 1, 2, 3, 4, 5, 6, 9, 10
};

/* ── EVE/CSL Annotation Signal ─────────────────────────────────────── */

static const edf_signal_def_t g_annotation_signal = {
    .label = "EDF Annotations",
    .transducer = "",
    .unit = "",
    .phys_min = -32768.0,
    .phys_max = 32767.0,
    .dig_min = -32768,
    .dig_max = 32767,
    .prefilter = "",
    .samples_per_record = 0,
    .invalid_passthrough = false,
};

/* ── STR.edf Signals (133 Summary Signals without Crc16) ───────────── */

static const edf_signal_def_t g_str_signals[EDF_STR_SIGNAL_COUNT] = {
/* [0-3] Session header */
        { .label="Date", .transducer="", .unit="", .phys_min=0.0, .phys_max=24836.0, .dig_min=0, .dig_max=24836, .prefilter="", .samples_per_record=1 },
        { .label="MaskOn", .transducer="", .unit="MINUTES", .phys_min=0.0, .phys_max=1440.0, .dig_min=0, .dig_max=1440, .prefilter="", .samples_per_record=20 },
        { .label="MaskOff", .transducer="", .unit="MINUTES", .phys_min=0.0, .phys_max=1440.0, .dig_min=0, .dig_max=1440, .prefilter="", .samples_per_record=20 },
        { .label="MaskEvents", .transducer="", .unit="", .phys_min=0.0, .phys_max=255.0, .dig_min=0, .dig_max=255, .prefilter="", .samples_per_record=1 },
        /* [4-5] Session core */
        { .label="Duration", .transducer="", .unit="min.", .phys_min=0.0, .phys_max=1440.0, .dig_min=0, .dig_max=1440, .prefilter="", .samples_per_record=1 },
        { .label="Mode", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        /* [6-13] CPAP/AutoSet settings */
        { .label="S.C.StartPress", .transducer="", .unit="cmH2O", .phys_min=4.0, .phys_max=20.0, .dig_min=200, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="S.C.Press", .transducer="", .unit="cmH2O", .phys_min=4.0, .phys_max=20.0, .dig_min=200, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="S.A.StartPress", .transducer="", .unit="cmH2O", .phys_min=4.0, .phys_max=20.0, .dig_min=200, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="S.A.MaxPress", .transducer="", .unit="cmH2O", .phys_min=4.0, .phys_max=20.0, .dig_min=200, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="S.A.MinPress", .transducer="", .unit="cmH2O", .phys_min=4.0, .phys_max=20.0, .dig_min=200, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="S.AFH.StartPress", .transducer="", .unit="cmH2O", .phys_min=4.0, .phys_max=20.0, .dig_min=200, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="S.AFH.MaxPress", .transducer="", .unit="cmH2O", .phys_min=4.0, .phys_max=20.0, .dig_min=200, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="S.AFH.MinPress", .transducer="", .unit="cmH2O", .phys_min=4.0, .phys_max=20.0, .dig_min=200, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        /* [14-21] VAuto settings */
        { .label="S.VA.StartPress", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="S.VA.MaxIPAP", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="S.VA.MinEPAP", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="S.VA.PS", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=20.0, .dig_min=0, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="S.VA.TiMax", .transducer="", .unit="seconds", .phys_min=0.0, .phys_max=4.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        { .label="S.VA.TiMin", .transducer="", .unit="seconds", .phys_min=0.0, .phys_max=4.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        { .label="S.VA.Trigger", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.VA.Cycle", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        /* [22-32] Spont settings */
        { .label="S.S.StartPress", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="S.S.IPAP", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="S.S.EPAP", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="S.S.EasyBreathe", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.S.RespRateEn", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.S.TiMax", .transducer="", .unit="seconds", .phys_min=0.0, .phys_max=4.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        { .label="S.S.TiMin", .transducer="", .unit="seconds", .phys_min=0.0, .phys_max=4.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        { .label="S.S.RiseEnable", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.S.RiseTime", .transducer="", .unit="msec", .phys_min=0.0, .phys_max=2000.0, .dig_min=0, .dig_max=2000, .prefilter="", .samples_per_record=1 },
        { .label="S.S.Trigger", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.S.Cycle", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        /* [33-42] ST settings */
        { .label="S.ST.StartPress", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="S.ST.IPAP", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="S.ST.EPAP", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="S.ST.RespRate", .transducer="", .unit="bpm", .phys_min=0.0, .phys_max=40.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        { .label="S.ST.TiMax", .transducer="", .unit="seconds", .phys_min=0.0, .phys_max=4.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        { .label="S.ST.TiMin", .transducer="", .unit="seconds", .phys_min=0.0, .phys_max=4.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        { .label="S.ST.RiseEnable", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.ST.RiseTime", .transducer="", .unit="msec", .phys_min=0.0, .phys_max=2000.0, .dig_min=0, .dig_max=2000, .prefilter="", .samples_per_record=1 },
        { .label="S.ST.Trigger", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.ST.Cycle", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        /* [43-49] Timed settings */
        { .label="S.T.StartPress", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="S.T.IPAP", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="S.T.EPAP", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="S.T.RespRate", .transducer="", .unit="bpm", .phys_min=0.0, .phys_max=40.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        { .label="S.T.Ti", .transducer="", .unit="seconds", .phys_min=0.0, .phys_max=4.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        { .label="S.T.RiseEnable", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.T.RiseTime", .transducer="", .unit="msec", .phys_min=0.0, .phys_max=2000.0, .dig_min=0, .dig_max=2000, .prefilter="", .samples_per_record=1 },
        /* [50-53] ASV settings */
        { .label="S.AV.StartPress", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="S.AV.EPAP", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="S.AV.MaxPS", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=20.0, .dig_min=0, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="S.AV.MinPS", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=20.0, .dig_min=0, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        /* [54-58] ASVAuto settings */
        { .label="S.AA.StartPress", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="S.AA.MaxEPAP", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="S.AA.MinEPAP", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="S.AA.MaxPS", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=20.0, .dig_min=0, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="S.AA.MinPS", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=20.0, .dig_min=0, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        /* [59-77] Common comfort/settings */
        { .label="S.AS.Comfort", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.RampEnable", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.RampTime", .transducer="", .unit="min.", .phys_min=5.0, .phys_max=45.0, .dig_min=5, .dig_max=45, .prefilter="", .samples_per_record=1 },
        { .label="S.EPR.ClinEnable", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.EPR.EPREnable", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.EPR.Level", .transducer="", .unit="cmH2O", .phys_min=1.0, .phys_max=3.0, .dig_min=50, .dig_max=150, .prefilter="", .samples_per_record=1 },
        { .label="S.EPR.EPRType", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.SmartStart", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.PtAccess", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.ABFilter", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.Mask", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.Tube", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.ClimateControl", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.HumEnable", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.HumLevel", .transducer="", .unit="", .phys_min=1.0, .phys_max=8.0, .dig_min=1, .dig_max=8, .prefilter="", .samples_per_record=1 },
        { .label="S.TempEnable", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.Temp", .transducer="", .unit="Celsius", .phys_min=15.6, .phys_max=30.0, .dig_min=156, .dig_max=300, .prefilter="", .samples_per_record=1 },
        { .label="HeatedTube", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="Humidifier", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        /* [78-91] Environment and oximetry stats */
        { .label="BlowPress.95", .transducer="", .unit="cmH2O", .phys_min=-10.0, .phys_max=45.0, .dig_min=-500, .dig_max=2250, .prefilter="", .samples_per_record=1 },
        { .label="BlowPress.5", .transducer="", .unit="cmH2O", .phys_min=-10.0, .phys_max=45.0, .dig_min=-500, .dig_max=2250, .prefilter="", .samples_per_record=1 },
        { .label="Flow.95", .transducer="", .unit="L/s", .phys_min=-2.0, .phys_max=3.0, .dig_min=-1000, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="Flow.5", .transducer="", .unit="L/s", .phys_min=-2.0, .phys_max=3.0, .dig_min=-1000, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="BlowFlow.50", .transducer="", .unit="L/s", .phys_min=-4.0, .phys_max=4.0, .dig_min=-2000, .dig_max=2000, .prefilter="", .samples_per_record=1 },
        { .label="AmbHumidity.50", .transducer="", .unit="mg/L", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="HumTemp.50", .transducer="", .unit="Celsius", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="HTubeTemp.50", .transducer="", .unit="Celsius", .phys_min=0.0, .phys_max=40.0, .dig_min=0, .dig_max=400, .prefilter="", .samples_per_record=1 },
        { .label="HTubePow.50", .transducer="", .unit="%", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="HumPow.50", .transducer="", .unit="%", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="SpO2.50", .transducer="", .unit="%", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=100, .prefilter="", .samples_per_record=1 },
        { .label="SpO2.95", .transducer="", .unit="%", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=100, .prefilter="", .samples_per_record=1 },
        { .label="SpO2.Max", .transducer="", .unit="%", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=100, .prefilter="", .samples_per_record=1 },
        { .label="SpO2Thresh", .transducer="", .unit="min.", .phys_min=0.0, .phys_max=1440.0, .dig_min=0, .dig_max=1440, .prefilter="", .samples_per_record=1 },
        /* [92-93] Spont trigger/cycle percentages */
        { .label="SpontTrig%", .transducer="", .unit="%", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        { .label="SpontCyc%", .transducer="", .unit="%", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        /* [94-115] Bilevel/ventilation summary stats */
        { .label="MaskPress.50", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=40.0, .dig_min=0, .dig_max=2000, .prefilter="", .samples_per_record=1 },
        { .label="MaskPress.95", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=40.0, .dig_min=0, .dig_max=2000, .prefilter="", .samples_per_record=1 },
        { .label="MaskPress.Max", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=40.0, .dig_min=0, .dig_max=2000, .prefilter="", .samples_per_record=1 },
        { .label="TgtIPAP.50", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=50.0, .dig_min=0, .dig_max=2500, .prefilter="", .samples_per_record=1 },
        { .label="TgtIPAP.95", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=50.0, .dig_min=0, .dig_max=2500, .prefilter="", .samples_per_record=1 },
        { .label="TgtIPAP.Max", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=50.0, .dig_min=0, .dig_max=2500, .prefilter="", .samples_per_record=1 },
        { .label="TgtEPAP.50", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="TgtEPAP.95", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="TgtEPAP.Max", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="Leak.50", .transducer="", .unit="L/s", .phys_min=0.0, .phys_max=2.0, .dig_min=0, .dig_max=100, .prefilter="", .samples_per_record=1 },
        { .label="Leak.95", .transducer="", .unit="L/s", .phys_min=0.0, .phys_max=2.0, .dig_min=0, .dig_max=100, .prefilter="", .samples_per_record=1 },
        { .label="Leak.70", .transducer="", .unit="L/s", .phys_min=0.0, .phys_max=2.0, .dig_min=0, .dig_max=100, .prefilter="", .samples_per_record=1 },
        { .label="Leak.Max", .transducer="", .unit="L/s", .phys_min=0.0, .phys_max=2.0, .dig_min=0, .dig_max=100, .prefilter="", .samples_per_record=1 },
        { .label="MinVent.50", .transducer="", .unit="L/min", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=240, .prefilter="", .samples_per_record=1 },
        { .label="MinVent.95", .transducer="", .unit="L/min", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=240, .prefilter="", .samples_per_record=1 },
        { .label="MinVent.Max", .transducer="", .unit="L/min", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=240, .prefilter="", .samples_per_record=1 },
        { .label="RespRate.50", .transducer="", .unit="bpm", .phys_min=0.0, .phys_max=90.0, .dig_min=0, .dig_max=450, .prefilter="", .samples_per_record=1 },
        { .label="RespRate.95", .transducer="", .unit="bpm", .phys_min=0.0, .phys_max=90.0, .dig_min=0, .dig_max=450, .prefilter="", .samples_per_record=1 },
        { .label="RespRate.Max", .transducer="", .unit="bpm", .phys_min=0.0, .phys_max=90.0, .dig_min=0, .dig_max=450, .prefilter="", .samples_per_record=1 },
        { .label="TidVol.50", .transducer="", .unit="L", .phys_min=0.0, .phys_max=4.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        { .label="TidVol.95", .transducer="", .unit="L", .phys_min=0.0, .phys_max=4.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        { .label="TidVol.Max", .transducer="", .unit="L", .phys_min=0.0, .phys_max=4.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        /* [116-118] Target minute ventilation */
        { .label="TgtVent.50", .transducer="", .unit="L/min", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=240, .prefilter="", .samples_per_record=1 },
        { .label="TgtVent.95", .transducer="", .unit="L/min", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=240, .prefilter="", .samples_per_record=1 },
        { .label="TgtVent.Max", .transducer="", .unit="L/min", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=240, .prefilter="", .samples_per_record=1 },
        /* [119-121] I:E ratio */
        { .label="IERatio.50", .transducer="", .unit="%", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=100, .prefilter="", .samples_per_record=1 },
        { .label="IERatio.95", .transducer="", .unit="%", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=100, .prefilter="", .samples_per_record=1 },
        { .label="IERatio.Max", .transducer="", .unit="%", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=100, .prefilter="", .samples_per_record=1 },
        /* [122-124] Inspiratory duration */
        { .label="Ti.50", .transducer="", .unit="seconds", .phys_min=0.0, .phys_max=4.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        { .label="Ti.95", .transducer="", .unit="seconds", .phys_min=0.0, .phys_max=4.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        { .label="Ti.Max", .transducer="", .unit="seconds", .phys_min=0.0, .phys_max=4.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        /* [125-132] Indices and CSR */
        { .label="AHI", .transducer="", .unit="", .phys_min=0.0, .phys_max=240.0, .dig_min=0, .dig_max=2400, .prefilter="", .samples_per_record=1 },
        { .label="HI", .transducer="", .unit="", .phys_min=0.0, .phys_max=240.0, .dig_min=0, .dig_max=2400, .prefilter="", .samples_per_record=1 },
        { .label="AI", .transducer="", .unit="", .phys_min=0.0, .phys_max=240.0, .dig_min=0, .dig_max=2400, .prefilter="", .samples_per_record=1 },
        { .label="OAI", .transducer="", .unit="", .phys_min=0.0, .phys_max=240.0, .dig_min=0, .dig_max=2400, .prefilter="", .samples_per_record=1 },
        { .label="CAI", .transducer="", .unit="", .phys_min=0.0, .phys_max=240.0, .dig_min=0, .dig_max=2400, .prefilter="", .samples_per_record=1 },
        { .label="UAI", .transducer="", .unit="", .phys_min=0.0, .phys_max=240.0, .dig_min=0, .dig_max=2400, .prefilter="", .samples_per_record=1 },
        { .label="RIN", .transducer="", .unit="", .phys_min=0.0, .phys_max=240.0, .dig_min=0, .dig_max=2400, .prefilter="", .samples_per_record=1 },
        { .label="CSR", .transducer="", .unit="", .phys_min=0.0, .phys_max=1440.0, .dig_min=0, .dig_max=1440, .prefilter="", .samples_per_record=1 },
};

#ifdef __cplusplus
}
#endif
