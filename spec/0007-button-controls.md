# 0007 — Button controls & SoftAP entry

- **Status:** Accepted
- **Author(s):** Ilya Kruchinin (@ilyakruchinin)
- **Created:** 2026-06-17
- **Last updated:** 2026-06-17
- **Related specs:** `0006-device-provisioning.md`

## 1. Summary

Defines the physical-button interaction model for the device, targeting the
**non-touch** hardware variant where all UI navigation must be driven by the
three onboard buttons. Navigation is short-press only; long-presses are
reserved exclusively for system actions (power off, Wi-Fi config entry).

## 2. Motivation / goals

- Provide a predictable, learnable UI without a touchscreen.
- Avoid overloading a single button with multiple short/long meanings, which
  testing/feedback found confusing.
- Reserve a deliberate, hard-to-trigger gesture for entering Wi-Fi (SoftAP)
  configuration mode.

## 3. Non-goals

- Touchscreen gestures (covered by the touch variant separately).
- The SoftAP web UI / provisioning flow itself (see `0006`).

## 4. Behaviour

### 4.1 Inputs

Three onboard buttons (Waveshare ESP32-S3-LCD-1.54 pinout):

| Button | GPIO | Notes |
|--------|------|-------|
| PLUS   | IO4  | Fully free, no system role. |
| BOOT   | IO0  | Strapping pin **at reset only**; free at runtime. |
| PWR    | IO5  | Power-circuit button; long-press = power off. |

All buttons are active-low.

### 4.2 States / flow

**Navigation (short press):**

| Button | Short press |
|--------|-------------|
| PLUS   | **Next** — move/scroll through items; **wraps around**. |
| PWR    | **OK / Select**. |
| BOOT   | **Back / Home**. |

Rationale: every navigation action is a single short press (no short/long
ambiguity); using PWR as Select mirrors the smartwatch convention (side/power
button selects); a wrapping Next removes the need for a dedicated Previous,
freeing BOOT for Back/Home.

**System actions (long press, 5 s hold):**

| Button | Long press (5 s) | Action |
|--------|------------------|--------|
| PWR    | 5 s hold | Power off (release battery latch). |
| BOOT   | 5 s hold | Restart into **SoftAP / Wi-Fi configuration** mode. |

### 4.3 Outputs / data formats

- On a recognised long-press the device gives clear on-screen feedback
  (e.g. "Powering off…" / "Entering Wi-Fi setup…") before acting.

### 4.4 Error handling & edge cases

- **BOOT vs. flashing:** holding BOOT at *runtime* only triggers SoftAP entry.
  ROM download mode is entered solely by holding BOOT *during reset/power-on*,
  which is the normal flashing procedure and is unaffected.
- **Strap re-sample on software reset:** because BOOT is a strapping pin, the
  SoftAP transition MUST NOT call `esp_restart()` while BOOT is still held
  (a software reset re-samples straps and could enter download mode). Implement
  by either (a) bringing up SoftAP in place without rebooting, or (b) waiting
  for BOOT release before restarting.
- **Accidental power-off during navigation:** PWR short-press (OK) must not
  trip the 5 s power-off; the power-off timer only fires on a continuous hold.

## 5. Acceptance criteria

- [ ] PLUS short-press advances selection and wraps at the end of a list.
- [ ] PWR short-press selects/confirms without powering off.
- [ ] BOOT short-press navigates back / returns home.
- [ ] PWR held 5 s powers the device off.
- [ ] BOOT held 5 s at runtime enters SoftAP config mode with on-screen feedback.
- [ ] Entering SoftAP never lands the device in ROM download mode.
- [ ] Holding BOOT during reset/power-on still enters download mode for flashing.

## 6. Security / privacy considerations

- SoftAP entry exposes the provisioning portal; it is gated behind a deliberate
  5 s hold to reduce accidental exposure. Portal security is covered by `0006`.

## 7. Open questions

- Should long-press durations be configurable, or is 5 s fixed?
- Do we need a "Previous" gesture (e.g. PLUS long-press) for long lists, or is
  wrap-around sufficient?
