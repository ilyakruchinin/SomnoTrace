# Pairing Your ResMed AirSense 11

> Pair SomnoTrace with a ResMed AirSense 11 over Bluetooth so therapy data can be recorded and uploaded automatically.

---

## Before You Start

- Make sure **Wi-Fi is already configured** on SomnoTrace ([flashing guide](flashing.md)).
- The AirSense 11 must be **powered on and in Bluetooth range** (within ~5–10 meters).
- The AirSense 11 should **not already be connected to the ResMed MyAir app** over BLE. If it is, disconnect or turn off Bluetooth on the phone/tablet first — the AS11 can only maintain one BLE connection at a time.

---

## Step 1: Put the AirSense 11 Into Pairing Mode

On the AirSense 11 LCD:

1. Select **More**.
2. Select **MyAir App**.
3. Select **OK, downloaded**.
4. Select **Connect**.

The CPAP is now waiting for a Bluetooth connection.

---

## Step 2: Pair From the SomnoTrace Web UI

1. Open a web browser on a device connected to the same Wi-Fi network as SomnoTrace:
   ```text
   http://somnotrace.local
   ```
   *(Or use the IP address shown on the SomnoTrace screen.)*
2. Open **Settings**.
3. Under **Connected Medical Devices (Data Sources)**, find **ResMed AirSense 11 CPAP**.
4. Click **Scan for AirSense 11**.
5. Select your device from the **Select Device** dropdown.
6. Click **Pair Device**.

---

## Step 3: Confirm the Passkey

1. The AirSense 11 screen will display a **4-digit passkey**.
2. In the web UI, enter the code in the field labeled:
   **"Enter the 4-digit code shown on your CPAP screen"**
3. Click **Confirm Code**.

If successful, the card shows **Paired** and SomnoTrace is now bonded to the AirSense 11. The pairing is saved and persists across reboots.

---

## Troubleshooting

- **No devices found:** Make sure the AirSense 11 is in pairing mode and that the MyAir app is not currently connected to it.
- **Pair fails after entering the code:** Check the passkey carefully and try again. If it keeps failing, unpair from any other app first and restart the pairing flow.
- **Need to re-pair:** Click **Unpair Device** in the web UI, then repeat the steps above.
