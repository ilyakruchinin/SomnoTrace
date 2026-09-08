# 0013 — Sleep Staging Algorithms (CPAP-Only, Oximetry-Only, Fused)

- **Status:** Implemented
- **Author(s):** Ilya Kruchinin (@ilyakruchinin)
- **Created:** 2026-09-07
- **Last updated:** 2026-09-07
- **Related specs:** `0002-edf-export.md`, `0009-web-interface.md`, `0011-web-api-endpoints.md`

## 1. Summary

This specification defines the three sleep-staging methods implemented in the
SomnoTrace web portal — **CPAP-only** (ResMed AirSense 11 periodic respiratory
data), **oximetry-only** (Wellue O₂ Ring pulse oximetry), and **fused**
(both sensors) — including their features, decision rules, and formulas. It
also documents the clean-room provenance of the approaches: the published
clinical literature they build on, and the design discoveries made before the
final approaches were selected.

## 2. Motivation / goals

- Provide a single normative reference for the staging behaviour shipped in
  `main/portal.html`, so future changes can be diffed against a spec.
- Record the provenance of every physiological assumption for clean-room
  (independent-implementation) purposes.
- Make the validation scope explicit: results are established on a 14-night
  single-patient cohort with watch-derived ground truth; cross-patient
  generalisation is supported by leave-one-night-out (LONO) folds only.

## 3. Non-goals

- Clinical diagnosis. Outputs are wellness-grade estimates, not medical
  diagnoses, and must never be presented as such.
- AASM-compliant hypnogram scoring (N1/N2/N3/REM with EEG). The engine
  produces a 4-class approximation (Wake / Light / Deep / REM) from
  autonomic and respiratory signals only.
- Apnea/desaturation event detection and AHI computation (separate concern;
  the event-density exploration in `.ai/` remains a non-shipped candidate).

## 4. Behaviour

### 4.1 Common definitions

All three methods operate on fixed **30-second epochs**. Stage codes:

| Code | Stage | Notes |
|---|---|---|
| 0 | Deep (N3) | Slow-wave sleep |
| 1 | Light (N1/N2) | Default non-REM stage |
| 2 | REM | Rapid eye movement sleep |
| 3 | Wake | Including device-off / uninterpretable epochs |

**Robust night-adaptive z-score.** All threshold features are expressed as
robust z-scores computed against the night's own worn-window distribution:

```
z(x) = (x − median(X)) / max(1e-4, IQR(X) / 1.349)
```

where `X` is the set of feature values inside the sleep span `[eOnset, eOffset]`
and 1.349 is the normal-consistency constant (IQR ≈ 1.349σ for Gaussian data).

**3-epoch smoothing window.** Windowed variants of per-epoch features use a
centred 90-second (3-epoch) mean: `win_f(e) = (f(e−1) + f(e) + f(e+1)) / 3`.

**Sampling-rate adaptation.** Pulse/motion features are normalised to their
1 Hz-equivalent so oximeters with slower cadence (e.g. 4 s) stage with the
same thresholds. With `dtSec = clamp(median(Δt), 1, 30)` in seconds:

```
rmssd     ← rmssd · (1/dtSec)^0.5      (successive-difference HRV grows ~√Δt)
sumMotion ← sumMotion · dtSec          (epoch sums grow ~linearly with samples)
validCntMin = max(3, min(5, round(5/dtSec)))
```

At 1 Hz every scale factor is exactly 1, so behaviour is unchanged.
`maxMotion` (per-sample max) and all z-scores are rate-invariant and are not
scaled. Measured on the validation cohort: 4 s decimation stages within
+0.09 pp of the 1 Hz baseline (oximetry-only) and +0.53 pp in fused mode.

**Sleep onset/offset gating.** `eOnset` is the start of the first run of ≥ 6
consecutive epochs passing the mode's wear screen; `eOffset` is the last such
run (searched backwards), clamped to `eOffset ≥ eOnset + 10`. Circadian
progress is `prog(e) = (e − eOnset) / (eOffset − eOnset)`.

