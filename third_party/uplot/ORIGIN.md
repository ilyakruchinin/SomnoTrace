# uPlot

- **Source:** https://github.com/leeoniya/uPlot
- **Version:** v1.6.32 (latest stable, March 2025)
- **Upstream author:** Leon Sorokin
- **License:** MIT
- **Used for:** lightweight time-series charting library for the web UI
  (CPU/memory graphs on the Status page, session data plots)

## Files vendored

Only the pre-built distribution files are included (no source modifications):

- `dist/uPlot.iife.min.js` — minified IIFE build (~51 KB), self-contained, no dependencies
- `dist/uPlot.min.css` — minified stylesheet (~1.9 KB)

## Selection rationale

- MIT license: compatible with GPLv3 and the project's commercial dual-licensing option
- Zero runtime dependencies
- ~51 KB minified — small enough to embed in firmware flash
- IIFE build: no module loader or bundler required, works with plain `<script>` tags
- Designed for fast time-series rendering, suitable for ESP32-served web UI
