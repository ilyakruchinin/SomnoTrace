# 0008 — Device configuration & network connection lifecycle

- **Status:** Accepted
- **Author(s):** Ilya Kruchinin (@ilyakruchinin)
- **Created:** 2026-06-17
- **Last updated:** 2026-06-17
- **Related specs:** `0006-device-provisioning.md`, `0007-button-controls.md`

## 1. Summary

Defines (a) the persistent device configuration stored in internal flash (NVS),
and (b) the boot-time network connection lifecycle: which Wi-Fi network to join,
roaming rules, the connection-failure fallback, and entry into SoftAP captive
portal mode. The configuration **web UI** itself is out of scope (see `0006`);
this spec covers the *logic* that governs real-world operation.

## 2. Motivation / goals

- The device must operate headlessly at a bedside, reconnecting reliably to
  known Wi-Fi without user interaction.
- All operational settings must survive reboots and power loss.
- When networking cannot be established, the device must offer a discoverable,
  cross-platform way to (re)configure Wi-Fi.

## 3. Non-goals

- The provisioning web UI / page content and the configuration HTTP API
  (covered by `0006-device-provisioning.md`).
- The button gesture that forces SoftAP entry (covered by
  `0007-button-controls.md`).
- BLE data-sync behaviour (covered by `0001`, `0003`).

## 4. Behaviour

### 4.1 Stored configuration (NVS)

All persistent configuration lives in NVS. The following fields are defined
(values are illustrative; exact keys/namespaces are an implementation detail):

| Group | Fields |
|-------|--------|
| **Wi-Fi** | Up to **4** entries, each `{ ssid, password }`. Order is priority for tie-breaking only (see 4.3). |
| **AirSense 11 BLE** | Pairing / bonding information needed to reconnect to a previously paired ResMed AirSense 11. |
| **O2 Ring BLE** | Pairing / bonding information needed to reconnect to a Wellue / O2 Ring device. |
| **SMB target** | `{ server, share/path, username, password }`. |
| **SleepHQ** | `{ access_token, access_secret }`. |
| **Device** | `hostname` — defaults to `SomnoTrace` when unset/empty. |

Notes:

- `hostname` is used for the mDNS responder and as the basis of the SoftAP SSID
  (see 4.5).
- Secrets (Wi-Fi passwords, SMB password, SleepHQ token/secret, BLE bonding
  keys) are sensitive — see §6.

### 4.2 Boot flow (overview)

1. Latch power (BAT_EN) and start the button monitor (per `0007`).
2. Load configuration from NVS.
3. Attempt to join a configured Wi-Fi network (4.3).
4. On success → normal operation (start mDNS, services).
5. On failure → enter SoftAP captive portal mode (4.5).
6. At any time, a 5 s BOOT long-press forces SoftAP mode (per `0007`).

### 4.3 Wi-Fi network selection & connection

1. Perform a full scan of nearby APs.
2. Build the candidate set = scan results whose SSID matches **any** of the
   up-to-4 configured SSIDs.
