#!/usr/bin/env python3
"""Static contract for the generated Waveshare 7B/QEMU LVGL font set."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
GENERATED = ROOT / "assets" / "fonts" / "generated"

SPACE_MEDIUM_SIZES = (13, 15, 17, 19, 23, 29, 34)
SPACE_SEMIBOLD_SIZES = (13, 15, 17, 19, 23, 29, 32, 34)
MONO_MEDIUM_SIZES = (11, 13, 15)
MONO_SEMIBOLD_SIZES = (11, 13, 15, 26, 29, 34)

EXPECTED = {
    *(f"somnotrace_space_grotesk_medium_{size}"
      for size in SPACE_MEDIUM_SIZES),
    *(f"somnotrace_space_grotesk_semibold_{size}"
      for size in SPACE_SEMIBOLD_SIZES),
    *(f"somnotrace_ibm_plex_mono_medium_{size}"
      for size in MONO_MEDIUM_SIZES),
    *(f"somnotrace_ibm_plex_mono_semibold_{size}"
      for size in MONO_SEMIBOLD_SIZES),
}


def main() -> None:
    generated = {}
    for path in GENERATED.glob("*.c"):
        match = re.search(
            r"const lv_font_t (somnotrace_[a-z0-9_]+) =", path.read_text()
        )
        if match:
            generated[match.group(1)] = path

    header = (ROOT / "main" / "somnotrace_fonts.h").read_text()
    declarations = set(re.findall(
        r"LV_FONT_DECLARE\((somnotrace_[a-z0-9_]+)\)", header
    ))
    cmake = (ROOT / "main" / "CMakeLists.txt").read_text()
    registered = set(re.findall(r"(somnotrace_[a-z0-9_]+)\.c", cmake))

    assert set(generated) == EXPECTED, "generated face set does not match contract"
    assert declarations == EXPECTED, "font declarations do not match contract"
    assert registered == EXPECTED, "CMake font sources do not match contract"

    for symbol, path in generated.items():
        text = path.read_text()
        assert ".bitmap_format = 1" in text, f"{symbol} is not compressed"
        assert " * Bpp: 4" in text, f"{symbol} is not 4-bpp"

    print(f"font asset contract passed ({len(EXPECTED)} faces)")


if __name__ == "__main__":
    main()
