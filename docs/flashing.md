# Web Flashing Guide

> Flash SomnoTrace onto your device directly from your web browser — no coding, command line, or software installation required!

---

## What You Need

1. **Waveshare ESP32-S3-Touch-LCD-1.54** board (Touch variant with battery preferred — [Buy on Waveshare](https://www.waveshare.com/esp32-s3-lcd-1.54.htm?sku=33869)).
2. **USB-C Data Cable** (make sure it is a *data* cable, not a charge-only cable).
3. **Computer with a modern browser**: Google Chrome, Microsoft Edge, Brave, or Opera (these browsers support Web Serial).
4. **MicroSD card** (formatted as FAT32 or exFAT, 8 GB to 32 GB recommended) inserted into the board's SD slot.

---

## Step-by-Step Instructions

### Step 1: Download the Firmware

1. Go to the [SomnoTrace Releases page](https://github.com/ilyakruchinin/SomnoTrace/releases).
2. Under the latest release, download the file ending in **`-full.bin`** (e.g., `somnotrace-v1.0.2-full.bin`).
   - ⚠️ **Do NOT download the `-ota.bin` file** — that is for over-the-air updates from within the web interface only, and cannot be used for initial flashing.
3. Save the file somewhere easy to find on your computer (e.g., your Downloads folder).

---

### Step 2: Connect Your Device

1. Plug the USB-C cable into the Waveshare board.
2. **Press and hold the BOOT button** (the left physical button on the side of the board) while plugging the other end into your computer. This puts the board into flashing mode.
3. Once plugged in, release the BOOT button. The screen may light up or remain dark — either is normal.

> **Already plugged in?** Unplug the cable, hold the BOOT button, plug it back in, then release.

---

### Step 3: Open the Web Flasher

1. Open **Google Chrome** or **Microsoft Edge**.
2. Navigate to the official [Espressif Web Flasher](https://espressif.github.io/esptool-js/).

---

### Step 4: Connect to the Board

1. In the Web Flasher, leave the baud rate at the default (**921600**).
2. Click the blue **Connect** button at the top.
3. A browser popup will appear showing available serial devices:
   - Look for **`USB JTAG/serial debug unit`** or **`ESP32-S3`** (or a COM / tty port).
   - Select it and click **Connect**.
4. Once connected, the console at the bottom will display device details (confirming it is an ESP32-S3).

> **Port already in use?** If the Connect button doesn't work or the port doesn't appear, refresh the browser page and try again.

---

### Step 5: Flash the Firmware

1. In the **Flash Address** box, make sure the address is set to:
   ```text
   0x0
   ```
2. Click **Choose File** (or Browse) next to `0x0`, and select the **`-full.bin`** file you downloaded in Step 1.
3. Click the **Program** (or Flash) button.
4. You will see a progress bar and percentage counter. Flashing takes about **30 to 60 seconds**.
5. When finished, the status will show **"Leaving... Finished successfully"**.

---

### Step 6: First Boot & Wi-Fi Setup

1. Unplug the USB cable and plug it back in (or press the power button) to restart the device.
2. The LCD display will show the **SomnoTrace** logo and display setup instructions.
3. On your smartphone or computer, search for nearby Wi-Fi networks:
   - Connect to the network named **`SomnoTrace-Setup`** (or `SomnoTrace-XXXXXX`).
4. A setup page will open automatically (or open your browser and visit `http://192.168.4.1`):
   - Select your home Wi-Fi network and enter the password.
   - Click **Save & Connect**.
5. SomnoTrace will connect to your home Wi-Fi and show its new IP address on the LCD screen!

You can now open any web browser on your home network and visit:
```text
http://somnotrace.local
```
*(or open the IP address shown on the screen).*

---

### Step 7: Pair Your AirSense 11

Once Wi-Fi is configured, the next step is to pair SomnoTrace with your CPAP over Bluetooth so therapy sessions can be recorded.

👉 **[AirSense 11 Pairing Guide](pairing.md)**

---

## Troubleshooting

- **Browser doesn't show the Connect popup?**  
  Make sure you are using Google Chrome or Microsoft Edge. Apple Safari and Mozilla Firefox do not support Web Serial.
- **No COM / Serial device appears in the list?**  
  - Try another USB-C cable (many cables included with phones/vapes are power-only and cannot transfer data).
  - Hold the **BOOT** button while plugging the board into your computer.
- **SD Card Error on screen?**  
  Ensure your MicroSD card is inserted securely into the slot before powering on, and that it is formatted as FAT32 or exFAT.
