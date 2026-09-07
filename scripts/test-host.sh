#!/usr/bin/env bash
set -euo pipefail
TEST_DIR="$(mktemp -d /tmp/somnotrace-feature-host.XXXXXX)"
trap 'rm -rf "${TEST_DIR}"' EXIT
cd "$(dirname "$0")/.."
python3 scripts/gen_tz_db.py
python3 scripts/therapy_alert_ack_contract_test.py
python3 scripts/clock_snapshot_contract_test.py
python3 scripts/capacity_snapshot_behavior_test.py
python3 scripts/upload_progress_snapshot_contract_test.py
python3 scripts/idf_async_allocation_test.py
python3 scripts/oximetry_forget_tombstone_contract_test.py
python3 scripts/psram_task_lifecycle_contract_test.py
python3 scripts/backend_runtime_test.py
python3 scripts/session_storage_behavior_test.py
python3 scripts/session_checkpoint_barrier_test.py
python3 scripts/rapid_session_lifecycle_contract_test.py
python3 scripts/session_writer_start_resilience_contract_test.py
python3 scripts/sd_recording_arbitration_contract_test.py
python3 scripts/oximetry_sd_lease_contract_test.py
python3 scripts/sd_mount_fallback_contract_test.py
python3 scripts/backend_recording_test.py
python3 scripts/therapy_lifecycle_race_contract_test.py
python3 scripts/therapy_gate_behavior_test.py
python3 scripts/storage_export_fault_test.py
python3 scripts/edf_rebuild_behavior_test.py
python3 scripts/pending_export_behavior_test.py
python3 scripts/pending_export_service_test.py
python3 scripts/upload_invalidation_behavior_test.py
python3 scripts/backend_realtime_test.py
python3 scripts/uploader_ox_lease_contract_test.py
python3 scripts/fork_backend_reconciliation_test.py

./scripts/test-platform4.sh

./scripts/test-platform5.sh

python3 scripts/font_asset_contract_test.py
PLATFORM6_TEST_DIR=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror -I main scripts/live_flow_plot_test.c -o "$PLATFORM6_TEST_DIR/flow"
"$PLATFORM6_TEST_DIR/flow"
rm -rf "$PLATFORM6_TEST_DIR"
python3 scripts/live_flow_units_contract_test.py
python3 scripts/storage_status_memory_contract_test.py
python3 scripts/screen_timeout_contract_test.py

python3 scripts/log_stream_retained_contract_test.py

python3 scripts/logs_retained_behavior_test.py

python3 scripts/log_stream_recent_contract_test.py

python3 scripts/log_stream_resilience_contract_test.py

python3 scripts/logs_touch_ui_contract_test.py

python3 scripts/touch_logs_ui_contract_test.py


cc -std=c11 -Wall -Wextra \
    -I scripts/test_include -I main \
    scripts/first_run_setup_test.c main/first_run_setup_model.c \
    -o "${TEST_DIR}/first_run_setup_test"
"${TEST_DIR}/first_run_setup_test"

cc -std=c11 -Wall -Wextra -DTIMEZONE_CATALOG_HOST_TEST \
    -I scripts/test_include -I main \
    scripts/timezone_catalog_test.c main/timezone_catalog.c \
    -o "${TEST_DIR}/timezone_catalog_test"
"${TEST_DIR}/timezone_catalog_test"

python3 scripts/first_run_setup_contract_test.py

python3 scripts/first_run_setup_ui_contract_test.py

python3 scripts/first_run_setup_runtime_contract_test.py

python3 scripts/first_run_setup_lifecycle_contract_test.py

python3 scripts/timezone_catalog_contract_test.py

python3 scripts/netprov_scan_contract_test.py


cc -std=c11 -Wall -Wextra -DTOUCH_HISTORY_MODEL_TEST \
    -I scripts/test_include -I main \
    scripts/touch_history_model_test.c main/touch_history.c -lm \
    -o "${TEST_DIR}/touch_history_model_test"
"${TEST_DIR}/touch_history_model_test"

python3 scripts/history_storage_lifecycle_contract_test.py

python3 scripts/history_trace_channels_contract_test.py

python3 scripts/touch_history_service_contract_test.py

cc -std=c11 -Wall -Wextra -Werror -D_DARWIN_C_SOURCE -D_POSIX_C_SOURCE=200809L -I main \
    scripts/history_flow_cache_test.c main/history_flow_cache.c -o "${TEST_DIR}/history_flow_cache_test"
"${TEST_DIR}/history_flow_cache_test"

cc -std=c11 -Wall -Wextra -Werror -DHISTORY_CACHE_HOST_TEST -I main \
    scripts/history_cache_test.c main/history_cache.c -lpthread -o "${TEST_DIR}/history_cache_test"
"${TEST_DIR}/history_cache_test"

python3 scripts/history_generation_test.py

python3 scripts/history_probe_cancellation_test.py

python3 scripts/history_flow_io_test.py

python3 scripts/history_service_cache_test.py

python3 scripts/history_flow_envelope_contract_test.py


python3 scripts/history_progressive_test.py

python3 scripts/touch_history_stress_test.py

python3 scripts/history_responsiveness_contract_test.py

python3 scripts/touch_history_ui_contract_test.py

python3 scripts/clinical_ui_language_contract_test.py

python3 scripts/touch_history_controller_contract_test.py

python3 scripts/touch_history_integration_contract_test.py


python3 scripts/uploader_config_persistence_test.py

python3 scripts/manage_config_behavior_test.py

python3 scripts/manage_config_touch_test.py

python3 scripts/upload_test_ownership_contract_test.py

echo "All synthetic host tests passed"
