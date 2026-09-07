#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
PLATFORM_TEST_DIR=$(mktemp -d)
trap 'rm -rf "$PLATFORM_TEST_DIR"' EXIT
cc -std=c11 -Wall -Wextra -Werror -I main scripts/touch_observation_test.c main/touch_observation.c -o "$PLATFORM_TEST_DIR/touch"
"$PLATFORM_TEST_DIR/touch"
cc -std=c11 -Wall -Wextra -DCONTROLLER_DIAGNOSTICS_HOST_TEST -I scripts/test_include -I main scripts/controller_diagnostics_test.c main/controller_diagnostics.c -o "$PLATFORM_TEST_DIR/diagnostics"
"$PLATFORM_TEST_DIR/diagnostics"
python3 scripts/compact_display_init_test.py
python3 scripts/rgb_frame_handoff_test.py
python3 scripts/touch_recovery_test.py
python3 scripts/waveshare_7b_contract_test.py