**Minimum-bout merger.** A deterministic 2-pass bout merger is applied to the
epoch sequence: bouts of Deep or Wake shorter than 2 epochs are replaced by
the surrounding majority stage (or Light if the neighbours disagree); bouts of
Light or REM shorter than 6 epochs (180 s) are replaced by the longer
neighbouring bout's stage. After merging, a direct Deep→REM transition is
rewritten as Light (biological constraint: N3→REM never occurs directly).

**Outputs.** Per recording: sample-aligned stage array, TST, per-stage
minutes and percentages of TST, wake minutes, efficiency, and a source tag
(`fused` / `oxi` / `cpap`).

### 4.2 Oximetry-only method

**Inputs.** Per-sample pulse rate (BPM) and accelerometer motion from the
O₂ Ring; 30 s epochs with ≥ `validCntMin` valid pulse samples (§4.1).

**Per-epoch features.**

- `meanPulse` — mean pulse over valid samples (30 < BPM < 220).
- `rmssd` — root-mean-square of successive pulse differences:
  `rmssd = sqrt( Σ (pᵢ − pᵢ₋₁)² / (n − 1) )` (pulse-domain proxy for HRV),
  normalised to 1 Hz-equivalent per §4.1.
- `maxMotion`, `sumMotion` — max and sum of per-sample motion (`sumMotion`
  normalised to the 1 Hz-equivalent epoch sum, §4.1).
- `winRmssd(e)` — 3-epoch mean of `rmssd`.

**Baseline pulse (night nadir).** `basePulse` = the minimum, over all rolling
20-epoch (10-minute) windows with ≥ 60% still-epoch coverage, of the median
`meanPulse` over still epochs (motion ≤ 10). Fallback: median over all still
epochs; final fallback 60 BPM. This tracks the nocturnal heart-rate nadir
that physiologically occurs in deep sleep (see §7.1).

**Wake screening.** An epoch is hard-Wake if pulse is invalid,
`validCnt < validCntMin`, `maxMotion > 25`, or `sumMotion > 60` (actigraphy
wake rules per §7.1), or if
it lies outside `[eOnset, eOffset]`.

**Autonomic wake gate (Step 4).** In oximetry-only mode, an epoch is
additionally classified Wake when all three autonomic arousal markers hold
simultaneously:

```
isAutoWake = (maxMotion > 6) ∧ (meanPulse > basePulse + 5.0) ∧ (winRmssd > 1.2)
```

i.e. movement above rest, pulse elevated ≥ 5 BPM above the night nadir, and
elevated beat-to-beat variability — the autonomic signature of a movement
arousal. The gate is disabled in fused mode (CPAP data disambiguates).

**Deep (N3) detection — hysteresis state machine.** With
`pDiff = meanPulse − basePulse`, `zP = z(pDiff)`, and a circadian threshold
`pThresh = 1.6 × (1.3 if prog < 0.35; 1.0 if prog < 0.65; else 0.85)`:

- Enter Deep when: `stillnessRun ≥ 2` (stillness = `maxMotion ≤ 2 ∧ sumMotion ≤ 4`),
  `maxMotion ≤ 2`, `prog < 0.80`, dip condition
  (`zP ≤ −0.8 ∨ pDiff ≤ pThresh`), and `winRmssd ≤ 1.05`.
- Exit Deep when: `maxMotion > 3 ∨ sumMotion > 10 ∨ (zP > −0.2 ∧ pDiff > pThresh + 1.2)`.

Rationale: deep sleep exhibits the lowest and most stable heart rate of the
night with suppressed HRV (§7.1); the dip threshold adapts to circadian
position because N3 concentrates in the first third of the night.

**REM detection — continuous score, seed & expand.** For non-Deep epochs with
`prog ≥ 0.15` and `maxMotion ≤ 4`:

```
score = 0.60 · zP + 0.40 · zR        (zR = z(winRmssd))
if (zP > 1.5 ∧ zR < 0.2) score ×= 0.5    (REM-calibration guard)
```

