<img src="assets/images/waveshare.jpeg" alt="Waveshare ESP32-S3-Touch-LCD-1.54" width="140" align="right" />
<p align="center">
  <img src="assets/svg/logo-full.svg" alt="SomnoTrace" width="480" />
</p>
<br clear="right" />

**SomnoTrace** is firmware for a small, inexpensive (~US$20) bedside device that:

- 📥 Automatically pulls therapy data from your **ResMed Series 10/11 (AirSense / AirCurve)**
  - <ins>Series 11</ins> - Directly, wirelessly over Bluetooth 
    - (*<ins>no SD card required in the machine<ins>*)
  - <ins>Series 10</ins> - Through [SomnoBridge](https://github.com/ilyakruchinin/somnobridge) and an EZShare WiFi SD card in the machine 
    - (Series 10 doesn't have Bluetooth support)
- 💓 Downloads your O2 Ring pulse-oximetry recordings over Bluetooth (wirelessly).
- ⏱️ <ins>Corrects the machine's clock drift</ins> so therapy records align to real internet time 
  - (*<ins>your CPAP and oximeter graphs line up properly</ins>*).
- 📤 Writes standard EDF files and uploads them automatically to your NAS/PC share (SMB) for [OSCAR](https://www.sleepfiles.com/OSCAR/), [SleepHQ](https://sleephq.com), [Aerivue](https://aerivue.app), or [BreathingRoom](https://breathingroom.io)

---

## Key PAP features
| Feature | Series 11 | Series 10 | Notes | Exclusive to SomnoTrace 💎 |
|---------|-----------|-----------|-------|-------------------------|
| Automatic data import | ✅ | ✅ | Automatically pulls therapy data from your ResMed machine | |
| Time drift correction | ✅ | ✅ | Corrects the machine's clock drift so therapy records align to oximetry data | 💎 wireless, no mods |
| Direct Bluetooth data pull | ✅ | ❌ | Wireless, no WiFi SD card required | |
| Real-time therapy interruption alerts | ✅ | ❌ | Push notifications to your phone, smartwatch, or smart bed shaker via ntfy | 💎 PUSH notifications |
| Real-time high-leak alerts | ✅ | ❌ | Push notifications to your phone, smartwatch, or smart bed shaker via ntfy | 💎 PUSH notifications |
| Optional escalating buzzer alarm | ✅ | ❌ | If you miss the notification, the device will sound an escalating buzzer alarm | 💎 if not acked |
| Send RPC commands to the machine | ✅ | ❌ | Query machine settings or start/stop therapy remotely | |
| Midday continuous sessions | ✅ | ❌ | Therapy spanning midday is recorded as one [continuous session](docs/cross-noon.md) | |
| Smart home automations | ✅ | limited | Built-in MQTT publishes live telemetry with Home Assistant Auto-Discovery | 💎 live therapy data |


## Key pulse oximetry features
| Feature | Availability | Notes | Exclusive to SomnoTrace 💎 |
|---------|-----------|-----------|-------|
| Automatic oximetry import | ✅ | Automatically pulls oximetry data from your O2 Ring | |
| Sleep staging | ✅ | Classifies sleep stages into Wake, REM, Light, or Deep sleep | 💎 on-device, no subscription, no cloud |

## Other features
| Feature | Availability | Notes | Exclusive to SomnoTrace 💎 |
|---------|-----------|-----------|-------|
| OTA firmware updates| ✅ | Simple update via web interface | |
| Support for SleepHQ, Aerivue and BreathingRoom | ✅ | Automatic uploads to cloud services | 💎 widest endpoint support |
| 3-click installation ([only takes 1 minute](#3-click-install)) | ✅ | Anyone can do it, no technical knowledge required | 💎 install from a web page |


---

## Extra features
- 🖥️ **Color screen & web dashboard:** Live breathing graphs, Wi-Fi status, battery level, and clock on the bedside LCD; interactive sleep charts and device settings from any browser at `http://somnotrace.local`. Fully local, on-device data, you don't need any subscription or Cloud service (optional)
- 🏕️ **Camping mode:** Records therapy with no Wi-Fi or internet at all. See the [Camping Mode guide](docs/camping.md).
- 🧠 **On-device sleep staging (official builds):** uses a pre-trained on-device machine learning model to classify sleep stages into Wake, REM, Light, or Deep sleep - locally on the ESP32-S3: no cloud, no subscription. *(Proprietary model bundled in official release builds only; source builds compile a functional stub — see [somnostage](https://github.com/ilyakruchinin/somnostage).)*

## Series 11 Highlights
- 🚨 **Therapy interruption & high-leak alerts:** If your mask slips off, the leak rate stays high, or therapy stops unexpectedly at night, SomnoTrace sends a push notification to your phone, smartwatch, or smart bed shaker via [ntfy](https://ntfy.sh) — with an optional escalating buzzer alarm on the device speaker.
- ⚡ **BLE → Wi-Fi bridge & smart home:** Query machine settings or start/stop therapy remotely, and build [Home Assistant automations](docs/automations.md) (e.g. turn off bedroom lights when therapy starts). Built-in MQTT publishes live telemetry with Home Assistant Auto-Discovery.

---

## Hardware

<img src="assets/images/waveshare.jpeg" alt="Waveshare ESP32-S3 Touch LCD 1.54 Front" width="260" align="right" />

SomnoTrace runs on a compact, affordable, all-in-one development board:

- **Hardware Board:** **[Waveshare ESP32-S3-Touch-LCD-1.54](https://www.waveshare.com/esp32-s3-lcd-1.54.htm?sku=33869)**  
  *(The **touch variant with battery** is strongly recommended for portable bedside use).*
- **Display:** 1.54" round-corner color screen with touch control.
- **Storage:** MicroSD card slot for saving high-resolution sleep data and EDF files.
- **Audio:** Onboard speaker for therapy alerts and status tones.
- **Power:** USB Type-C or internal rechargeable battery with smart charging.

**Additional accessories required:**
- micro-SDHC card (8 GB minimum, 16 GB+ recommended; U1 or U3 speed class)
  - goes inside the WaveShare board for internal file storage
- USB-C data cable (for initial flashing and charging)
- USB-C charger (for overnight power)

> ⚡ **Power note:** SomnoTrace is designed to run on **stable USB-C power** overnight — the battery is outage protection, not a primary power source. See the [Battery & Power Guidelines](docs/battery.md) (including the `--% ⚡` calibration fix).

---

## Supported Devices

- **CPAP Machines:**
  - ResMed AirSense 11 (AutoSet / Elite) — direct Bluetooth
  - ResMed AirCurve 11 (VAuto / ASV) — direct Bluetooth
  - ResMed Series 10 (AirSense 10 / AirCurve 10 / Lumis) — via [SomnoBridge](https://github.com/ilyakruchinin/somnobridge) + an EZShare Wi-Fi SD card
- **Pulse Oximeters:**
  - **O2 Ring S (Gen2)** — Viatom model PO2B (S8-AW). Also sold as: 
    - Wellue O2Ring S
    - SleepHQ O2 Ring Pro.
  - **O2 Ring (Gen1)** — Viatom model PO2 (S9). Also sold as:
    - Wellue O2Ring
    - LOOKEE O2Ring
    - SleepHQ O2 Ring (non-Pro)
- **Upload Destinations:**
  - Local network share (SMB — Windows, macOS, Linux/Samba) - for use with [OSCAR](https://www.sleepfiles.com/OSCAR/) or as a backup storage solution
  - [SleepHQ](https://sleephq.com)
  - [Aerivue](https://aerivue.app)
  - [BreathingRoom](https://breathingroom.io)

<br clear="right"/>

<details>
<summary><b>🔘 Button Controls</b></summary>

The board has three physical buttons on the side. Here's what each one does:

| Button | Action | What It Does |
|---|---|---|
| **BOOT** (left) | Hold 5 seconds | Enters Wi-Fi setup mode (AP hotspot) for initial configuration or network changes. |
| **POWER** (middle) | Single click | Toggles the LCD backlight on or off on demand (or dismisses a temporary wake). Works across all display modes. |
| **POWER** (middle) | Hold 5 seconds | Powers off the device. |
| **POWER** (middle) | Hold 2 seconds | Powers on the device (when off). |
| **PLUS** (right) | Single click | Acknowledges and silences an interrupted therapy or high-leak alert (if enabled). |
| **PLUS** (right) | Double click | Starts or stops therapy on the AirSense 11 (toggle — same as pressing the machine's own button). |

</details>

<details>
<summary><b>SomnoBridge LCD Indicators (Series 10)</b></summary>

On the status screen, the Bluetooth icon shows the SomnoBridge link and the
card icon shows the bridge's EZShare Wi-Fi status:

| Bluetooth | Card | Meaning |
|---|---|---|
| Blue | Green check | Bridge connected; card Wi-Fi joined. |
| Blue | Amber `!` | Bridge connected; card Wi-Fi disconnected. |
| Blue | Grey `?` | Bridge connected; card status not yet known or stale. |
| Dim | Dim `x` | Bridge paired but disconnected or reconnecting. |

The card icon is shown only for SomnoBridge, not a direct AirSense connection.
It indicates a Wi-Fi connection, not a guarantee that card files are reachable
or that transfers are currently allowed. During a long transfer, status
polling can be delayed and the card icon may temporarily show `?`. The Wi-Fi
bars at the right of the screen indicate SomnoTrace's own Wi-Fi connection.

</details>

---

## How It Works

```mermaid
flowchart LR
    AS11["ResMed Series 11\n(AirSense 11 / AirCurve 11)\n(Bluetooth)"] -->|Wireless Sync| ESP["SomnoTrace\n(Bedside Device)"]
    AS10["ResMed Series 10\n(AirSense 10 / AirCurve 10)\n(EZShare card)"] -->|Wi-Fi| SB["SomnoBridge"]
    SB -->|BLE| ESP
    O2["Viatom O2 Ring\n(Gen1 / Gen2)\n(Bluetooth)"] -->|Oximetry Sync| ESP
    ESP -->|Saved Locally| SD["MicroSD Card\n(.edf files)"]
    ESP -->|Auto Upload| SMB["Home NAS / PC Share"]
    ESP -->|Auto Upload| CLOUD["SleepHQ / Aerivue /\nBreathingRoom"]
    ESP -->|View in Browser| WEB["Web Dashboard & Charts"]
```

1. **Pair once** — [connect SomnoTrace to your ResMed Series 11 machine](docs/pairing.md) (or SomnoBridge to a Series 10) and O2 Ring over Bluetooth in seconds.
2. **Sleep normally** — SomnoTrace records airflow, pressure, leak, and respiratory events in real time (or imports after therapy ends for Series 10), then writes standard EDF files and uploads them to your configured destinations.
3. **Review anywhere** — open `http://somnotrace.local` for interactive charts (in your local network), or (optionally) find your night in SleepHQ, Aerivue, or BreathingRoom.

---

## Web Dashboard

Access the built-in web portal from any device on your Wi-Fi network without installing any apps:

<p align="center">
  <img width="1107" height="1317" alt="SomnoTrace Web Dashboard" src="assets/images/ui.png" />
</p>

- **Interactive Sleep Graphs:** High-resolution zoomable graphs for Breathing Flow, Mask Pressure, Leak Rate, Respiratory Rate, and Flow Limitation.
- **Sleep Stage Hypnogram (Official Builds):** Visualizes your night's sleep architecture (Wake, REM, Light, Deep) with connected stage ribbons, transition analytics, and per-stage duration & percentage totals.
- **Clinical Sleep Metrics:** AHI, Obstructive Apnea (OA), Central Apnea (CA), Hypopnea (H), RERA, and 95th percentile pressure & leak stats.
- **One-Click Wi-Fi & Device Setup:** Configure Wi-Fi networks, upload destinations, screen brightness, and alert settings with simple toggles.
- **Over-the-Air (OTA) Updates:** Update firmware directly through the web interface with a single click.

---

## Installation

<a id="3-click-install"></a>
### 3-Click Web Installer at somnotrace.com (Recommended — Under 1 Minute)

No software, drivers, command line, or file downloads required. Install SomnoTrace directly from your browser (**Google Chrome**, **Microsoft Edge**, **Brave**, or **Opera**):

1. Plug your Waveshare board into your computer with a **USB-C data cable**.
2. Visit **[somnotrace.com](https://somnotrace.com)**.
3. It takes literally **3 clicks**:
   - Click **Install SomnoTrace**
   - Select **USB JTAG/serial debug unit** in the browser popup
   - Click **Connect**

That's it! The web flasher automatically fetches the latest stable release, switches the board into download mode, flashes the firmware, and reboots the device into SomnoTrace.

Once rebooted, connect your phone or computer to the **`SomnoTrace-Setup`** Wi-Fi network to complete Wi-Fi setup.

👉 **[Detailed Web Flashing & Setup Guide](docs/flashing.md)**

*Developers: Docker-based build & flash commands are documented in [`AGENTS.md`](AGENTS.md).*

---

## Documentation

- 📖 **[Web Flashing Guide](docs/flashing.md)** — Easy browser-based installation guide for everyone.
- 🔗 **[AirSense 11 Pairing Guide](docs/pairing.md)** — Pair your ResMed CPAP with SomnoTrace over Bluetooth.
- 🔋 **[Battery & Power Guidelines](docs/battery.md)** — Why USB-C power is recommended, and the `--% ⚡` calibration fix.
- 🏕️ **[Camping Mode](docs/camping.md)** — Recording offline with no Wi-Fi or internet.
- 🕛 **[Cross-Noon Sessions](docs/cross-noon.md)** — Why midday therapy stays one continuous session.
- 🪟 **[Windows SMB Share Setup](docs/windows_10-11_smb.md)** — Securely share a folder on Windows 10/11 for automatic EDF uploads.
- ⚡ **[ResMed BLE RPC Bridge Guide](docs/rpc-bridge.md)** — Send direct queries and commands (`curl` examples) to the AirSense 11 over Wi-Fi.
- 🏠 **[Smart Home & Home Assistant Guide](docs/automations.md)** — Set up bedtime automations, compliance tracking, and mask-off alerts.
- 🛠️ **[Hardware Reference](docs/hardware/README.md)** — Pinouts, schematics, and hardware architecture.

<details>
<summary><b>📁 Accessing Files via FTP</b></summary>

SomnoTrace includes a built-in FTP server for downloading EDF and session files directly from the MicroSD card over Wi-Fi — no need to physically remove the card. FTP is optional and can be enabled or disabled from the web dashboard. By default, it allows anonymous (no password) access on your local network, but you can configure a username and password if you prefer.

Connect with any FTP client (e.g., FileZilla) to `ftp://somnotrace.local`.

> **FileZilla tip:** If you experience connection errors, go to **Site Manager → Edit → Transfer Settings** and set **"Limit number of simultaneous connections"** to **1**. The ESP32-S3 FTP server processes one connection at a time.

</details>

---

## Contributing

Contributions and ideas are always welcome! Please review [`CONTRIBUTING.md`](CONTRIBUTING.md) before submitting pull requests.

- **Contributor License Agreement:** A [CLA](CLA/individual-cla.md) is required before PRs can be merged (automated on your first PR).
- **Clean-Room Policy:** SomnoTrace is a clean-room implementation based on open protocol documentation. Do not copy third-party source code into this repository.

---

## Acknowledgements

SomnoTrace protocol understanding and interoperability research was informed by the following open-source projects (clean-room implemented — see [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md)):

- [airbreak-plus](https://github.com/m-kozlowski/airbreak-plus) — ResMed AirSense 11 BLE protocol reference
- [o2ring-s-protocol](https://github.com/nglessner/o2ring-s-protocol) — Wellue / O2 Ring S (Gen2) BLE protocol reference
- [farolone/wellue-o2ring-protocol](https://github.com/farolone/wellue-o2ring-protocol) — O2 Ring (Gen1) BLE protocol reference
- [OSCAR](https://gitlab.com/pholy/OSCAR-code) — European Data Format (EDF) interoperability and statistical metric reference
- [libsmb2](https://github.com/sahlberg/libsmb2) — SMB2/SMB3 client library (LGPL-2.1)
- [esp-idf-ftpServer](https://github.com/nopnop2002/esp-idf-ftpServer) — Embedded FTP server (MIT)
- [uPlot](https://github.com/leeoniya/uPlot) — Fast time-series charting library (MIT)
- [Roboto Font](https://github.com/google/fonts/tree/main/ofl/roboto) — UI typeface (SIL Open Font License 1.1)

---

## License

SomnoTrace is free software released under the **GNU General Public License v3.0** with an author attribution requirement under **GPLv3 Section 7(b)**.

- Full license text: [`LICENSE`](LICENSE)
- Copyright & Section 7(b) Attribution Terms: [`NOTICE`](NOTICE)

Any redistributed or derivative works must remain licensed under GPLv3 and preserve the author attribution notice:  
> *"Based on SomnoTrace, originally created by Ilya Kruchinin (https://github.com/ilyakruchinin)."*

**Optional SomnoStage Model Component:**  
Official release builds (published on GitHub Releases and flashed via the 1-click web installer at [somnotrace.com](https://somnotrace.com)) bundle the **SomnoStage** sleep-staging model under a separate, closed-source proprietary license (free for personal use on official builds). The model is not covered by GPLv3 and its weights are not stored in this repository. Compiling SomnoTrace from source automatically builds a functional stub without the proprietary model. Full model details, PSG benchmark accuracy, and licensing terms are documented at [ilyakruchinin/somnostage](https://github.com/ilyakruchinin/somnostage).

---

## Medical Disclaimer

SomnoTrace is an independent open-source project and is **not affiliated with, endorsed by, or associated with** ResMed, Wellue / Viatom, SleepHQ, Aerivue, or BreathingRoom. It is intended strictly for personal data portability and interoperability research. SomnoTrace is **not a medical device** and must not be used for clinical diagnosis, treatment decisions, or life-critical monitoring. Use entirely at your own risk.

![GitHub Downloads (all assets, latest release)](https://img.shields.io/github/downloads/ilyakruchinin/somnotrace/latest/total)
