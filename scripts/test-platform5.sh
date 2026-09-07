#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 scripts/qemu_ui_contract_test.py
python3 scripts/qemu_worktree_test.py
