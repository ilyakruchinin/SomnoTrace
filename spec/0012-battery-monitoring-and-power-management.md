# 0012 — Battery Monitoring & Power Management

- **Status:** Implemented
- **Author(s):** Ilya Kruchinin (@ilyakruchinin)
- **Created:** 2026-09-04
- **Last updated:** 2026-09-04
- **Related specs:** `0007-button-controls.md`, `0009-web-interface.md`, `0011-web-api-endpoints.md`

## 1. Summary

This specification defines the battery voltage measurement, state-of-charge (SoC) estimation, hardware charger detection, uncalibrated state machine, warm-reboot persistence, dynamic CC/CV slew rate limiting, and user interface presentation for the SomnoTrace firmware running on the Waveshare ESP32-S3-Touch-LCD-1.54 with an attached single-cell lithium-ion (Li-ion / LiPo) battery.

## 2. Motivation / goals

- **Accurate and Honest Gauge:** Provide a trustworthy battery percentage on both the onboard LCD and Web Portal that reflects true chemical charge without erratic jumping, sudden drops upon plugging in a charger, or false 100% readings on reboot.
- **Physics-Based CC/CV Charging Model:** Accurately mirror the physical behavior of the onboard ETA6098 switching charger, allowing rapid progress during Constant Current (CC) mode while realistically slowing down during the Constant Voltage (CV) saturation phase.
- **Fast Interactive Response:** Debounce and reflect USB cable insertion and removal on the display within 2–3 seconds rather than tens of seconds.
- **Clean Handling of Unknown State:** When cold-booting on a charger in the ambiguous float zone ($V_{BAT} \ge 4100\text{ mV}$) without prior history, display an honest `--%` (Calibrating) indicator instead of guessing or jumping to a false 100%.
- **Zero-Lag Unplug Snap:** When unplugging from an uncalibrated state, wait 10 seconds for electrochemical surface charge to relax, then snap directly to the genuine open-circuit voltage (OCV) without entering a multi-minute downward crawl.
- **Hardware-Preserved Persistence:** Retain the calibrated percentage across warm software restarts, watchdog resets, and USB firmware flashing via ESP32-S3 RTC Fast SRAM (`RTC_NOINIT_ATTR`).
- **Support for Battery-less Operation:** Allow users running purely on USB power without a connected battery to disable the battery gauge cleanly via Device Settings.

## 3. Non-goals

- Hardware fuel-gauge integration (the device does not include a dedicated I2C Coulomb-counter IC; state estimation relies on factory-calibrated ADC measurements and modeled OCV curves).
- Multi-chemistry support (parameters are calibrated specifically for 3.7 V nominal / 4.2 V max Li-ion/LiPo cells).

---

## 4. Behaviour

### 4.1 Hardware Architecture & Power Latch

- **Board:** Waveshare ESP32-S3-Touch-LCD-1.54.
- **Power Latch (`BAT_EN`, GPIO2):** Driven high at boot to maintain system power via the onboard PMOS switch. Releasing this pin (driven low or floating) unlatches power when operating on battery.
- **Charger Status (`CHG_STAT`, GPIO3):** Open-drain output from the ETA6098 switching charger IC with internal/external pull-up. Pulled low by the charger IC during active charging; floats high when charging is complete or disconnected.
- **Battery ADC (`BAT_ADC`, GPIO1 / ADC1_CH0):** Connected to the battery terminal through a 2:1 resistive divider ($200\text{ k}\Omega / 100\text{ k}\Omega$, total attenuation $\times 3$).
- **ADC Calibration:** Utilizes ESP-IDF eFuse-backed line calibration (`esp_adc_cali`) with `ADC_ATTEN_DB_12` to eliminate per-chip full-scale gain errors.

### 4.2 ADC Sampling & Burst Filtering

- **Sample Burst:** 256 consecutive ADC readings spread across ~1 second (4 ms inter-sample delay). Spreading across 1 second prevents momentary Wi-Fi or BLE radio transmit bursts (which dip the supply rail for several hundred microseconds) from biasing the entire measurement.
- **Trimmed Mean:** The lowest 1/8th and highest 1/8th of samples are discarded before computing the arithmetic mean, eliminating spike outliers.
- **Infinite Impulse Response (IIR) Filter:** Consecutive bursts are smoothed with a low-pass filter:
  $$V_{filtered} \leftarrow V_{filtered} + \frac{V_{OCV} - V_{filtered}}{4}$$
- **Base Sampling Cadence:** ADC bursts run on a consistent 10-second cadence (`BAT_SAMPLE_PERIOD_S = 10`), keeping voltage tracking and charger detection responsive.

