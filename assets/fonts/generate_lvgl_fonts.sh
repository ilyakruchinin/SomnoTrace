#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="${SCRIPT_DIR}/source"
OUTPUT_DIR="${SCRIPT_DIR}/generated"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

FONT_PYTHON="${SOMNOTRACE_FONT_PYTHON:-}"
if [[ -z "${FONT_PYTHON}" ]]; then
    for candidate in python3 /usr/bin/python3 python3.13 python3.12; do
        if command -v "${candidate}" >/dev/null 2>&1 && \
            "${candidate}" -c 'import xml.parsers.expat' >/dev/null 2>&1; then
            FONT_PYTHON="${candidate}"
            break
        fi
    done
fi
if [[ -z "${FONT_PYTHON}" ]]; then
    echo "No healthy Python 3 interpreter found; set SOMNOTRACE_FONT_PYTHON." >&2
    exit 1
fi

"${FONT_PYTHON}" -m pip install --quiet --disable-pip-version-check \
    --target "${WORK_DIR}/python" 'fonttools==4.59.1'

PYTHONPATH="${WORK_DIR}/python" "${FONT_PYTHON}" -m fontTools.varLib.instancer \
    "${SOURCE_DIR}/SpaceGrotesk[wght].ttf" wght=500 \
    --output "${WORK_DIR}/SpaceGrotesk-Medium.ttf"
PYTHONPATH="${WORK_DIR}/python" "${FONT_PYTHON}" -m fontTools.varLib.instancer \
    "${SOURCE_DIR}/SpaceGrotesk[wght].ttf" wght=600 \
    --output "${WORK_DIR}/SpaceGrotesk-SemiBold.ttf"

mkdir -p "${OUTPUT_DIR}"

# Printable ASCII plus the punctuation, navigation, status, units, and keyboard
# glyphs used by the bedside UI. Keeping the same coverage in every face makes
# runtime font selection predictable without carrying full Unicode cmap tables.
GLYPH_RANGES='0x20-0x7E,0x00A7,0x00B0,0x00B2,0x00B7,0x00D7,0x2013-0x2014,0x2019,0x201C-0x201D,0x2022,0x2026,0x203A,0x2082,0x2191-0x2192,0x2212,0x2264-0x2265'

generate_face() {
    local source_font="$1"
    local symbol_prefix="$2"
    shift 2

    local size
    for size in "$@"; do
        local symbol="${symbol_prefix}_${size}"
        npx --yes lv_font_conv@1.5.3 \
            --font "${source_font}" \
            --size "${size}" \
            --bpp 4 \
            --format lvgl \
            --range "${GLYPH_RANGES}" \
            --lv-include lvgl.h \
            --lv-font-name "${symbol}" \
            --output "${OUTPUT_DIR}/${symbol}.c"
    done
}

generate_face "${WORK_DIR}/SpaceGrotesk-Medium.ttf" \
    somnotrace_space_grotesk_medium 13 15 17 19 23 29 34
generate_face "${WORK_DIR}/SpaceGrotesk-SemiBold.ttf" \
    somnotrace_space_grotesk_semibold 13 15 17 19 23 29 32 34
generate_face "${SOURCE_DIR}/IBMPlexMono-Medium.ttf" \
    somnotrace_ibm_plex_mono_medium 11 13 15
generate_face "${SOURCE_DIR}/IBMPlexMono-SemiBold.ttf" \
    somnotrace_ibm_plex_mono_semibold 11 13 15 26 29 34

# lv_font_conv records absolute input/output paths in its banner. Normalize
# those comments so regeneration is byte-for-byte stable across machines and
# temporary directories; this does not alter any font data.
FONT_WORK_DIR="${WORK_DIR}" FONT_SCRIPT_DIR="${SCRIPT_DIR}" \
    PYTHONPATH="${WORK_DIR}/python" "${FONT_PYTHON}" - <<'PY'
import os
from pathlib import Path

output_dir = Path(os.environ["FONT_SCRIPT_DIR"]) / "generated"
replacements = {
    os.environ["FONT_WORK_DIR"] + "/SpaceGrotesk-Medium.ttf":
        "source/SpaceGrotesk[wght].ttf@wght=500",
    os.environ["FONT_WORK_DIR"] + "/SpaceGrotesk-SemiBold.ttf":
        "source/SpaceGrotesk[wght].ttf@wght=600",
    os.environ["FONT_SCRIPT_DIR"] + "/source/": "source/",
    os.environ["FONT_SCRIPT_DIR"] + "/generated/": "generated/",
}
for generated in output_dir.glob("*.c"):
    text = generated.read_text(encoding="utf-8")
    for source, normalized in replacements.items():
        text = text.replace(source, normalized)
    generated.write_text(text.rstrip() + "\n", encoding="utf-8")
PY
