#!/usr/bin/env bash
# Build a pinned Espressif QEMU with native 1024x600 RGB output.
set -euo pipefail

RELEASE="esp-develop-9.2.2-20260417"
EXPECTED_COMMIT="40edccac415693c5130f91c01d84176ae6008566"
ARCH="$(uname -m)"
case "${ARCH}" in
    arm64|x86_64) ;;
    *)
        echo "Unsupported macOS architecture: ${ARCH}" >&2
        exit 1
        ;;
esac

CACHE_BASE="${XDG_CACHE_HOME:-${HOME}/Library/Caches}/SomnoTrace"
SOURCE_DIR="${CACHE_BASE}/qemu-${RELEASE}-rgb1024-source"
BUILD_DIR="${SOURCE_DIR}/build-rgb1024-${ARCH}"
QEMU_BIN="${BUILD_DIR}/qemu-system-xtensa"
PATCH_REVISION="4"
PATCH_STAMP="${BUILD_DIR}/.somnotrace-patch-revision"
if [ -x "${QEMU_BIN}" ] && [ -f "${PATCH_STAMP}" ] && \
        [ "$(<"${PATCH_STAMP}")" = "${PATCH_REVISION}" ]; then
    echo "${QEMU_BIN}"
    exit 0
fi

# The emulator/toolchain cache is shared; only one cold-cache caller may patch
# or build it. Other worktrees may keep using an already completed binary.
mkdir -p "${CACHE_BASE}"
CACHE_LOCK="${SOURCE_DIR}.lock"
for attempt in $(seq 1 300); do
    if mkdir "${CACHE_LOCK}" 2>/dev/null; then
        break
    fi
    if [ -x "${QEMU_BIN}" ] && [ -f "${PATCH_STAMP}" ] && \
            [ "$(<"${PATCH_STAMP}")" = "${PATCH_REVISION}" ]; then
        echo "${QEMU_BIN}"
        exit 0
    fi
    if [ "${attempt}" = 300 ]; then
        echo "Timed out waiting for QEMU cache builder: ${CACHE_LOCK}" >&2
        exit 1
    fi
    sleep 1
done
TEMP_DIR=""
cleanup() {
    if [ -n "${TEMP_DIR}" ]; then rm -rf "${TEMP_DIR}"; fi
    rmdir "${CACHE_LOCK}"
}
trap cleanup EXIT

if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is required for QEMU runtime libraries." >&2
    exit 1
fi
missing_formulae=()
for formula in libgcrypt glib pixman sdl2 libslirp ninja pkg-config; do
    if ! brew list --versions "${formula}" >/dev/null 2>&1; then
        missing_formulae+=("${formula}")
    fi
done
if [ "${#missing_formulae[@]}" -gt 0 ]; then
    HOMEBREW_NO_AUTO_UPDATE=1 \
    HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1 \
        brew install "${missing_formulae[@]}" >&2
fi

mkdir -p "${CACHE_BASE}"
TEMP_DIR="$(mktemp -d "${CACHE_BASE}/qemu-source.XXXXXX")"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ ! -d "${SOURCE_DIR}/.git" ]; then
    git clone --quiet --depth 1 --branch "${RELEASE}" \
        https://github.com/espressif/qemu.git "${TEMP_DIR}/source" >&2
    ACTUAL_COMMIT="$(git -C "${TEMP_DIR}/source" rev-parse HEAD)"
    if [ "${ACTUAL_COMMIT}" != "${EXPECTED_COMMIT}" ]; then
        echo "QEMU source commit mismatch" >&2
        exit 1
    fi
    git -C "${TEMP_DIR}/source" apply "${SCRIPT_DIR}/qemu-rgb-1024.patch"
    git -C "${TEMP_DIR}/source" apply "${SCRIPT_DIR}/qemu-touch.patch"
    git -C "${TEMP_DIR}/source" apply --unidiff-zero \
        "${SCRIPT_DIR}/qemu-emulator-stability.patch"
    mv "${TEMP_DIR}/source" "${SOURCE_DIR}"
fi

ACTUAL_COMMIT="$(git -C "${SOURCE_DIR}" rev-parse HEAD)"
if [ "${ACTUAL_COMMIT}" != "${EXPECTED_COMMIT}" ]; then
    echo "Cached QEMU source is not the pinned release" >&2
    exit 1
fi

if ! git -C "${SOURCE_DIR}" diff --quiet -- include/hw/display/esp_rgb.h; then
    :
elif ! git -C "${SOURCE_DIR}" apply --check "${SCRIPT_DIR}/qemu-rgb-1024.patch"; then
    echo "Cached QEMU source has an unexpected RGB patch state" >&2
    exit 1
else
    git -C "${SOURCE_DIR}" apply "${SCRIPT_DIR}/qemu-rgb-1024.patch"
fi
if ! grep -q '^#define ESP_RGB_MAX_WIDTH   (1024)$' \
        "${SOURCE_DIR}/include/hw/display/esp_rgb.h"; then
    echo "Cached QEMU source does not contain the 1024-pixel RGB patch" >&2
    exit 1
fi

if grep -q '"esp-rgb-touch"' "${SOURCE_DIR}/hw/display/esp_rgb.c"; then
    :
elif ! git -C "${SOURCE_DIR}" apply --check \
        "${SCRIPT_DIR}/qemu-touch.patch"; then
    echo "Cached QEMU source has an unexpected touch patch state" >&2
    exit 1
else
    git -C "${SOURCE_DIR}" apply "${SCRIPT_DIR}/qemu-touch.patch"
fi
if ! grep -q '"esp-rgb-touch"' "${SOURCE_DIR}/hw/display/esp_rgb.c" || \
        ! grep -q 'A_RGB_TOUCH_STATUS' \
            "${SOURCE_DIR}/include/hw/display/esp_rgb.h"; then
    echo "Cached QEMU source does not contain the touch patch" >&2
    exit 1
fi

if grep -q 'SomnoTrace: an SDL expose must preserve' \
        "${SOURCE_DIR}/hw/display/esp_rgb.c" && \
        grep -q 'touch_press_latched' \
        "${SOURCE_DIR}/include/hw/display/esp_rgb.h"; then
    :
elif ! git -C "${SOURCE_DIR}" apply --unidiff-zero --check \
        "${SCRIPT_DIR}/qemu-emulator-stability.patch"; then
    echo "Cached QEMU source has an unexpected emulator stability patch state" >&2
    exit 1
else
    git -C "${SOURCE_DIR}" apply --unidiff-zero \
        "${SCRIPT_DIR}/qemu-emulator-stability.patch"
fi
if ! grep -q 'SomnoTrace: an SDL expose must preserve' \
        "${SOURCE_DIR}/hw/display/esp_rgb.c" || \
        ! grep -q 'SomnoTrace: retain a complete short click' \
            "${SOURCE_DIR}/hw/display/esp_rgb.c" || \
        ! grep -q 'touch_press_latched' \
            "${SOURCE_DIR}/include/hw/display/esp_rgb.h"; then
    echo "Cached QEMU source does not contain the emulator stability patch" >&2
    exit 1
fi

mkdir -p "${BUILD_DIR}"
(
    cd "${BUILD_DIR}"
    ../configure \
        --python=/usr/bin/python3 \
        --target-list=xtensa-softmmu \
        --enable-sdl \
        --enable-slirp \
        --disable-dbus-display \
        --disable-werror >&2
)
ninja -C "${BUILD_DIR}" qemu-system-xtensa >&2
test -x "${QEMU_BIN}"
printf '%s\n' "${PATCH_REVISION}" > "${PATCH_STAMP}"
echo "${QEMU_BIN}"
