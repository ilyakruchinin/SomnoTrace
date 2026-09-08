# Hardware reference — Waveshare ESP32-S3-Touch-LCD-1.54

Concise board reference for development. Read only when you need pin/peripheral
details. Source: <https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54>.

> Note: there is also a non-touch variant ("ESP32-S3-LCD-1.54"). This board is
> the **touch** version (CST816 controller present).

## Core specs

- **MCU:** ESP32-S3R8 (dual-core Xtensa LX7 @ 240 MHz), Wi-Fi 2.4 GHz + BLE 5.
- **Memory:** 8 MB PSRAM, 16 MB NOR flash.
- **Display:** 1.54" 240×240 LCD, **ST7789** controller, 4-wire SPI.
- **Touch:** **CST816** capacitive controller, I2C.
- **Audio:** **ES8311** codec + **ES7210** ADC (echo cancel) over I2S;
  **NS4150B** amplifier (enabled via PA CTRL).
- **IMU:** **QMI8658** 6-axis (I2C, INT line).
- **Other:** RTC, microSD (TF) slot (SDMMC 4-bit), USB Type-C (native
  USB-Serial-JTAG), Li-battery charge/discharge interface, chip antenna.

## Buttons / boot

- Physical buttons: **PWR** (IO5), **PLUS** (IO4), **BOOT** (IO0).
- **IO0 = BOOT strapping pin** (labelled "KEY MINUS" in the pin table). Hold it
  during reset/connect to force ROM **download mode** for flashing.
- No dedicated EN/RESET button is exposed.

## GPIO map (from board pinout table)

| GPIO | Function(s) |
|------|-------------|
| IO0  | KEY MINUS / **BOOT** strap |
| IO1  | BAT ADC (battery voltage sense) |
| IO2  | BAT EN (battery power latch/enable) |
| IO3  | CHG STAT (charger status) |
| IO4  | KEY PLUS |
| IO5  | KEY PWR |
| IO6  | IMU INT |
| IO7  | PA CTRL (audio amp enable) |
| IO8  | I2S MCLK |
| IO9  | I2S SCLK |
| IO10 | I2S LRCK |
| IO11 | I2S ASDOUT |
| IO12 | I2S DSDIN |
| IO13 | SD D2 |
| IO14 | SD D3 |
| IO15 | SD CMD |
| IO16 | SD CLK |
| IO17 | SD D0 |
| IO18 | SD D1 |
| IO19 | USB D- (USB_N) |
| IO20 | USB D+ (USB_P) |
| IO21 | LCD CS |
| IO38 | LCD CLK (SCLK) |
| IO39 | LCD DIN (MOSI) |
| IO40 | LCD RST |
| IO41 | I2C SCL (shared: touch + codec + IMU) |
| IO42 | I2C SDA (shared: touch + codec + IMU) |
| IO43 | ESP TXD (UART0 TX) |
| IO44 | ESP RXD (UART0 RX) |
| IO45 | LCD DC |
| IO46 | LCD BL (backlight) |
| IO47 | TP RST (touch reset) |
| IO48 | TP INT (touch interrupt) |

### Buses at a glance

- **LCD (SPI):** CS=IO21, CLK=IO38, DIN/MOSI=IO39, DC=IO45, RST=IO40, BL=IO46.
- **Shared I2C:** SCL=IO41, SDA=IO42 — CST816 touch, ES8311/ES7210 codec,
  QMI8658 IMU all sit on this bus. Touch also uses RST=IO47, INT=IO48.
- **I2S audio:** MCLK=IO8, SCLK=IO9, LRCK=IO10, ASDOUT=IO11, DSDIN=IO12,
  PA enable=IO7.
- **microSD (SDMMC 4-bit):** CLK=IO16, CMD=IO15, D0=IO17, D1=IO18, D2=IO13,
  D3=IO14.
- **Battery:** ADC=IO1, enable/latch=IO2, charger status=IO3.

## Battery monitoring — limitations and design notes

The board uses a **200 kΩ / 100 kΩ resistor divider** on GPIO1 (BAT_ADC,
ADC1_CH0) to measure VBAT.  The divider ratio is ×3, so the ADC sees
VBAT/3.  Key constraints and mitigations:

