# Native 7-inch UI — Rev B 3

Status: implementation contract

## Goal

The 1024×600 Waveshare 7B UI must be usable as a standalone bedside
interface. The browser remains available for raw export, local firmware files,
and deep simultaneous multi-channel inspection, but it is not a prerequisite
for setup, morning review, or diagnostics.

The visual source is the `Handoff Rev B 3` package. Rev A continues to govern
Home and any state not replaced below.

## Wave-one scope

- Replace native History with the morning-review design and its eleven named
  states.
- Add the resumable six-step first-run setup flow and its sixteen named states.
- Add Logs as a native Manage destination and cover its six named states plus
  save and keyboard states.
- Change Manage to the eight-destination rail: Devices, Connectivity, Alerts,
  Uploads, Storage, System, Logs, Advanced.
- Preserve the already-shipping display controls by housing them under System
  until the dedicated display-preferences redesign lands.
- Apply the Rev B copy and degraded-state corrections where the runtime can
  report the underlying state truthfully.

## Hardware and protocol corrections

The handoff contains illustrative assumptions that conflict with the running
device. Firmware behavior is authoritative for these details:

- AirSense 11 pairing uses the four-digit code produced after SomnoTrace sends
  `StartKeyExchange`. The touch flow still starts with the machine-side
  `More` → `myAir App` instructions, then scans, selects a machine, starts key
  exchange, and asks for the four-digit code.
- ESP32-S3 Wi-Fi is 2.4 GHz. Fixtures and copy must not promise a 5 GHz link.
- Native History keeps Flow in L/min, matching stored data and the browser.
- Generic apnea remains a distinct event type. The compact marker lane may
  share geometry, but it must never relabel generic apnea as obstructive apnea.
- Pair completion can initially prove only the advertised name, BLE address,
  and client identity. Serial and firmware remain explicitly unavailable until
  the encrypted device-identification RPC has supplied them.
- The card copy refers to the microSD slot without assuming an enclosure edge.

## Interaction rules

- The actual Rev B 64 px header and 74 px bottom navigation govern the shared
  Home/History/Manage shell. Setup uses its separate 60 px header and no bottom
  navigation.
- Compact visual controls receive an invisible hit area of at least 44×44 px.
- Actions continue on touch-down. There is no delayed release animation.
- History keeps only an all-night envelope in PSRAM. Detailed zoom windows are
  reread asynchronously from microSD and never expose a false Cancel action.
- Initial night loading is cancellable and never publishes partial data.
- Unknown measurements render as an em dash and are never coerced to zero.
- Gaps remain gaps; no chart interpolates across missing data or therapy-off
  intervals.
- Focusing Logs search pauses the viewport. Closing the keyboard does not
  silently resume it.
- `Save to card` writes a named snapshot of the native retained log buffer;
  it is separate from the continuous rotating logs already written to card.
- `Clear` clears only the native in-memory view and never deletes card logs.

## Resource rules

- Build History, setup, and Manage detail surfaces lazily or reuse bounded row
  objects. Do not eagerly allocate every gallery state.
- Large retained data belongs in PSRAM; LVGL object metadata and DMA-critical
  buffers must remain within the measured internal-RAM budget.
- History, Logs, uploads, and recording must use the shared SD lease rather
  than opening files concurrently.
- QEMU acceptance covers deterministic pixels and touch paths. BLE, SDMMC,
  GT911, backlight, and RGB timing still require a physical-board pass.

## Deferred scope

Wave two owns full Connectivity, Alerts, Uploads, and System configuration.
Wave three owns firmware/maintenance, destructive Advanced actions, and the
true Information-only Home design. Fixed landscape and speaker controls are
not backlog items for the 7B hardware.
