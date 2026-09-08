#!/usr/bin/env python3
# SomnoTrace - Build-time timezone database fetcher
# Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
#
# This file is part of SomnoTrace.
#
# SomnoTrace is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
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
"""Download the POSIX timezone database (zones.json) from nayarsystems/posix_tz_db.

This script runs at build time to fetch the IANA-to-POSIX-TZ mapping and save
it as main/zones.json, which is then embedded into firmware via
target_add_binary_data(). The embedded JSON is served to the web UI via the
/api/tz endpoint, allowing timezone selection without internet connectivity.

Source: https://github.com/nayarsystems/posix_tz_db (MIT License)
"""

import os
import sys
import urllib.request

URL = "https://raw.githubusercontent.com/nayarsystems/posix_tz_db/master/zones.json"
OUTPUT = os.path.join(os.path.dirname(__file__), "..", "main", "zones.json")


def main():
    out = os.path.normpath(OUTPUT)
    if os.path.exists(out):
        print(f"gen_tz_db: {out} already exists, skipping download")
        return 0

    print(f"gen_tz_db: downloading {URL}")
    try:
        urllib.request.urlretrieve(URL, out)
    except Exception as e:
        print(f"gen_tz_db: FAILED to download: {e}", file=sys.stderr)
        print("gen_tz_db: Run manually: python3 scripts/gen_tz_db.py", file=sys.stderr)
        return 1

    print(f"gen_tz_db: saved to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
