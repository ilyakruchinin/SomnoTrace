# assets/fonts/

Font assets used by the firmware UI (e.g. converted to bitmap/glyph sources
for the display) live here.

## Licensing rules

SomnoTrace is GPLv3 (with a commercial-licensing option), so included fonts
must permit commercial use and redistribution and remain GPLv3-compatible.
Keep each upstream license and provenance record with its source. Do not add
non-commercial or proprietary font assets.

## Included bedside UI families

The checked-in sources and generated LVGL fonts are pinned to Google Fonts
commit `205859f680703e449fe05dce0f792cc041d6dc89`.

| Family | Source file | SHA-256 |
| --- | --- | --- |
| Space Grotesk | `source/SpaceGrotesk[wght].ttf` | `acad6de1fc93436f5c0f1f4137751ef04f1aea3063e7036535970ffcfbd79f72` |
| IBM Plex Mono Medium | `source/IBMPlexMono-Medium.ttf` | `a9b4c49bb299e05b5f6c481e7fb5e78943d2793249a0c8874ab574a2d1ea6755` |
| IBM Plex Mono SemiBold | `source/IBMPlexMono-SemiBold.ttf` | `d3c38e55c78f5b0f28009fddba4834ec503278936a5986032424c9bd2d23aa46` |

Upstream paths:

- `ofl/spacegrotesk/SpaceGrotesk[wght].ttf`
- `ofl/ibmplexmono/IBMPlexMono-Medium.ttf`
- `ofl/ibmplexmono/IBMPlexMono-SemiBold.ttf`

Both families are licensed under the SIL Open Font License 1.1. Exact license
texts from the pinned source tree are retained in `licenses/`.

## Generated LVGL set

`generate_lvgl_fonts.sh` uses pinned `fonttools==4.59.1` and
`lv_font_conv@1.5.3`. It instantiates Space Grotesk at weights 500 and 600,
then emits compressed 4-bpp LVGL fonts. IBM Plex Mono uses its upstream static
500 and 600 faces. Generator path comments are normalized so identical tools
and sources produce byte-for-byte stable C outputs.

- Space Grotesk Medium: 13, 15, 17, 19, 23, 29, and 34 px
- Space Grotesk SemiBold: 13, 15, 17, 19, 23, 29, 32, and 34 px
- IBM Plex Mono Medium: 11, 13, and 15 px
- IBM Plex Mono SemiBold: 11, 13, 15, 26, 29, and 34 px; the larger faces
  cover runtime and metric-value data roles

This maps the handoff's fractional 12.5 and 14.5 px mono roles to LVGL's 13
and 15 px raster sizes. Regenerate from the repository root with:

```sh
./assets/fonts/generate_lvgl_fonts.sh
```

Set `SOMNOTRACE_FONT_PYTHON` if `python3` does not name a healthy Python 3
installation. The generator covers printable ASCII (`U+0020` through
`U+007E`) plus `§ ° ² · × – — ’ “ ” • … › ₂ ↑ → − ≤ ≥`. Symbols not present
in both source families (including the non-breaking hyphen, check mark, keypad
arrows, and icons) must be normalized to supported text, remain LVGL symbols,
or use native drawn controls rather than silently rendering missing-glyph
boxes.
