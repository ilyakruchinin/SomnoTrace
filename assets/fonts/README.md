# assets/fonts/

Font assets used by the firmware UI (e.g. converted to bitmap/glyph headers for
the display) live here.

## Licensing — must allow commercial use AND be GPLv3-compatible

SomnoTrace is GPLv3 (with a commercial-licensing option), so only include
fonts whose license permits commercial use and redistribution and does not
conflict with GPLv3:

- **SIL Open Font License 1.1 (OFL)** — the license used by most
  [fonts.google.com](https://fonts.google.com) families (e.g. Roboto's newer
  releases, Open Sans, Inter, Noto). OFL allows commercial use and bundling;
  it is compatible with being shipped alongside GPL software. **Keep the OFL
  text and the Reserved Font Name rules intact**, and do not sell the fonts on
  their own.
- **Apache License 2.0** — used by some Google fonts (e.g. original Roboto).
  Commercial use OK; Apache-2.0 is compatible with GPLv3.
- **Public domain / CC0** — always fine.

**Avoid** fonts under CC BY-NC (non-commercial) or any proprietary EULA.

## Rules

1. For each font family, include its upstream license file here, e.g.
   `Inter/OFL.txt`, plus an `ORIGIN.md` noting the source URL and version.
2. Record the font in the repo-root `THIRD-PARTY-NOTICES.md`.
3. If you embed a converted form (e.g. a generated `.h` glyph table), keep the
   original font file (or a clear pointer to it) so the asset is reproducible.

## Layout

```
assets/fonts/
  Inter/
    OFL.txt
    ORIGIN.md
    Inter-Regular.ttf
  <generated headers, if any, may live with the component that uses them>
```
