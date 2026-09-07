#!/usr/bin/env bash
# Executed by the ESP-IDF Docker entrypoint, after IDF_PATH is established.
set -euo pipefail
python3 /project/scripts/idf-sdk-patches.py
exec "$@"
