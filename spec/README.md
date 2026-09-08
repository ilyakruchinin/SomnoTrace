# Specifications

This folder holds **specification documents** that describe the behaviour the
firmware must implement. Specs are the source of truth for *what* and *why*;
the code is *how*.

These documents are part of the repository and are reviewed/versioned with the
code.

## Conventions

- One spec per file, numbered sequentially: `NNNN-short-title.md`
  (e.g. `0001-airsense11-ble-sync.md`).
- Start from [`0000-template.md`](0000-template.md).
- A spec should be implementation-agnostic where practical: describe observable
  behaviour, data formats, states, and acceptance criteria — not internal code
  structure.
- When a spec changes materially, update its **Status** and **Changelog**.

## Index

- `0001-airsense11-ble-sync.md` — discovering, pairing, and pulling therapy
  data from the ResMed AirSense 11 over BLE. _(Draft stub)_
- `0002-edf-export.md` — mapping pulled data to EDF/EDF+ files. _(Draft stub)_
- `0003-o2ring-ble-sync.md` — retrieving overnight oximetry from the Wellue /
  O2 Ring devices. _(Draft stub)_
- `0004-upload-smb.md` — SMB upload behaviour, paths, retries. _(Draft stub)_
- `0005-upload-sleephq.md` — SleepHQ upload/integration behaviour. _(Draft stub)_
- `0006-device-provisioning.md` — Wi-Fi/credentials provisioning & config.
  _(Draft stub)_
- `0007-button-controls.md` — physical button navigation model and SoftAP entry
  gesture. _(Accepted)_
- `0008-config-and-network-lifecycle.md` — NVS configuration schema, Wi-Fi
  selection/roaming logic, and SoftAP captive-portal fallback. _(Draft)_
- `0009-web-interface.md` — Progressive Web App (PWA) web interface with theme support. _(Draft)_
- `0010-web-ui-architecture-and-design.md` — web UI architecture, design tokens, and CSS conventions. _(Draft)_
- `0011-web-api-endpoints.md` — HTTP REST and SSE data contracts for telemetry and configuration. _(Proposed)_
- `0012-battery-monitoring-and-power-management.md` — battery ADC sampling, OCV estimation, calibration state machine, dynamic CC/CV slew rate, and power latch. _(Implemented)_
- `0013-sleep-staging-algorithms.md` — CPAP-only, oximetry-only, and fused sleep-staging algorithms: features, decision rules, formulas, validation gates, and clean-room provenance. _(Implemented)_

> Draft stubs are placeholders to guide structure; flesh them out as the design
> firms up.