3. If the candidate set is empty → go to SoftAP fallback (4.5).
4. Order the matching SSIDs into a **candidate list**, strongest signal first
   (by the highest RSSI of each SSID's strongest BSSID). Configured slot order
   (slot 1 = "home") is used **only as a tie-breaker** for equal signal.
   - If a single SSID is broadcast by multiple APs/BSSIDs (e.g. a Wi-Fi mesh —
     same SSID, multiple BSSIDs), connect to the BSSID with the strongest
     signal.
5. For each candidate SSID in order, attempt to connect with **3 attempts** and
   a **5-second delay** between attempts.
6. If a candidate connects, that SSID becomes the pinned target. If it fails all
   3 attempts, move to the next candidate. When **all** candidates are exhausted
   → go to SoftAP fallback (4.5).

> In practice only one configured SSID is expected to be reachable at a time
> (the 4 slots cover different households), so the candidate list is usually a
> single entry.

Once connected, the **target SSID is fixed for the remainder of this power
cycle** — the device must not switch to a different SSID even if another
configured SSID later appears stronger.

### 4.4 Roaming (within a connected SSID)

- The device **may roam between BSSIDs of the same SSID** (e.g. mesh nodes) to
  follow the strongest signal / maintain connectivity.
- The device **must not roam to a different SSID**; SSID is pinned at first
  successful connection (4.3 step 6) until reboot or explicit reconfiguration.
- If the connection to the pinned SSID is fully lost (e.g. a power blip at the
  AP), the device **retries indefinitely** within that SSID (re-scanning its
  BSSIDs) and does **not** fall back to SoftAP. A drop during normal operation
  is treated as transient and expected to recover on its own.

### 4.5 SoftAP captive portal mode

Entered when (a) no configured SSID is found, (b) every matching candidate SSID
fails its 3 connection attempts, or (c) the user forces it via BOOT long-press
(`0007`).

1. Start Wi-Fi in an **open** (no-password) **SoftAP** mode.
   - SoftAP **SSID** = `${hostname}-setup` (default `SomnoTrace-setup`).
2. Assign the device a fixed gateway IP for the AP (conventionally
   `192.168.4.1`).
3. Do **not** run mDNS in SoftAP mode; clients reach the portal via the fixed
   gateway IP and the DNS hijack below. (`.local` resolution is unreliable on
   mobile OSes during captive-portal onboarding.)
4. Start a **DNS server** that answers **all** queries with the SoftAP IP
   (wildcard / DNS hijack) so any hostname resolves to the portal.
5. Start the HTTP server and respond to the OS captive-portal probes (4.6) with
   a redirect to the configuration portal, triggering the "Sign in to network"
   prompt on the client.
6. The portal lets the user set the up-to-4 Wi-Fi SSIDs/passwords (UI is `0006`).
7. After credentials are saved, the device re-attempts connection (4.3) — by
   reboot or in-place transition (must respect the BOOT-strap caveat in `0007`).
8. If no configuration is saved within a **10-minute idle timeout**, the device
   leaves SoftAP and re-attempts the STA flow (4.3); if that still fails it
   returns to SoftAP. This avoids being stuck in an open AP indefinitely after a
   transient outage that has since recovered.

### 4.6 Captive-portal probe endpoints to intercept

To trigger automatic captive-portal detection across platforms, the DNS hijack
(4.5 step 4) catches all lookups, and the HTTP server returns an HTTP **302**
redirect to the portal for these probe URLs instead of their expected success
response:

| Platform | Probe URL | Normal "online" response |
|----------|-----------|--------------------------|
| **Apple (iOS/macOS)** | `http://captive.apple.com/hotspot-detect.html` | `200` with body containing `Success` |
| **Android / Chrome** | `http://connectivitycheck.gstatic.com/generate_204`, `http://clients3.google.com/generate_204`, `http://www.google.com/generate_204` | `204 No Content` |
| **Windows** | `http://www.msftconnecttest.com/connecttest.txt` (`200` body `Microsoft Connect Test`); `http://www.msftncsi.com/ncsi.txt` (`200` body `Microsoft NCSI`) | as noted |
| **Firefox** | `http://detectportal.firefox.com/success.txt` | `200` body `success` |
| **Linux (NetworkManager/GNOME)** | `http://nmcheck.gnome.org/check_network_status.txt`, `http://network-test.debian.org/nm`, `http://connectivity-check.ubuntu.com` | `200` expected text / `204` |

Implementation note: because the DNS server resolves everything to the SoftAP
IP, any of the above hostnames reaches our HTTP server; returning a 302 (or the
non-matching body) is what signals "captive portal present" to the client OS.
Probe lists evolve over time — treat this table as the maintained reference and
update as needed.

## 5. Acceptance criteria

- [ ] All listed configuration groups persist in NVS across reboot/power loss.
- [ ] `hostname` defaults to `SomnoTrace` when unset.
- [ ] On boot, the device scans and connects to a configured SSID if present.
- [ ] When multiple configured SSIDs are visible, the strongest is chosen.
- [ ] When one SSID has multiple BSSIDs (mesh), the strongest BSSID is chosen.
- [ ] The device roams between BSSIDs of the connected SSID but never switches
      SSID within a power cycle.
- [ ] Each candidate SSID is attempted 3× with 5 s spacing before moving on.
- [ ] With no configured SSID found, or after all candidates fail, the device
      enters SoftAP mode.
- [ ] A connection drop during normal operation triggers indefinite retry, not
      SoftAP.
- [ ] SoftAP exits and re-attempts STA after 10 minutes of no configuration.
- [ ] SoftAP SSID is `${hostname}-setup` and the AP is open.
- [ ] In normal operation the device advertises `${hostname}.local` via mDNS;
      SoftAP mode does **not** run mDNS and instead hijacks DNS to the portal.
- [ ] Captive-portal auto-detection triggers on iOS, Android, Windows, and
      macOS/Linux using the documented probe endpoints.

## 6. Security / privacy considerations

- NVS stores multiple secrets (Wi-Fi passwords, SMB credentials, SleepHQ
  token/secret, BLE bonding keys). Strongly recommend an **encrypted NVS
  partition** backed by **flash encryption** so secrets are not recoverable from
  a dumped flash image.
- The SoftAP portal is intentionally **open/unauthenticated** (chosen for the
  lowest-friction onboarding); exposure is bounded by the entry conditions, the
  deliberate 5 s BOOT hold, and the **10-minute idle timeout** (4.5).
- This device handles personal medical data; never log secrets or patient data.

## 7. Decisions

Previously-open questions, now resolved:

- **Attempt counting:** 3 attempts × 5 s are **per candidate SSID**. Candidates
  are tried strongest-signal-first; SoftAP is entered only once **all** matching
  candidates are exhausted.
- **SSID priority:** selection is **signal-strength first**; configured slot
  order (slot 1 = home) is only a tie-breaker for equal RSSI.
- **Runtime connection loss:** **retry indefinitely** within the pinned SSID;
  never fall back to SoftAP for a drop during normal operation.
- **SoftAP idle timeout:** **10 minutes**, then re-attempt STA (4.5 step 8).
- **SoftAP SSID naming:** `${hostname}-setup` (default `SomnoTrace-setup`).
- **AP security:** **open** SoftAP, for lowest onboarding friction.
- **mDNS:** enabled in **STA / normal operation only** (`${hostname}.local`);
  **not** used in SoftAP mode.
