#!/usr/bin/env bash
# Build a merged, offset-0 image for Waveshare ESP32-S3-Touch-LCD-7B.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="build-7b"
SDKCONFIG="sdkconfig.7b"
DEFAULTS="sdkconfig.defaults;sdkconfig.7b.defaults"
OUTPUT="somnotrace-waveshare-7b-test-full.bin"

if [ "${1:-}" = "--clean" ]; then
    "${SCRIPT_DIR}/idf.sh" -B "${BUILD_DIR}" -D "SDKCONFIG=${SDKCONFIG}" fullclean || true
elif [ "$#" -ne 0 ]; then
    echo "Usage: $0 [--clean]" >&2
    exit 2
fi

# Refresh git-derived PROJECT_VER even when the source tree is otherwise unchanged.
"${SCRIPT_DIR}/idf.sh" -B "${BUILD_DIR}" \
    -D "SDKCONFIG=${SDKCONFIG}" \
    -D "SDKCONFIG_DEFAULTS=${DEFAULTS}" reconfigure
"${SCRIPT_DIR}/idf.sh" -B "${BUILD_DIR}" \
    -D "SDKCONFIG=${SDKCONFIG}" \
    -D "SDKCONFIG_DEFAULTS=${DEFAULTS}" build
"${SCRIPT_DIR}/idf.sh" -B "${BUILD_DIR}" \
    -D "SDKCONFIG=${SDKCONFIG}" merge-bin -o "${OUTPUT}"

mkdir -p "${PROJECT_DIR}/dist"
cp "${PROJECT_DIR}/${BUILD_DIR}/${OUTPUT}" "${PROJECT_DIR}/dist/${OUTPUT}"
cp "${PROJECT_DIR}/${BUILD_DIR}/somnotrace.bin" \
   "${PROJECT_DIR}/dist/somnotrace-waveshare-7b-test-ota.bin"

echo
echo "7B firmware ready:"
echo "  ${PROJECT_DIR}/dist/${OUTPUT}"
echo "Flash the full image at address 0x0."
