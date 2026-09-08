# SomnoTrace — Requirements

This document records the **hardware** the firmware targets and the
**toolchain/software** required to build and flash it. Keep it in sync with
`scripts/idf.sh` (the pinned ESP-IDF version) and `sdkconfig.defaults`.

## 1. Hardware

| Item | Requirement | Notes |
|------|-------------|-------|
| MCU module | **ESP32-S3** (dual-core LX7, Wi-Fi + BLE 5) | BLE central + Wi-Fi/Ethernet for upload |
| Flash | ≥ 8 MB (16 MB recommended) | OTA + EDF staging |
| PSRAM | Recommended (Octal/Quad) | Buffering EDF/oximetry data |
| Connectivity | 2.4 GHz Wi-Fi | SMB / SleepHQ upload |
| USB | USB-C (native USB-Serial-JTAG) | Flashing & logs |

### Target devices (BLE peripherals)

| Device | Role | Reference protocol |
|--------|------|--------------------|
| ResMed AirSense 11 | CPAP therapy data source | clean-room, informed by `airbreak-plus` |
| Wellue O2 Ring S / SleepHQ O2 Ring Pro | Overnight oximetry source | clean-room, informed by `o2ring-s-protocol` |

> Update this section with the exact dev board / custom PCB once finalised
> (e.g. specific module part number, antenna, power/battery details).

## 2. Build toolchain

| Tool | Version | How it's provided |
|------|---------|-------------------|
| **ESP-IDF** | **v5.5.1** (pinned) | Official `espressif/idf:v5.5.1` Docker image via `scripts/idf.sh` |
| Docker Engine | ≥ 20.10 | Only host dependency required |
| Bash | ≥ 4 | To run the build scripts |
| Git | any recent | Version stamping of build artifacts |

The ESP-IDF version is pinned in `scripts/idf.sh` (env `IDF_TAG`). To override
temporarily:

```bash
IDF_TAG=v5.5 ./scripts/idf.sh build
```

### Why Docker

No ~2 GB SDK install on the host; reproducible, pinned toolchain. The only
host requirement is Docker (plus Bash/Git). A `.devcontainer` is provided for
VS Code users who prefer an in-container workflow.

## 3. Build & flash

```bash
# One-shot: build + produce a single flashable image in dist/
./scripts/build-dist.sh

# Force a clean (from-scratch) build first:
./scripts/build-dist.sh --clean

# Flash (device on /dev/ttyACM0 by default; override with IDF_PORT):
./scripts/idf.sh -p /dev/ttyACM0 flash monitor
```

All images in `dist/` are **merged** and flashable at offset **`0x0`**.

## 4. Target chip / project config

- Target: `esp32s3` (set via `idf.py set-target esp32s3`; recorded in
  `sdkconfig.defaults`).
- Project defaults live in `sdkconfig.defaults`. The generated `sdkconfig` is
  **not** committed.
