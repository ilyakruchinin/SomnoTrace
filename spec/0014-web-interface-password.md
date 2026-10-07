# 0014 — Optional web interface password

- **Status:** Implemented
- **Author(s):** Matthias Niedermaier
- **Created:** 2026-10-07
- **Last updated:** 2026-10-07
- **Related specs:** `0008-config-and-network-lifecycle.md`, `0010-web-ui-architecture-and-design.md`, `0011-web-api-endpoints.md`

## 1. Summary

An optional password for the HTTP web interface. It is **off by default**:
nothing changes for a device where no password was ever set. Once set, every
page and API endpoint served by the device's web server, except the static
app shell and the login endpoints, requires either a browser session (cookie)
or the password sent with HTTP Basic authentication (Home Assistant, `curl`).

## 2. Motivation / goals

- The web server exposes therapy and oximetry data, log streams, the AirSense
  11 RPC bridge (start/stop therapy), SD-card formatting and firmware updates
  (`/api/ota`, `/api/ota-url`) to anyone on the local network. Shared, guest or
  poorly segmented networks make that a real exposure.
- Keep the zero-friction default for users who do not need it.
- Keep Home Assistant and scripted API use working with a password set.

## 3. Non-goals

- The FTP server, MQTT and BLE: they have their own credentials (or none) and
  are configured separately.
- Transport encryption. The device serves plain HTTP (see §6).
- Multiple users or roles. There is one password.
- Password recovery without physical access (see §4.7).

## 4. Behaviour

### 4.1 Scope: default-deny

Every route is registered through `web_auth_register()`, which wraps its
handler in an authentication gate. Only these routes are public
(`web_auth_register_public()`):

| Route | Why public |
|-------|------------|
| `/`, `/wifi` | The app shell; it renders the login screen. No data. |
| `/manifest.json`, `/sw.js`, `/uplot.js`, `/uplot.css`, `/logo.svg`, `/favicon.svg` | Static assets. |
| `/api/tz` | Public IANA timezone table. |
| `/api/auth/status`, `/api/auth/login` | Needed to sign in. |
| SoftAP captive-portal probes and the SoftAP 404 redirect | Redirect only. |

`scripts/lint.sh` fails if anything outside `main/web_auth.c` calls
`httpd_register_uri_handler()`, so a new endpoint cannot skip the gate.

The gate applies in **SoftAP mode too**. Otherwise anyone could get around the
password by forcing the device into its open setup hotspot (jamming or
deauthenticating its Wi-Fi), because the SoftAP portal serves the same API.

### 4.2 Credentials accepted by the gate

1. **Session cookie** `st_session`, set by `POST /api/auth/login`:
   `HttpOnly; SameSite=Strict; Path=/; Max-Age=30 days`.
2. **HTTP Basic**: `Authorization: Basic base64(user:password)`. The user name
   is ignored. The gate never sends `WWW-Authenticate`, so browsers never show
   their own dialog or cache Basic credentials. That cache would otherwise be
   attached to cross-site requests.

Refusals: `401 {"error":"auth_required"}`, `429 {"error":"throttled"}` with
`Retry-After`, `403 {"error":"cross_origin"}`; all `Cache-Control: no-store`.
A refused request with a body larger than 512 bytes (e.g. a firmware upload)
has its connection closed instead of drained. A refused WebSocket upgrade is
closed straight after the handshake.

### 4.3 Endpoints

| Endpoint | Auth | Body | Result |
|----------|------|------|--------|
| `GET /api/auth/status` | public | – | `{"enabled":bool,"authenticated":bool}` (cookie only) |
| `POST /api/auth/login` | public | `{"password":"…"}` | `200` + cookie · `401 wrong_password` · `429` |
| `POST /api/auth/logout` | required | – | Rotates the signing key: **every** browser session ends. Clears the cookie. |
| `POST /api/auth/password` | required | `{"current":"…","new":"…"}` | Set (`new` 8–64 bytes, no control characters), change, or remove (`new` = `""`). `current` is required whenever a password is set, even with a valid session. `403 wrong_password` · `400 weak_password` · `429`. |

Setting, changing or removing the password rotates the signing key (ending
all sessions), closes every other open HTTP/WebSocket connection, and issues
a fresh cookie to the caller.

### 4.4 Storage

NVS namespace `webauth`, written only through the `nvs_writer` task:

- `pw`: `{version=1, iterations, salt[16], PBKDF2-HMAC-SHA256(password)[32]}`.
  The password itself is never stored, logged or returned.
- `skey`: 32-byte random session-signing key.

A record that exists but cannot be read or validated **fails closed**: the
interface stays locked rather than falling back to open.

PBKDF2 uses 10 000 iterations for new passwords. The count is stored per
record, so it can be raised later without invalidating existing passwords.
The device logs how long hashing took.

### 4.5 Session tokens

