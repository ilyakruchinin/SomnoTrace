#!/usr/bin/env bash
# Headless boot smoke test with board, framebuffer and firmware identity checks.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
for ARG in "$@"; do
    if [ "${ARG}" = "--help" ] || [ "${ARG}" = "-h" ]; then
        echo "Usage: $0 [--board 154|7b] [--build] (default: 7b)"
        exit 0
    fi
done
PROFILE="$(python3 "${SCRIPT_DIR}/qemu_targets.py" test "$@")"
IFS=$'\t' read -r BOARD BUILD_DIR SDKCONFIG DEFAULTS BUILD_REQUEST CLEAN_REQUEST <<< "${PROFILE}"
FLASH="${PROJECT_DIR}/${BUILD_DIR}/qemu_flash.bin"
EFUSE="${PROJECT_DIR}/${BUILD_DIR}/qemu_efuse.bin"
if [ "${BUILD_REQUEST}" = "1" ] || [ ! -f "${FLASH}" ] || [ ! -f "${EFUSE}" ]; then
    "${SCRIPT_DIR}/build-qemu.sh" --board "${BOARD}"
fi
python3 "${SCRIPT_DIR}/qemu-artifacts.py" verify --board "${BOARD}"
QEMU_BIN="$("${SCRIPT_DIR}/setup-qemu-macos.sh")"
exec python3 "${SCRIPT_DIR}/qemu_boot_smoke.py" --board "${BOARD}" --qemu "${QEMU_BIN}"
