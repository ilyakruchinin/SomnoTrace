#!/usr/bin/env bash
# Launch the SomnoTrace ESP32-S3 UI preview in Espressif QEMU.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
for ARG in "$@"; do
    if [ "${ARG}" = "--help" ] || [ "${ARG}" = "-h" ]; then
        echo "Usage: $0 [--board 154|7b] [--build] (default: 7b)"
        exit 0
    fi
done
PROFILE="$(python3 "${SCRIPT_DIR}/qemu_targets.py" run "$@")"
IFS=$'\t' read -r BOARD BUILD_DIR SDKCONFIG DEFAULTS BUILD_REQUEST CLEAN_REQUEST <<< "${PROFILE}"

FLASH="${PROJECT_DIR}/${BUILD_DIR}/qemu_flash.bin"
EFUSE="${PROJECT_DIR}/${BUILD_DIR}/qemu_efuse.bin"
if [ "${BUILD_REQUEST}" = "1" ] || [ ! -f "${FLASH}" ] || [ ! -f "${EFUSE}" ]; then
    "${SCRIPT_DIR}/build-qemu.sh" --board "${BOARD}"
fi
python3 "${SCRIPT_DIR}/qemu-artifacts.py" verify --board "${BOARD}"
if command -v ps >/dev/null 2>&1; then
    # Fixed-string argv matching avoids treating worktree names as regexes.
    RUNNING_QEMU="$(ps -axo pid=,command= | awk -v flash="file=${FLASH},if=mtd,format=raw" \
        'index($0, "qemu-system-xtensa") && index($0, flash) && !index($0, "awk -v") {print $1}')"
    if [ -n "${RUNNING_QEMU}" ]; then
        echo "SomnoTrace QEMU is already running (PID ${RUNNING_QEMU//$'\n'/, })." >&2
        echo "Close that window or stop its terminal before starting another." >&2
        exit 1
    fi
fi
QEMU_BIN="$("${SCRIPT_DIR}/setup-qemu-macos.sh")"
# QEMU's disposable -snapshot overlays also belong to this checkout.
export TMPDIR="${PROJECT_DIR}/${BUILD_DIR}"

exec "${QEMU_BIN}" \
    -M esp32s3 -snapshot \
    -m 8M \
    -drive "file=${FLASH},if=mtd,format=raw" \
    -drive "file=${EFUSE},if=none,format=raw,id=efuse" \
    -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
    -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
    -nic user,model=open_eth \
    -display sdl,show-cursor=on \
    -serial mon:stdio
