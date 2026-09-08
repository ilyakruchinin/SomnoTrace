# Security Policy

## Supported Versions

SomnoTrace is actively maintained. Security updates are applied to the latest
release on the `main` branch.

| Version | Supported          |
| ------- | ------------------ |
| >= 0.7  | :white_check_mark: |
| < 0.7   | :x:                |

## Reporting a Vulnerability

SomnoTrace interacts with medical devices over wireless protocols (Bluetooth Low
Energy) and handles local Wi-Fi / network communication. We take the security
and privacy of these interfaces seriously.

If you discover a security vulnerability or sensitive data issue:

1. **Do not** report security vulnerabilities via public GitHub issues or
   discussions.
2. Please report the issue privately using **GitHub Private Vulnerability
   Reporting** on this repository (under the **Security** tab).
3. Alternatively, contact the maintainer directly via
   [github.com/ilyakruchinin](https://github.com/ilyakruchinin).

### What to include in your report

- A description of the vulnerability and its potential impact.
- Clear steps or a proof-of-concept to reproduce the issue.
- Details of the device firmware version and hardware setup used.

### Response timeline

- **Initial response:** Within 48 hours.
- **Status update / triage:** Within 7 business days.
- **Fix / Disclosure:** We will coordinate with you on a patch and a responsible
  disclosure timeline.
