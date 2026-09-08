# Session Storage & EDF Export Architecture

- **Status:** Implemented
- **Author(s):** Ilya Kruchinin
- **Created:** 2026-06-29
- **Related specs:** 0002-edf-export.md

## 1. Summary

Defines the on-SD-card folder structure for separating ESP-native session
data (raw streams, spool files) from ResMed-compatible EDF export output.
Covers the 30-day Summary spool lookback, per-day summary storage,
multi-record STR.edf generation, and noon-based day grouping.

## 2. Motivation / goals

- **Clean separation**: ESP-native files (`.snt`, `.json`, `.spool`) never
  mix with ResMed SD card files (`.edf`, `Identification.json`). A user can
  copy or zip `SDCARD/` and hand it to OSCAR without any ESP artifacts.
- **Re-exportable**: `SDCARD/` is fully derived from `.sessions/` and can be
  deleted and regenerated at any time without BLE access.
- **Multi-day STR.edf**: OSCAR expects a single `STR.edf` at the SD card
  root with one record per day. Summary spool data is stored per-day and
  combined at export time.
- **DST-safe naming**: Session file prefixes include seconds and a numeric
  suffix is appended on collision (DST fallback produces the same local
  timestamp twice).

## 3. Non-goals

- Web-based re-export UI (future work).
- Automatic re-export on SD card insertion.
- Backfilling missing summary data from the AS11 outside the 30-day window.

## 4. Folder structure

```
/somnotrace/
  .sessions/                              # ESP-native data (internal)
    streams/
      20260627/                           # noon-day folder (YYYYMMDD)
        20260627_224219_brp.snt           # breath waveform stream
        20260627_224219_brp_mm.snt        # breath waveform (1 Hz summary)
        20260627_224219_sa2.snt           # SpO2/pulse stream
        20260627_224219_pld.snt           # per-breath stats stream
        20260627_224219_events.snt        # live event notifications (JSONL)
        20260627_224219_resp_events.bin   # TherapyEvents spool (protobuf)
        20260627_224219_ident.json        # device identification (Get RPC)
        20260627_224219_settings.json     # current settings (Get RPC)
        20260627_224219_session.json      # session metadata
        20260627_224219_manifest.json     # post-therapy collection manifest
    summaries/
      20260627.spool                      # per-day Summary spool (protobuf)
      20260628.spool
      meta.json                           # pull metadata

  SDCARD/                                 # ResMed SD card image (OSCAR-ready)
    STR.edf                               # multi-record daily summary
    Identification.json                   # device identity (nested AS11 format)
    Identification.crc                    # CRC-32 of Identification.json
    SETTINGS/
      CurrentSettings.json                # latest settings snapshot
    DATALOG/
      20260627/                           # noon-day folder
        20260627_224219_BRP.edf
        20260627_224219_PLD.edf
        20260627_224219_SA2.edf
        20260627_224219_EVE.edf
        20260627_224219_CSL.edf
```

### 4.1 Key naming conventions

- **Session prefix**: `YYYYMMDD_HHMMSS[_n]` where `_n` is a numeric suffix
  appended only on DST fallback collision (same local timestamp occurs twice).
- **Noon-based day folder**: Sessions before noon belong to the previous
  day's folder. This matches ResMed's AS11 native grouping.
- **Summary spool files**: Named `YYYYMMDD.spool` from the AS11's
  PeriodStart field (not ESP clock). Latest pull overwrites (atomic write).
- **EDF filenames**: `<prefix>_TYPE.edf` where prefix matches the session
  prefix from `.sessions/streams/`.

### 4.2 Summary spool collection

- Fixed 30-day lookback window (no state file needed).
- Pulled after each therapy session ends via `post_therapy_collect()`.
- Decoded from protobuf top-level field-2 wrappers into per-day records.
- Each day record written atomically to `.sessions/summaries/YYYYMMDD.spool`.
- `clock_drift_ms` applied to PeriodStart to convert AS11 internal time to
  NTP time before computing the day label.

### 4.3 Multi-record STR.edf generation

- Scans `.sessions/summaries/` for `*.spool` files.
- Parses each spool: extracts PeriodStart, session entries (MaskOn/MaskOff),
  statistics, and settings.
- Sorts records chronologically by PeriodStart.
- Writes a single `SDCARD/STR.edf` with one EDF data record per day.
- Each record: 77 data signals (115 int16 values) + 1 Crc16 = 232 bytes.
- Record duration: 86400.00 seconds (1 day).
- Start time: 12.00.00 (noon).

### 4.4 DATALOG EDF generation

- Per-session EDFs (BRP, PLD, SA2, EVE, CSL) generated from `.snt` files.
- Placed in `SDCARD/DATALOG/YYYYMMDD/` using the same noon-day as the
  source `.sessions/streams/` folder.
- `Identification.json` + `.crc` written to `SDCARD/` root.
- `CurrentSettings.json` copied to `SDCARD/SETTINGS/`.

## 5. Acceptance criteria

- [x] `.sessions/streams/` and `.sessions/summaries/` directories created
      on SD card init.
- [x] `SDCARD/` directory tree created on SD card init.
- [x] Session files use prefix-based naming in noon-day folders.
- [x] DST fallback collision handled with numeric suffix.
- [x] Summary spool pulled with 30-day lookback, stored per-day.
- [x] STR.edf contains one record per valid summary spool, sorted
      chronologically.
- [x] DATALOG EDFs placed in correct noon-day folders.
- [x] `Identification.json` and `CurrentSettings.json` written to SDCARD.
- [x] Build succeeds with no warnings.

## 6. Security / privacy considerations

- All data is stored on the local SD card (FAT32).
- No real patient data in tests or fixtures.
- `SDCARD/` contains personal medical data — user should handle accordingly
  when exporting.

## 7. Changelog

- 2026-06-29: Initial spec. Implemented in edf_gen.c, session_writer.c,
  post_therapy.c, sd_storage.c.
