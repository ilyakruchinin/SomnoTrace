# ResMed AirSense 11 BLE RPC → Wi-Fi Bridge

> SomnoTrace includes a built-in JSON-RPC bridge that allows advanced users, developers, and home automation systems to query and control a paired ResMed AirSense 11 machine directly over local Wi-Fi via simple HTTP `POST` requests.

---

## Overview

When SomnoTrace is paired with an AirSense 11, it maintains an active, encrypted Bluetooth Low Energy (BLE) link secured via SRP-6a and AES-256.

The **RPC Bridge** exposes this secure connection to your local network through a single HTTP endpoint:

```http
POST http://somnotrace.local/api/ble/passthrough
Content-Type: application/json
```

Any standard JSON-RPC command sent to this endpoint is automatically encrypted by SomnoTrace, forwarded over BLE to the AirSense 11, and the machine's decrypted response is returned directly in the HTTP response.

---

## Prerequisites

1. SomnoTrace is powered on and connected to your local Wi-Fi.
2. The AirSense 11 is paired with SomnoTrace and within Bluetooth range (~5–10 meters).
3. The machine's BLE session is active (the mask icon is illuminated or visible on the SomnoTrace screen).

---

### Web interface password

If a password is set for the web interface (**Settings → Web Interface Password**), the bridge requires it too. Send it with HTTP Basic authentication; the user name is ignored:

```bash
curl -u somnotrace:YOUR-PASSWORD -X POST http://somnotrace.local/api/ble/passthrough ...
```

Without credentials the bridge answers `401 Unauthorized`. See [spec 0014](../spec/0014-web-interface-password.md).

---

## Example Commands (`curl`)

### 1. Query Device Identity (`Get`)

Retrieve the machine model, hardware identifiers, and geographic region:

```bash
curl -X POST http://somnotrace.local/api/ble/passthrough \
  -H "Content-Type: application/json" \
  -d '{
    "id": 1,
    "jsonrpc": "1.0",
    "method": "Get",
    "params": [
      "ProductGeographicIdentifier",
      "HardwareIdentifier",
      "ProductIdentifier"
    ]
  }'
```

**Example Response:**
```json
{
  "id": 1,
  "jsonrpc": "1.0",
  "result": {
    "ProductGeographicIdentifier": "ANZ",
    "HardwareIdentifier": "39485",
    "ProductIdentifier": "AirSense 11 AutoSet"
  }
}
```

---

### 2. Query Machine Settings & Prescription (`Get`)

Read current therapy mode, pressure settings, and comfort features:

```bash
curl -X POST http://somnotrace.local/api/ble/passthrough \
  -H "Content-Type: application/json" \
  -d '{
    "id": 2,
    "jsonrpc": "1.0",
    "method": "Get",
    "params": [
      "TherapySetting-Mode",
      "TherapySetting-SetPressure",
      "TherapySetting-MinPressure",
      "TherapySetting-MaxPressure",
      "ComfortSetting-EPR-Enable",
      "ComfortSetting-EPR-Level"
    ]
  }'
```

---

### 3. Read Internal Device Clock (`GetDateTime`)

Check the machine's internal clock time:

```bash
curl -X POST http://somnotrace.local/api/ble/passthrough \
  -H "Content-Type: application/json" \
  -d '{
    "id": 3,
    "jsonrpc": "1.0",
    "method": "GetDateTime"
  }'
```

**Example Response:**
```json
{
  "id": 3,
  "jsonrpc": "1.0",
  "result": {
    "dateTime": "2026-08-20T21:30:00.000Z"
  }
}
```

---

### 4. Start Therapy Remotely (`EnterTherapy`)

Turn on the AirSense 11 blower and start a therapy session remotely:

```bash
curl -X POST http://somnotrace.local/api/ble/passthrough \
  -H "Content-Type: application/json" \
  -d '{
    "id": 4,
    "jsonrpc": "1.0",
    "method": "EnterTherapy"
  }'
```

**Example Response:**
```json
{
  "id": 4,
  "jsonrpc": "1.0",
  "result": null
}
```

---

### 5. Stop Therapy Remotely (`EnterStandby`)

Stop an active therapy session and place the machine into standby mode:

```bash
curl -X POST http://somnotrace.local/api/ble/passthrough \
  -H "Content-Type: application/json" \
  -d '{
    "id": 5,
    "jsonrpc": "1.0",
    "method": "EnterStandby"
  }'
```

---

### 6. Run Mask Fit Test (`EnterMaskFit`)

Trigger the AirSense 11 Mask Fit diagnostic routine:

```bash
curl -X POST http://somnotrace.local/api/ble/passthrough \
  -H "Content-Type: application/json" \
  -d '{
    "id": 6,
    "jsonrpc": "2.0",
    "method": "EnterMaskFit"
  }'
```

---

### 7. Pull Diagnostic Spool Data (`StartSpool` + `PullSpoolFragments`)

`PullSpoolFragments` is special: its direct RPC response is only an
acknowledgement — the actual payload arrives asynchronously as
`SpoolFragment` notifications on the same BLE characteristic.  The
passthrough endpoint arms a fragment collector whenever it detects a
`PullSpoolFragments` request, waits for the notifications, and returns
them inside the HTTP response.

Typical sequence (two calls, run back-to-back):

```bash
# 1. Open the spool — returns {"result":{"spoolId":123,...}}
curl -X POST http://somnotrace.local/api/ble/passthrough \
  -H "Content-Type: application/json" \
  -d '{"id":7,"jsonrpc":"1.0","method":"StartSpool",
       "params":{"spoolAddress":{"CellularActivityEvents":
         {"fromDateTime":"2026-06-25T00:00:00.000Z"}},"maxSpoolSize":100000}}'

# 2. Pull the fragments using that spoolId
curl -X POST http://somnotrace.local/api/ble/passthrough \
  -H "Content-Type: application/json" \
  -d '{"id":8,"jsonrpc":"1.0","method":"PullSpoolFragments",
       "params":{"spoolId":123,"maxFragmentSize":2808,"maxNotifications":0}}'
```

The second call returns the ack object **plus** the collected fragments:

```json
{
  "id": 8,
  "jsonrpc": "1.0",
  "result": null,
  "spoolFragments": [
    {"seq": 0, "len": 2808, "data": "<base64>"},
    {"seq": 1, "len": 1400, "data": "<base64>"}
  ],
  "spoolStatus": "SPOOL_COMPLETE_NO_MORE_DATA",
  "spoolHash": "<sha256-hex-of-concatenated-raw-data>"
}
```

Notes:

- `data` is base64, matching the BLE wire format; concatenate the decoded
  fragments in `seq` order to reconstruct the raw spool payload.
- `spoolHash` (when present) is the SHA-256 of that concatenation — verify
  it after reassembly.
- If the fragment wait hits the deadline, the response is returned with
  whatever arrived and `"spoolTimedOut": true`; if the response would
  exceed ~96 KB, excess fragments are dropped and `"spoolTruncated": true`
  is set.
- A `PullSpoolFragments` call holds the shared BLE RPC channel for its
  whole cycle, so it cannot interleave with the device's own post-therapy
  spool collection; while it runs, other passthrough requests fail fast
  with a busy/timeout error.

---

## Full Protocol Reference

For a complete catalog of supported AirSense 11 JSON-RPC methods, variable keys, and parameters discovered through reverse-engineering research, refer to:

👉 **[AirBreak-Plus RPC Protocol Documentation](https://github.com/m-kozlowski/airbreak-plus/blob/master/docs/as11/rpc_protocol.md)**
