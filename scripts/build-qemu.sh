#!/usr/bin/env bash
# Build the ESP32-S3 QEMU UI preview (default 7B, optional original 1.54-inch).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
for ARG in "$@"; do
    if [ "${ARG}" = "--help" ] || [ "${ARG}" = "-h" ]; then
        echo "Usage: $0 [--board 154|7b] [--clean] (default: 7b)"
        exit 0
    fi
done
PROFILE="$(python3 "${SCRIPT_DIR}/qemu_targets.py" build "$@")"
IFS=$'\t' read -r BOARD BUILD_DIR SDKCONFIG DEFAULTS BUILD_REQUEST CLEAN_REQUEST <<< "${PROFILE}"
SOURCE_STATE="$(python3 "${SCRIPT_DIR}/qemu-artifacts.py" source-state --board "${BOARD}")"

if [ "${CLEAN_REQUEST}" = "1" ]; then
    "${SCRIPT_DIR}/idf.sh" -B "${BUILD_DIR}" -D "SDKCONFIG=${SDKCONFIG}" fullclean || true
fi

# Refresh git-derived PROJECT_VER so captured frames and logs identify the
# exact source revision even when Ninja has no other CMake reason to rerun.
"${SCRIPT_DIR}/idf.sh" -B "${BUILD_DIR}" \
    -D "SDKCONFIG=${SDKCONFIG}" \
    -D "SDKCONFIG_DEFAULTS=${DEFAULTS}" reconfigure
"${SCRIPT_DIR}/idf.sh" -B "${BUILD_DIR}" \
    -D "SDKCONFIG=${SDKCONFIG}" \
    -D "SDKCONFIG_DEFAULTS=${DEFAULTS}" build

"${SCRIPT_DIR}/idf.sh" exec bash -lc \
    "cd /project/${BUILD_DIR} && python -m esptool --chip esp32s3 merge_bin --output qemu_flash.bin --fill-flash-size 16MB @flash_args"

# Advertise the ESP32-S3 revision expected by ESP-IDF during early startup.
dd if=/dev/zero of="${PROJECT_DIR}/${BUILD_DIR}/qemu_efuse.bin" bs=336 count=1 2>/dev/null
printf '\014' | dd of="${PROJECT_DIR}/${BUILD_DIR}/qemu_efuse.bin" \
    bs=1 seek=38 conv=notrunc 2>/dev/null

python3 "${SCRIPT_DIR}/qemu-artifacts.py" record --board "${BOARD}" --source-state "${SOURCE_STATE}"

echo
echo "QEMU UI firmware ready:"
echo "  ${PROJECT_DIR}/${BUILD_DIR}/qemu_flash.bin"