Stateless: `hex(flags | expiry | nonce[16] | HMAC-SHA256(skey, …))`.

- If the wall clock is plausible (≥ 2025, i.e. NTP has run since power-on;
  the RTC keeps time across software reboots), the expiry is Unix time. The
  session survives the reboot that follows a settings save.
- Otherwise the expiry is in seconds of uptime, and the MAC also covers a
  random per-boot ID, so the token dies with the boot.
- The maximum lifetime is 30 days. Tokens with a later expiry are rejected.

### 4.6 Brute force and request forgery

- **Rate limit.** Applies to logins, password re-entry and Basic auth. The
  first 5 consecutive wrong passwords are free; each later one doubles the
  wait (1 s, 2 s, 4 s …), capped at 5 minutes. A success resets it. The
  limiter is global rather than per IP: a LAN attacker can change IP
  trivially. A browser with a valid session, and an API client whose
  password already verified this boot, are not affected by the lockout.
- **CSRF.** `SameSite=Strict` keeps the cookie off cross-site requests. As
  defence in depth, any cookie-authenticated request other than GET/HEAD, any
  WebSocket upgrade, and the login must carry an `Origin` that matches the
  `Host` header when the browser sends one.
- **DNS rebinding.** A rebound origin has no cookie and no cached Basic
  credentials, so it only ever sees 401.
- **Clickjacking.** `/` is served with `X-Frame-Options: DENY` and
  `X-Content-Type-Options: nosniff`.
- **Browser cache.** The service worker caches only the public app shell.
  Data, files and API answers never land in the browser's Cache Storage,
  where they would outlive a logout.

### 4.7 Recovery

A forgotten password cannot be reset over the network. Erasing the NVS
partition over USB (e.g. `esptool.py erase_region 0x9000 <nvs size>`, or a
full `erase_flash` + reflash) clears it, together with all other settings.

### 4.8 Web UI

- With a password set and no valid session, the page shows only a login
  screen. Any later `401` from a same-origin request (session expired, signed
  out elsewhere, password changed) brings the login screen back, and the log
  WebSocket stops reconnecting.
- **Settings → Web Interface Password**: set, change or remove the password,
  and "Sign out on all devices". The card is hidden in SoftAP mode: the
  password should not be chosen over an open hotspot. A password that was
  already set is still enforced there.

## 5. Acceptance criteria

Checked items are verified by the firmware build, host tests or lint. The
rest are implemented but still need verifying on hardware.

- [ ] Without a stored password, every endpoint behaves exactly as before.
- [ ] With a password, every non-public route answers 401 without credentials,
      in both STA and SoftAP mode.
- [ ] Browser login sets an `HttpOnly; SameSite=Strict` cookie. Logout and
      password changes invalidate all existing cookies.
- [ ] API clients (Home Assistant `rest`, `curl -u`) authenticate with HTTP Basic.
- [ ] PBKDF2 at 10 000 iterations takes well under a second on the ESP32-S3
      (see the `web_auth` log line when a password is set).
- [x] Passwords are stored only as salted PBKDF2-HMAC-SHA256.
- [x] Repeated wrong passwords are throttled up to 5 minutes per attempt.
- [x] No route can be registered without going through the gate (`lint.sh`).
- [x] Host tests: PBKDF2 against published vectors, token forgery and expiry,
      Basic/Origin parsing, limiter (`scripts/web_auth_test.c`).

## 6. Security / privacy considerations

- **Plain HTTP.** The password (at login, or in every Basic-auth request) and
  the session cookie cross the network unencrypted. WPA2/3 protects them on
  the air from outsiders, but anyone who can watch the LAN's traffic (an
  ARP-spoofing host, a compromised router, any SoftAP client) can capture
  them. The password protects against casual access by other people on the
  network, not against an active network attacker. Users should pick a
  password they use nowhere else.
- **SoftAP is an open network.** Signing in over the setup hotspot exposes the
  password to anyone in radio range.
- **Flash is not encrypted** (no NVS/flash encryption in `sdkconfig.defaults`).
  With physical access the hash and signing key can be read. The hash is
  salted PBKDF2, but a short password can still be brute-forced offline.
  Physical access also allows reflashing, so this is in line with the other
  secrets in NVS (§6 of `0008`).
- Logged events: login (with client IP), wrong password (client IP + count),
  password set/changed/removed (client IP). Never the password.
- Out of scope but worth knowing: the FTP server, if enabled in anonymous
  mode, still exposes the SD card without this password.

## 7. Open questions

- A physical reset gesture for a forgotten password (e.g. a second BOOT long
  press while in SoftAP mode) would avoid the full NVS erase.
- HTTPS with a device-generated certificate would close the plain-HTTP gap,
  at the cost of browser certificate warnings and internal RAM.

## 8. Changelog

- 2026-10-07: Initial version, implemented.