### 4.3 Open-Circuit Voltage (OCV) & Adaptive Full-Charge Anchor

- **OCV Piecewise Table:** Terminal voltage during battery discharge is mapped to state-of-charge percentage via linear interpolation:
  | Voltage ($mV$) | Percentage (%) | Operating Phase |
  | :--- | :--- | :--- |
  | 4160 | 100% | Full Float Ceiling |
  | 4100 | 90% | Upper Slope |
  | 3950 | 75% | Upper Plateau |
  | 3850 | 60% | Mid Plateau |
  | 3750 | 45% | Mid Plateau |
  | 3650 | 30% | Lower Plateau |
  | 3550 | 15% | Lower Knee |
  | 3450 | 5% | Low-Battery Warning |
  | 3300 | 0% | Cutoff Threshold |

- **IR Drop Compensation:** When charging is active, an empirical 30 mV offset is subtracted from the measured terminal voltage before evaluating the OCV curve to account for the internal cell resistance ($R_i \approx 60\text{ m}\Omega$ at 500 mA charge current).
- **Adaptive 100% Anchor:** The top of the curve adapts to the specific hardware charger and cell float voltage. If a charge session completes and the relaxed voltage rests between 4000 mV and 4160 mV, the learned termination voltage is persisted to NVS (`bat.full_mv`) to ensure the gauge reaches 100% on subsequent cycles.

### 4.4 Charger Debouncing & Unplug Relaxation

- **2-Second Edge Debounce:** Consecutive readings of `CHG_STAT` must remain steady for 2 seconds (`BAT_DEBOUNCE_SEC = 2`) before committing a charger connected or disconnected state transition.
- **10-Second Unplug Relaxation Window:** Upon debounced charger disconnection, the cell enters an electrochemical settling state (`BAT_UNPLUG_SETTLE_S = 10`). During these 10 seconds:
  - The charging bolt is immediately extinguished on display.
  - The displayed percentage is frozen, preventing surface overvoltage decay from causing false reading adjustments.
  - The monitor task sleeps for the remaining settle time rather than the full sample period.

### 4.5 Warm-Reboot & Flash Persistence (`RTC_NOINIT_ATTR`)

- A 12-byte backup structure is allocated in ESP32-S3 RTC Fast SRAM:
  ```c
  typedef struct {
      uint32_t magic;        /* 0x534E5442 ('SNTB') */
      int16_t  shown_pct;    /* displayed percentage (0..100) */
      int16_t  filtered_mv;  /* filtered millivolts */
      uint32_t crc;          /* XOR CRC verification */
  } rtc_bat_backup_t;
  ```
- **Preservation:** Annotated with `RTC_NOINIT_ATTR` (`.rtc_noinit (NOLOAD)`). Unlike standard `.rtc.data` or `.rtc.bss`, this section is **not** cleared by the C runtime startup code across reboots.
- **Validation:** Protected by the magic constant `0x534E5442` and a 16-bit CRC. True cold power-on (battery reconnection) leaves random uninitialized SRAM that fails validation, resetting the backup safely. Software restarts, watchdog resets, and USB firmware flashing preserve the data, restoring `shown_pct` and maintaining calibration without jumping to `--%` or 100%.

### 4.6 Uncalibrated (`--%`) State Machine

```
               [ Cold Boot / Invalid RTC ]
                           |
            +--------------+--------------+
            |                             |
      [ On Battery ]               [ On Charger ]
            |                             |
    Sample OCV Voltage             V_BAT >= 4100 mV?
            |                             |
     is_calibrated = true        +--------+--------+
     shown_pct = target_pct      |                 |
     Display: [X%]             (Yes)              (No)
                                 |                 |
                       is_calibrated = false  is_calibrated = true
                       shown_pct = -1         shown_pct = target_pct
                       Display: [--% ⚡]       Display: [X% ⚡]
                                 |
                 +---------------+---------------+
                 |                               |
        [ Unplug Charger ]             [ Charge Completes ]
                 |                               |
        Hold [--%] for 10 s             V_BAT >= 4140 mV & CHG off
                 |                               |
        Sample Relaxed OCV              shown_pct = 100%
                 |                      is_calibrated = true
        shown_pct = target_pct          Display: [100%]
        is_calibrated = true
        Display: [X%]
```

- When booting on a charger in the constant-voltage float zone ($V_{BAT} \ge 4100\text{ mV}$) without prior RTC history, the gauge enters the uncalibrated state (`is_calibrated = false`, `shown_pct = -1`).
- The LCD renders `--%` in muted slate-blue (`rgb565(160, 180, 205)`) next to the charging bolt.
- The Web Portal Status tab renders `--% (X.XXV) ⚡ (Calibrating)`.
- If unplugged before full charge, it holds `--%` for 10 seconds of electrochemical relaxation, then snaps directly to the relaxed open-circuit voltage percentage, setting `is_calibrated = true` and engaging normal guardrails.
- If left plugged in until the charger IC stops, it locks to `100%` and sets `is_calibrated = true`.

