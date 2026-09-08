#!/usr/bin/env python3
# SomnoTrace - Synthetic oximetry and SleepHQ contract test
# Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
#
# This file is part of SomnoTrace.
#
# SomnoTrace is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3, or (at your option) any later version.
#
# SomnoTrace is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
# A PARTICULAR PURPOSE. See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with
# this program. If not, see <https://www.gnu.org/licenses/>.
#
# ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
# attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
# (https://github.com/ilyakruchinin)." See the NOTICE file for details.

"""Run a no-patient O2 import contract test against SleepHQ.

The upload path is opt-in because it creates an account-side import. Credentials
are read only from SLEEPHQ_CLIENT_ID and SLEEPHQ_CLIENT_SECRET and are never
printed or written to disk.
"""

import argparse
import hashlib
import json
import os
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

BASE = "https://sleephq.com"


def synthetic_format_a(count=120):
    header = bytes((1, 3, 0, 0, 0, 0, 0, 0, 4, 0))
    records = b"".join(bytes((97 + i % 3, 70 + i % 10, i % 4)) for i in range(count))
    trailer = bytearray(48)
    trailer[4:8] = bytes.fromhex("48125ada")
    trailer[12:14] = struct.pack("<H", count)
    trailer[16:19] = bytes.fromhex("010103")
    trailer[34] = 97
    trailer[35] = 95
    return header + records + trailer


def synthetic_vld(count=120):
    header = bytearray(40)
    header[0] = 3
    header[1] = 0
    header[2:4] = struct.pack("<H", 2099)
    header[4:9] = bytes((1, 1, 0, 0, 0))
    header[9:13] = struct.pack("<I", 40 + count * 5)
    header[13:15] = struct.pack("<H", count * 4)
    records = b"".join(struct.pack("<BHBB", 97, 70, 1, 0) for _ in range(count))
    return bytes(header) + records


def request(method, path, token=None, body=None, content_type=None):
    headers = {"Accept": "application/vnd.api+json", "User-Agent": "SomnoTrace-contract-test/1.0"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if body is not None:
        headers["Content-Length"] = str(len(body))
        if content_type:
            headers["Content-Type"] = content_type
    req = urllib.request.Request(BASE + path, data=body, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        payload = error.read().decode("utf-8", "replace")
        try:
            payload = json.loads(payload)
        except json.JSONDecodeError:
            payload = {"error": payload[:400]}
        return error.code, payload


def multipart(fields, filename, payload):
    boundary = "----SomnoTraceContractBoundary"
    chunks = []
    for name, value in fields.items():
        chunks.extend((f"--{boundary}\r\n", f'Content-Disposition: form-data; name="{name}"\r\n\r\n', str(value), "\r\n"))
    chunks.extend((f"--{boundary}\r\n", f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n', "Content-Type: application/octet-stream\r\n\r\n"))
    body = "".join(chunks).encode() + payload + f"\r\n--{boundary}--\r\n".encode()
    return body, f"multipart/form-data; boundary={boundary}"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--upload", action="store_true", help="create and process a synthetic SleepHQ O2 import")
    parser.add_argument("--format", choices=("vld", "format-a"), default="vld", help="synthetic source format (default: vld)")
    args = parser.parse_args()
    payload = synthetic_vld() if args.format == "vld" else synthetic_format_a()
    name = "20990101000000.vld" if args.format == "vld" else "20990101000000"
    if not args.upload:
        print(json.dumps({"format": args.format, "synthetic_bytes": len(payload), "md5_filename_content": hashlib.md5(name.encode() + payload).hexdigest(), "upload": False}))
        return 0

    client_id = os.environ.get("SLEEPHQ_CLIENT_ID")
    client_secret = os.environ.get("SLEEPHQ_CLIENT_SECRET")
    if not client_id or not client_secret:
        print("SLEEPHQ_CLIENT_ID and SLEEPHQ_CLIENT_SECRET are required", file=sys.stderr)
        return 2
    form = urllib.parse.urlencode({"grant_type": "password", "client_id": client_id, "client_secret": client_secret, "scope": "read write"}).encode()
    status, auth = request("POST", "/oauth/token", body=form, content_type="application/x-www-form-urlencoded")
    if status < 200 or status >= 300 or "access_token" not in auth:
        print(f"authentication failed: HTTP {status}", file=sys.stderr)
        return 1
    token = auth["access_token"]
    status, me = request("GET", "/api/v1/me", token=token)
    data = me.get("data", {}) if isinstance(me, dict) else {}
    attrs = data.get("attributes", {}) if isinstance(data, dict) else {}
    team = attrs.get("current_team_id", data.get("current_team_id"))
    if status != 200 or team is None:
        print(f"team discovery failed: HTTP {status}", file=sys.stderr)
        return 1
    status, created = request("POST", f"/api/v1/teams/{team}/imports?o2=true", token=token)
    import_data = created.get("data", {}) if isinstance(created, dict) else {}
    import_id = import_data.get("id")
    if status != 201 or import_id is None:
        print(f"O2 import creation failed: HTTP {status}", file=sys.stderr)
        return 1
    digest = hashlib.md5(name.encode() + payload).hexdigest()
    body, content_type = multipart({"import_id": import_id, "name": name, "path": "/OXYMETRY/contract-test", "content_hash": digest}, name, payload)
    status, uploaded = request("POST", f"/api/v1/imports/{import_id}/files", token=token, body=body, content_type=content_type)
    if status != 201:
        print(f"O2 file upload failed: HTTP {status} import={import_id}", file=sys.stderr)
        return 1
    status, _ = request("POST", f"/api/v1/imports/{import_id}/process_files", token=token)
    if status < 200 or status >= 300:
        print(f"O2 processing request failed: HTTP {status} import={import_id}", file=sys.stderr)
        return 1
    for _ in range(30):
        status, result = request("GET", f"/api/v1/imports/{import_id}", token=token)
        attrs = result.get("data", {}).get("attributes", {}) if isinstance(result, dict) else {}
        state = attrs.get("status")
        if state in ("complete", "completed"):
            print(json.dumps({"status": state, "import_id": import_id, "file_bytes": len(payload)}))
            return 0
        if state in ("failed", "error"):
            print(json.dumps({"status": state, "import_id": import_id, "reason_present": bool(attrs.get("failed_reason"))}))
            return 1
        time.sleep(2)
    print(json.dumps({"status": "timeout", "import_id": import_id}))
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