REM candidates are runs of `score ≥ 0.85` of length ≥ 3 epochs (or ≥ 2 if the
run's peak score ≥ 1.1), expanded into neighbours with `score ≥ 0.20`.
Rationale: REM combines elevated, variable pulse (sympathetic activation)
with elevated HRV (§7.1); the calibration term suppresses the movement-arousal
pulse pattern that otherwise mimics REM.

**Post-processing.** 2-pass minimum-bout merger (§4.1), Deep→REM rewrite.

### 4.3 CPAP-only method

**Inputs.** Per-sample CPAP periodic respiratory data from the AirSense 11
PLD stream: respiratory rate (RR), tidal volume (TV), leak, flow-limitation
score (FL), snore. Epochs require ≥ 3 valid RR samples.

**Per-epoch features.**

- `meanRr`, `sdRr` — mean/SD of valid RR (4 < RR < 60).
- `meanTv`, `sdTv`, `cvTv = sdTv / meanTv` (meanTv > 0.05) — tidal-volume
  variability (breathing regularity).
- `maxLeak` — max leak (mask seal quality).
- `meanFl`, `maxFl` — mean/max flow-limitation score.
- Windowed: `winCvTv`, `winSdRr`, `winFl` (3-epoch means).

**Wear gating.** `eOnset`/`eOffset` = first/last run of ≥ 6 epochs with
`cnt ≥ 3` valid RR samples. Epochs with `cnt < 3`, `maxLeak > 45` sustained
for ≥ 3 epochs, or invalid RR are hard-Wake (device off / mask off).

**Night-adaptive z-scores.** `zCv = z(winCvTv)`, `zRr = z(winSdRr)` against
the worn-window robust statistics.

**Classification rules (per epoch).**

```
stillnessRun: consecutive epochs with zCv ≤ 0.1 ∧ zRr ≤ 0.1

isDeep = stillnessRun ≥ 2 ∧ prog < 0.60 ∧ (zCv + zRr ≤ −0.3)
SWS immunity: if (winFl ≥ 0.15 ∨ maxFl ≥ 0.22) then isDeep = false

isRem = prog > 0.18 ∧ (zCv ≥ 1.35 ∨ zRr ≥ 1.35)

stage = isDeep ? Deep : (isRem ? REM : Light)
```

Rationale: N3 is the quadrant of minimal respiratory variability (regular,
high-amplitude breathing driven by strong parasympathetic tone); REM is the
quadrant of maximal respiratory variability (breathing becomes irregular and
chemoreflex control is attenuated in REM, §7.1). The SWS-immunity gate
implements the flow-limitation criterion of §7.2: upper-airway
collapse/flow-limitation markers are physiologically incompatible with
stable slow-wave sleep, so any epoch bearing them is barred from Deep.

**Post-processing.** 2-pass minimum-bout merger (minBout = 6, consistent with
the fused path), Deep→REM rewrite.

### 4.4 Fused method

**Inputs.** Oximetry (pulse, motion) plus ≥ 10 CPAP epochs overlapping the
recording (`isFused`), with per-epoch CPAP features joined where the PLD
stream covers the epoch (`hasCpap`).

**Wake screening.** As §4.2, plus: in fused mode, `maxLeak > 45` sustained
for ≥ 3 epochs forces Wake (mask leak makes pulse features unreliable). The
autonomic wake gate (§4.2) is **disabled** in fused mode.

**Deep detection.** As §4.2 (pulse/HRV hysteresis), with two fused guards:

- Entry additionally requires `winCvTv ≤ 0.22 ∧ zCv ≤ 0.8`
  (respiratory variability must also be low).
- Exit additionally triggers on `winCvTv > 0.24 ∨ zCv > 1.2`.

**REM scoring.** For non-Deep, non-Wake epochs with `prog ≥ 0.15` and
`maxMotion ≤ 4`, the fused score blends respiratory and autonomic evidence:

```
fused:   score = 0.40 · zCv + 0.25 · zRr + 0.25 · zP + 0.10 · zR
oxi-only: score = 0.60 · zP + 0.40 · zR   (with the §4.2 calibration term)
```

Candidate seeding and expansion thresholds are identical to §4.2
(seed ≥ 0.85, run ≥ 3 epochs, expand ≥ 0.20).

**Mode selection.** `isFused = (CPAP epochs ≥ 10)`; otherwise the pipeline
falls back to oximetry-only behaviour on the same recording.

### 4.5 Error handling & edge cases

- Recordings with < 60 samples or < 6 epochs are not staged.
- Recording gaps (no samples in an epoch) stage as Wake.
- Multi-session nights are staged per recording; per-night aggregates sum
  session statistics (the divergence from per-night staging is accepted and
  documented in the benchmark harness).
- All thresholds are night-adaptive (robust z-scores) except the fixed
  physiological constants listed in §4.2–4.4; no per-user configuration is
  required.

## 5. Acceptance criteria

Validated on a 14-night single-patient cohort with consumer-watch
ground truth; full evidence chain in `.ai/telegram/IMPROVE.md` §28.

- [x] Overall epoch-share MAE (Deep/Light/REM vs GT): Fused ≤ 5.0%,
      CPAP-only ≤ 5.5%, Oximetry-only ≤ 5.5% (measured: 4.71 / 5.05 / 4.71)
- [x] Per-stage MAE gates: Deep < 5.0%, Light < 7.0%, REM < 5.0% for every
      mode (all pass)
- [x] Transition counts: Fused ≤ 30, CPAP-only ≤ 50, Oximetry ≤ 30 per night
- [x] Mean REM share within 11–19% cohort band; per-night REM within the
      clinical 5–25% band in 14/14 nights for all three modes
- [x] LONO out-of-sample overall MAE ≤ 5.5% for all modes
- [x] Shipped-code parity: benchmarks execute functions extracted live from
      `main/portal.html`; reference copy and portal agree
- [x] Determinism: same input bytes → same stage output (no RNG, no floats
      beyond IEEE-754 deterministic ops)
- [x] Sampling-rate robustness: 1 Hz output bit-identical to the validated
      engine; 4 s-decimated input stages within +0.15 pp of the 1 Hz baseline
      in oximetry-only mode (measured +0.09 pp)

## 6. Security / privacy considerations

- Staging operates on already-collected device data; no network egress.
- Sleep-stage outputs are health-adjacent personal data: they inherit the
  EDF/SNT handling rules of specs `0002`/`0003` (local storage, never
  transmitted without explicit user-configured upload).
- No real patient data may be embedded in tests or fixtures.

## 7. Clean-room provenance

### 7.1 Published clinical/physiological methods relied upon

The staging approaches are built exclusively from publicly documented
physiology and published methods. No third-party source code was consulted,
copied, or translated for the staging engine (clean-room rule:
`CONTRIBUTING.md`; vendored code is limited to `third_party/` and unrelated
to staging).

- **Epoch-based stage scoring.** 30-second epoch scoring with Wake/NREM/REM
  categories follows the public AASM scoring manual conventions (Silber et
  al. 2007, *Sleep*; AASM Scoring Manual v2.x), which itself consolidated
  Rechtschaffen & Kales (1968). SomnoTrace scores 3 classes (Deep, Light,
  Wake) plus REM from autonomic/respiratory proxies; it does not implement
  EEG scoring.
- **Actigraphy wake rules.** Motion-threshold wake screening descends from
  published actigraphy scoring: Cole & Kripke et al. 1992, "Automatic
  sleep/wake identification from wrist activity," *Sleep* 15(5):461–469,
  and Sadeh et al. 1994. The specific thresholds (`maxMotion > 25`,
  `sumMotion > 60`) were re-derived on SomnoTrace's own data.
- **Autonomic correlates of sleep stages.** The Deep detector (pulse nadir +
  suppressed beat-to-beat variability) and REM detector (elevated, variable
  pulse) rest on the well-replicated literature that deep sleep shows the
  highest and most stable parasympathetic activity while REM shows elevated
  sympathetic tone and HRV (Berlad et al. 1993; Penzel et al. 2003; and the
  PPG sleep-staging literature generally). All thresholds were tuned de novo
  on SomnoTrace's own cohort.
- **Flow limitation from CPAP flow contour.** The CPAP-only features and the
  SWS-immunity gate follow Condos, Norman, Krishnasamy, Peduzzi, Goldring &
  Rapoport, "Flow Limitation as a Noninvasive Assessment of Residual
  Upper-Airway Resistance During Continuous Positive Airway Pressure Therapy
  of Obstructive Sleep Apnea," *Am J Respir Crit Care Med* 150(2):475–480
  (1994), doi:10.1164/ajrccm.150.2.8049832 — inspiratory flow contour from a
  conventional CPAP circuit noninvasively indicates elevated upper-airway
  resistance (flattening/plateau). SomnoTrace uses the device's own numeric
  flow-limitation channel rather than reimplementing contour analysis.
- **Respiratory-event physiology.** The (non-shipped, §30.2) obstructive
  event rule (`winFl ≥ 0.22` OR `snore ≥ 1.0 ∧ winFl ≥ 0.12`) follows the
  AASM hypopnea framework (≥ 30% flow reduction with arousal/desaturation)
  adapted to the AirSense 11's normalised marker channels.
- **Robust z-scores.** Night-adaptive normalisation uses the standard robust
  z-score with the 1.349 IQR-to-σ consistency constant (Gaussian efficiency
  of the median/IQR), a textbook technique.

### 7.2 Design discoveries preceding the final approaches

The following findings (full audit trail in `.ai/telegram/IMPROVE.md`) shaped
the shipped approaches; each was measured, not assumed:

1. **REM over-call from movement arousals.** Pulse elevation alone mimics
   REM; the autonomic calibration term (`zP > 1.5 ∧ zR < 0.2 → score × 0.5`)
   removed a systematic +2 pp REM bias (§10, §14).
2. **SWS immunity to flow limitation.** Upper-airway collapse markers
   (`winFl`, `maxFl`) systematically mislabel N3 epochs as Deep; the
   immunity gate (§4.3) removed the largest single Deep error source (§24).
3. **Autonomic wake gate.** A triple condition (motion ∧ pulse-Δ ∧ RMSSD)
   is required; single-marker gates over-fire. A 100-point sensitivity sweep
   showed the shipped operating point sits at the optimum of a flat basin
   (5.05% MAE; only 1/100 neighbours better, by −0.03 pp) — §30.4.
4. **Viterbi/AASM and HSMM decoders rejected.** Feeding the rule engine's
   per-epoch decisions as emissions into a Viterbi trellis (Design A) or a
   duration-constrained HSMM (Design B) degraded overall MAE to 5.60% /
   5.17% vs the shipped 4.26%: hard emissions discard the sequential context
   (`stillnessRun`, circadian progress) that the deterministic 2-pass merger
   exploits (§26.3, §30.3).
5. **Event-density feature deferred.** A rolling obstructive-event density
   term is neutral on unfragmented nights (fires on 1.0% of epochs; −0.12 pp,
   within rounding noise) and is retained as a candidate for highly
   fragmented datasets only (§19.1.1, §30.2).
6. **Night-adaptive robust statistics over fixed thresholds.** Fixed
   absolute thresholds failed across nights; median/IQR z-scores against the
   night's own worn window made every mode self-calibrating (§9).

## 8. Security / privacy considerations

See §6. Sleep-stage data is derived health data; it is processed on-device,
stored only in local recordings/EDF exports, and leaves the device only via
the user-configured SMB/SleepHQ uploads (specs `0004`/`0005`).

## 9. Open questions

- Cross-patient validation of all thresholds (especially the autonomic wake
  gate and the fixed physiological constants) awaits the first
  externally-contributed multi-night dataset; re-run the sensitivity and
  LONO harnesses on it (§30.4).
- Event-density integration for fragmented datasets (§30.2) — parameters
  validated, not shipped.

## 10. Changelog

- 2026-09-07: Initial draft; documents the shipped algorithms, validation
  gates, and clean-room provenance.
- 2026-09-08: Added sampling-rate adaptation (§4.1): rate-dependent pulse/
  motion features normalised to 1 Hz-equivalent (RMSSD √Δt law, sumMotion
  linear law, scaled validCnt floor). 1 Hz behaviour unchanged (verified
  bit-identical); 4 s cadence recovers from 10.57% to 5.14% overall MAE.