### 4.7 Dynamic CC/CV Charging Slew Rate

To match the physical charging curve of the ETA6098 switching charger, `shown_pct` increases by at most 1% after an elapsed duration dictated by the current charge level:

| Percentage Range | Charging Mode | Min Delay per +1% | Physical Rationale |
| :--- | :--- | :--- | :--- |
| **0% – 69%** | Constant Current (CC) | **35 seconds** | Full 500 mA charge rate; rapid, linear accumulation |
| **70% – 84%** | Early CV Transition | **60 seconds** | Voltage reaches 4.15 V; current begins tapering |
| **85% – 94%** | Deep CV Absorption | **120 seconds** (2 min) | Current decays to ~150–200 mA |
| **95% – 99%** | Trickle Saturation | **180 seconds** (3 min) | Current drops below 100 mA |
| **100%** | Charge Termination | **Immediate Snap** | Triggered when `!debounced_charging && filtered_mv >= 4140` |

- **Direction Lock:** While charging, `shown_pct` is strictly non-decreasing ($\text{shown\_pct}_{t+1} \ge \text{shown\_pct}_t$).

### 4.8 Two-Tier Discharge Guardrail

When operating on battery power, `shown_pct` decreases by at most 1% after an elapsed duration:

| Percentage Range | Min Delay per -1% | Physical Rationale |
| :--- | :--- | :--- |
| **20% – 100%** | **30 seconds** | Dampens radio transmission dips while tracking real drain (~120–180s per 1%) |
| **0% – 19%** | **15 seconds** | Responsive tracking through the steep discharge knee before PMU 3.3 V shutdown |

- **Direction Lock:** While discharging, `shown_pct` is strictly non-increasing ($\text{shown\_pct}_{t+1} \le \text{shown\_pct}_t$).

### 4.9 User Interface & Telemetry Contracts

- **Onboard Status Screen:**
  - **Frame:** 22px wide × 14px tall outline with a 3×6px terminal nub.
  - **Position:** Anchored at `x = 118, y = 12`, aligning flush with the baseline of the top status bar icons.
  - **Charging Bolt:** 11px tall × 8px wide bold lightning bolt in vivid yellow (`#FFCC00`).
  - **Digit Colors:** Green (`> 30%`), Orange (`16% – 30%`), Red (`0% – 15%`), Muted Slate-Blue (`--%` calibrating).
- **Web Portal Telemetry (`/api/status`):**
  - Exposed under the `"battery"` object:
    - `percent`: Integer 0..100, or `-1` when calibrating, or `null` if disabled/invalid.
    - `millivolts`: Integer raw ADC millivolts, or `null` if disabled/invalid.
    - `charging`: Boolean charger active flag.
    - `valid`: Boolean battery enabled and operational flag.
- **Device Settings (`/api/device/settings`):**
  - Controlled via the `battery_enabled` boolean.
  - When set to `false`, the battery indicator is removed from the LCD status screen, and the Web Portal renders `USB Power (No battery)`.

---

## 5. Acceptance criteria

- [x] Battery ADC sampling uses a 256-sample burst over 1s with trimmed mean and IIR filtering.
- [x] Physical charger connect/disconnect is debounced in 2 seconds.
- [x] Displaying `--%` when cold-booting on a charger in the $\ge 4100\text{ mV}$ CV float zone.
- [x] Unplugging from `--%` holds for 10 seconds of relaxation then snaps directly to true relaxed OCV.
- [x] Warm reboots and USB firmware flashing preserve battery percentage using `RTC_NOINIT_ATTR`.
- [x] Charging slew rate slows dynamically through CC and CV phases (35s $\rightarrow$ 60s $\rightarrow$ 120s $\rightarrow$ 180s).
- [x] Discharging slew rate allows 30s steps normally and 15s steps below 20%.
- [x] Charge completion locks immediately to 100% when the hardware charger IC turns off above 4140 mV.
- [x] Setting `battery_enabled = false` cleanly hides the LCD indicator and reports `USB Power (No battery)` in the portal.

## 6. Security / privacy considerations

Battery telemetry does not contain personal health data or patient identifiers.

## 7. Open questions

None.

## 8. Changelog

- 2026-09-04: Initial specification documenting implemented battery monitoring, power management, OCV calibration, dynamic CC/CV slew rate, and RTC persistence architecture.
