# 0001 — AirSense 11 BLE sync

- **Status:** Draft
- **Author(s):** Ilya Kruchinin (@ilyakruchinin)
- **Created:** 2026-06-17
- **Last updated:** 2026-06-24
- **Related specs:** `0002-edf-export.md`

## 1. Summary

Discovering, pairing, and pulling therapy data from the ResMed AirSense 11
over BLE.  The firmware acts as a BLE central, connects to the AS11's custom
GATT service, and performs an SRP-6a key exchange to establish a session.

## 2. Motivation / goals

- Wirelessly pair the ESP32-S3 to the AS11 without a phone app.
- Retrieve CPAP therapy session data for EDF export and cloud upload.
- Store pairing credentials (clientId + masterPairKey) in NVS for subsequent
  reconnections.

## 3. Non-goals

- Acting as a BLE peripheral (advertising).
- BLE link-layer encryption / pairing (the AS11 does not require it for the
  custom GATT service; security is handled at the application layer via SRP).
- Replacing the AirView / myAir app ecosystem.

## 4. Behaviour

### 4.1 Inputs

- BLE device address (from scan results or manual entry).
- 4-digit passkey displayed on the AS11 LCD during pairing.

### 4.2 States / flow

```
IDLE -> SCANNING -> IDLE
IDLE -> CONNECTING -> WAIT_PASSKEY -> CONFIRMING -> PAIRED
                                -> ERROR (any step fails)
```

**GATT discovery sequence** (must match the order the AS11 expects):

1. **MTU exchange** -- the AS11 often initiates this before our code calls
   `ble_gattc_exchange_mtu`; the `BLE_GAP_EVENT_MTU` handler updates `s_mtu`
   regardless.  Do not rely on the `on_mtu` GATT callback firing.
2. **Full service discovery** -- `ble_gattc_disc_all_svcs` (Read By Group
   Type: Primary Service).  Captures the ResMed vendor service handle range
   (UUID 0xFD56).
3. **Full characteristic discovery** -- `ble_gattc_disc_all_chrs` (Read By
   Type: Characteristic, handles 1-65535).  Captures TX, RX, Device Name,
   Appearance, and three Steehl vendor characteristic handles.
4. **Descriptor discovery** -- `ble_gattc_disc_all_dscs` starting at
   `s_rx_handle` (NimBLE internally adds 1, so the search begins at
   `s_rx_handle + 1` where the CCCD lives).  Finds the RX CCCD (0x2902).
5. **Write Client Supported Features** -- handle 0x0008, value `0x05`
   (UUID 0x2B29).
6. **Subscribe to Service Changed** -- write `0x0200` to handle 0x0004
   (Service Changed CCCD, UUID 0x2A05).
7. **Read characteristics by handle** -- Device Name (0x2A00), Appearance
   (0x2A01), three Steehl vendor characteristics.  Uses ATT Read Request
   (opcode 0x0A), not Read By Type.
8. **Re-write Service Changed CCCD** -- BlueZ does this twice in the reference
   trace; we replicate it.
9. **Enable notifications** -- write `0x0100` to RX CCCD.

**RPC flow:**

1. **StartKeyExchange** -- send client public key (256-byte hex).  AS11
   responds with `serverPk` and `salt`.  AS11 LCD shows a 4-digit passkey.
2. **ConfirmKeyExchange** -- send SRP client proof (M1, 32-byte hex).  AS11
   responds with `clientId` and `serverConfirmation` (M2).  Verify M2 =
   H(pad(A) || M1 || K) per RFC 5054.
3. On success, save `clientId` + master pair key (K) to NVS.

### 4.3 Outputs / data formats

- **FIG framing:** 4-byte sync (0xCAFEBABE LE), 8-byte header (VCID LE,
  payload length LE, payload CRC32 LE), 4-byte header CRC32 LE, then
  payload.  CRC32 uses IEEE polynomial (same as `binascii.crc32`).
- **RPC:** JSON-RPC 2.0 over FIG.  TX VCID = 0x0393, RX VCID = 0x0392.
- **Chunking:** FIG packets are sent as sequential Write Requests
  (opcode 0x12), chunked to `MTU - 3` bytes.  Do NOT use Write Long
  (Prepare+Execute) -- the AS11 rejects it with ATT error 3.
- **Credentials:** stored in NVS namespace `as11`: `client_id` (string),
  `master_pair_key` (hex string), `ble_addr`, `ble_name`.

### 4.4 Error handling & edge cases

- **Notification silently dropped:** If `CONFIG_BT_NIMBLE_GATT_SERVER` is
  disabled (which happens when `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=n`),
  NimBLE compiles out `ble_att_svr_rx_notify` from the ATT dispatch table.
  Incoming notifications are received at the HCI layer but never generate
  `BLE_GAP_EVENT_NOTIFY_RX`.  **Fix:** enable both
  `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y` and
  `CONFIG_BT_NIMBLE_GATT_SERVER=y` in `sdkconfig.defaults`, even though
  the device never advertises.  See the file header comment in
  `main/as11_ble.c` for details.
- **Stale sdkconfig:** `sdkconfig.defaults` changes do not take effect if
  a generated `sdkconfig` file already exists.  Delete `sdkconfig` before
  rebuilding when changing Kconfig defaults.
- **Descriptor discovery start handle:** `ble_gattc_disc_all_dscs` adds 1
  to the start handle internally.  Pass `s_rx_handle` (not
  `s_rx_handle + 1`) to begin searching at `s_rx_handle + 1`.
- **MTU exchange timeout:** The AS11 may initiate MTU exchange before our
  `ble_gattc_exchange_mtu` call, so the `on_mtu` callback never fires.
  `s_mtu` is still updated by `BLE_GAP_EVENT_MTU`.  Use a short timeout
  (500 ms) instead of 5 s.
- **HeartBeat notifications:** The AS11 sends periodic `HeartBeat` JSON-RPC
  notifications (method but no id).  These are filtered out in
  `handle_notify()` during pairing.

## 5. Acceptance criteria

- [x] Device scans for and discovers AirSense 11 by name prefix.
- [x] GATT discovery completes and captures TX/RX/CCCD handles.
- [x] `StartKeyExchange` RPC is sent and response (serverPk + salt) is
      received via notification.
- [x] AS11 displays 4-digit passkey on its LCD.
- [x] `ConfirmKeyExchange` RPC is sent and response (clientId +
      serverConfirmation) is received and verified.
- [x] Pairing credentials are saved to NVS.
- [ ] Subsequent reconnection using stored credentials succeeds.
- [ ] Therapy data can be pulled and exported to EDF.

## 6. Security / privacy considerations

- Handles personal medical therapy data.  Never put real patient data in
  issues, tests, or fixtures.
- SRP-6a key exchange uses a 2048-bit modulus.  The 4-digit passkey is
  shown on the AS11 LCD and must be entered by the user.
- The master pair key (K) is stored in NVS on the ESP32.  Consider flash
  encryption for production builds.
- BLE link-layer encryption is not used; security relies on the
  application-layer SRP exchange.

## 7. Open questions

- Can the HeartBeat notification interleaving with multi-chunk FIG
  responses cause reassembly buffer corruption?  (Likely not an issue in
  practice since HeartBeats are ~5 s apart and responses arrive in <2 s.)
- Should the connection be kept alive after pairing, or re-established on
  demand for data sync?

## 8. Changelog

- 2026-06-17: Initial draft (stub).
- 2026-06-24: Filled in with implementation details after successful
  pairing.  Documented the NimBLE GATT_SERVER configuration requirement,
  GATT discovery sequence, FIG framing, and SRP-6a flow.