- **Divider drain.** The divider draws ~12 µA continuously (VBAT / 300 kΩ).
  This is small relative to the system load but is always present, even
  when the device is "off" (BAT_EN latched off).  There is no firmware
  switch to disconnect it; only removing the battery eliminates the drain.

- **ADC calibration.** `ADC_ATTEN_DB_12` is not a clean 0–3.3 V range and
  varies per chip.  The firmware uses the ESP-IDF eFuse-backed curve-fitting
  calibration (`adc_cali_curve_fitting_config`) when available.  On chips
  without eFuse calibration data, readings fall back to nominal
  full-scale and will read several percent low.  There is no runtime
  workaround — the eFuse Vref/DOTP must be programmed at the factory.

- **Wi-Fi/BLE TX bursts.** Radio transmissions dip the power rail for
  a few hundred microseconds.  Back-to-back ADC samples all land inside
  the same dip and average to the same wrong value.  The monitor spreads
  256 samples over ~1 second (4 ms apart) and uses a trimmed mean
  (discards the lowest and highest 12.5%) to reject these transients.

- **Charging IR offset.** While the charger is active (CHG_STAT = GPIO3
  low), terminal voltage sits above the cell's true open-circuit voltage
  (OCV) by ~120 mV due to internal resistance.  The firmware subtracts
  this offset before looking up the OCV table.  After unplugging, a
  30-second settle period lets the terminal voltage relax to OCV before
  the next sample.

- **Li-ion discharge curve.** Battery percentage is derived from a
  piecewise-linear OCV lookup table (4200 mV = 100%, 3300 mV = 0%),
  not a linear voltage-to-percent map.  The curve is flat from ~3.9 V
  to ~3.6 V (most of the capacity) then drops steeply, so a linear map
  would read 50% for most of the discharge and then cliff-dive.

- **Slew limiting.** The published percentage moves at most 1 percentage
  point per update cycle (60 s discharge, 20 s charge) and is forced
  monotonic while charging, so the display walks smoothly instead of
  jumping on every sample.

- **Sampling cadence.** 60 s when discharging, 20 s when charging
  (users watch a charge bar), 30 s settle after charger unplug.  A real
  battery cannot move fast, so sampling rarely saves power and reduces
  display jitter.

## Wi-Fi — limitations and design notes

- **Network selection by RSSI.** At boot and during failover, all
  configured SSIDs are scanned and ranked by signal strength (strongest
  first).  The device connects to the strongest visible candidate.  If
  two SSIDs are equally strong, the lower slot number wins (stable sort).
  There is no manual priority override — RSSI is the sole ranking factor.

- **Stale connection bug (fixed).** The ESP-IDF Wi-Fi event handler does
  not automatically clear the application-level "connected" flag on
  `WIFI_EVENT_STA_DISCONNECTED`.  The firmware now calls `link_mark_down()`
  on disconnect, which immediately clears the published IP and SSID.
  Consumers (`/api/status`, LCD) read the live link state via
  `netprov_get_link()` and never cache "connected".

- **Failover.** `esp_wifi_connect()` only retries the single SSID
  currently programmed into the driver.  Without intervention, a
  permanently disappeared network strands the device even when another
  configured network is in range.  The event handler counts failed
  reconnects; after 5 failures it sets a rescan flag.  A background
  supervisor task (`link_supervisor_task`) then performs a full
  scan-and-rank across all configured networks and connects to the
  best available candidate.  Credentials are cached at initial connect
  so the supervisor can rescan without NVS access.

- **Retry timing.** Initial connection: 3 attempts per SSID, 5 s between
  retries.  Reconnect after link loss: immediate retry, then up to 5
  retries to the same SSID before escalating to a full rescan.  The
  rescan itself is blocking (~1–2 s active scan) and runs off the event
  loop in the supervisor task.

## Further reading (fetch only if needed)

- Main docs: <https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54>
- ESP-IDF setup: <https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54/Development-Environment-Setup-ESPIDF>
- Resources (schematic PDF, datasheets): <https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54/Resources-And-Documents>
- Code examples (Apache-2.0; reference only, do not copy — clean-room):
  <https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54>
