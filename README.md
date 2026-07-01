# SomnoTrace

An open-source ESP32-S3 bridge that pulls sleep-therapy data from a ResMed
AirSense 11 CPAP machine over Bluetooth Low Energy, writes standard EDF files,
and uploads them to SMB and/or SleepHQ — no SD card required.

## Goals

- **Eliminate the SD card hassle.** No need to eject and reinsert the SD
  card daily just to upload data — SomnoTrace pulls therapy data directly
  over Bluetooth (BLE). This also avoids the "SD Card Errors" that some AirSense 11
  machines experience when using WiFi-enabled SD cards in the SD slot.
  Based on the BLE protocol work by
  [m-kozlowski/airbreak-plus](https://github.com/m-kozlowski/airbreak-plus),
  SomnoTrace is the first open-source solution to package this into a
  standalone device that looks and feels like a finished consumer product.
- **Correct timestamp drift.** The AirSense 11 has a known issue with
  inaccurate internal timestamps, causing drift in exported EDF files.
  SomnoTrace corrects for this drift by synchronising against NTP, enabling
  perfect time-matching with oximetry data from separate devices.
- **Cool live visualisation.** A real-time breathing flow waveform is
  displayed on the device's LCD during therapy.

## Device

<img src="https://www.waveshare.com/media/catalog/product/cache/1/image/560x560/9df78eab33525d08d6e5fb8d27136e95/e/s/esp32-s3-touch-lcd-1.54-1.jpg" alt="Waveshare ESP32-S3 Touch LCD 1.54 Front" width="300" align="right" />

- **Board:** Waveshare ESP32-S3-Touch-LCD-1.54 (touch variant preferred, but not required)
- **MCU:** ESP32-S3 (Wi-Fi + BLE 5) with 8 MB PSRAM
- **Display:** 1.54" ST7789 LCD (optional CST816 capacitive touch)
- **Source device:** ResMed AirSense 11 AutoSet

<img src="https://www.waveshare.com/media/catalog/product/cache/1/image/560x560/9df78eab33525d08d6e5fb8d27136e95/e/s/esp32-s3-touch-lcd-1.54-2.jpg" alt="Waveshare ESP32-S3 Detail View" width="160" style="margin-top: 10px; margin-right: 10px;" /> <img src="https://www.waveshare.com/media/catalog/product/cache/1/image/560x560/9df78eab33525d08d6e5fb8d27136e95/e/s/esp32-s3-touch-lcd-1.54-4.jpg" alt="Waveshare ESP32-S3 Side View" width="160" style="margin-top: 10px;" /> <img src="https://www.waveshare.com/media/catalog/product/cache/1/image/560x560/9df78eab33525d08d6e5fb8d27136e95/e/s/esp32-s3-touch-lcd-1.54-3.jpg" alt="Waveshare ESP32-S3 Side View" width="160" style="margin-top: 10px;" />

<br clear="right"/>

## How it works

1. Connects to the AirSense 11 over BLE and negotiates a session.
2. Receives the therapy data stream (flow, pressure, respiratory events,
   etc.) from the AirSense 11 and collects it in real time.
3. On therapy stop, collects summary spools and generates standard EDF files.
4. Uploads files to a configured SMB share and/or SleepHQ automatically.

## Planned features

- **Viatom O2 Ring S support** — BLE oximetry data collection from the
  Viatom/O2 Ring S pulse oximeter, with NTP-corrected timestamps for perfect
  alignment with CPAP therapy data.
- **Browser-based data visualisation** — built-in web interface for viewing
  therapy graphs and reports directly from the device. No need for third-party
  tools like OSCAR; all charts available through a browser.
- **Therapy interruption alarm** — configurable sound alert if therapy stops
  during the night (e.g. mask removed and not put back on), so you don't lose
  half a night of treatment without knowing.
- **AirSense 10 support** — therapy data collection from the AirSense 10 via
  WiFi SD cards (FYSETC SD WiFi Pro, FYSETC SD WiFi, EZShare WiFi SD), which
  sit in the machine's SD card slot and expose files over Wi-Fi.

## License

License to be determined. Likely GPLv3 with a Section 7(b) attribution requirement.
Will be published along with source code when ready.
