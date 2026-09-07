#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 scripts/therapy_alert_ack_contract_test.py
python3 scripts/clock_snapshot_contract_test.py
python3 scripts/capacity_snapshot_behavior_test.py
python3 scripts/upload_progress_snapshot_contract_test.py
python3 scripts/idf_async_allocation_test.py
python3 scripts/oximetry_forget_tombstone_contract_test.py
python3 scripts/psram_task_lifecycle_contract_test.py
python3 scripts/backend_runtime_test.py
