# Native History data services

## Memory and repeated work

`history_cache.c` retains at most 192 KiB of payload in PSRAM across 32 LRU
entries, with a 64 KiB maximum item. Its approximately 2 KiB entry table also
lives in PSRAM, preserving internal heap for RGB DMA, BLE, and OTA. There is no internal-memory fallback for retained entries.
It caches night metadata/session captions, parsed session manifests, unified
axes, parsed respiratory events, therapy gates, graph windows, and exact
statistics under separate keys. Calendar/index scans cannot evict the selected
night.

Session arrays above 64 items and event arrays above 1,024 items bypass
retention. They keep their existing complete-source semantics. Visible events
are collected and filtered once per request, so an oversized night does not
restart its complete event scan for each 32-marker page. The 64-marker display
limit still reports truncation and retains the whole-night event count.

Keys include day, channel, range, therapy filter, and a card-content generation.
Recording completion, export/maintenance release, mount/unmount, post-therapy
metadata publication, and FTP writes invalidate previous generations. Explicit
History refresh clears the memoized entries. Read-only operations and derived
pyramid generation do not invalidate otherwise reusable source results.

## Disposable on-card Flow pyramid

Terminal v2 `*_flow_mm.snt` files gain a `*.snt.fpy` sidecar containing 4-, 16-,
and 64-second min/max levels. The post-stop worker builds it after the normal
post-therapy/export pipeline. Existing and recovered nights are backfilled
once foreground graph/statistics work finishes. A new recording or newer
History request cancels this optional work; missing caches use recorded files.
Legacy min/max formats retain their existing reader.

Each record includes a per-second validity mask and CRC. A coarse block is used
only when it fits wholly within one requested display bin. Mixed gaps and bin
boundaries descend to finer levels and ultimately the existing one-second
source. Thus the optimized envelope matches the one-second reader's extrema,
valid counts, and missing bins, including partial windows. Gaps within an
already aggregated one-second source retain that source's original precision;
close inspection continues to use the 25 Hz raw waveform.

The header binds the cache version to the source header, size, modification
time, and full source CRC. The source CRC is verified on first cache use per
card-content generation; subsequent reads still check source metadata/header
and the checksums of cache records they use. Truncated, stale, unsupported, or
corrupt caches fall back to the source and are rebuilt later. Generation writes
check the source again, flush and sync a temporary file, then publish it by
rename. FAT's replacement fallback can lose only the disposable cache if power
fails between unlink and rename. Raw recording files are never changed.

Exact percentiles continue to use the existing raw-source histogram and
therapy eligibility calculation. Display envelopes and previews never supply
percentile values.

## Validation and limits

- `history_flow_cache_test.c` compares production pyramid reads with an
  independent one-second oracle across an eight-hour trace, narrow peaks,
  mixed/long gaps, subsecond window boundaries, partial tails, source rewrites
  with preserved size/timestamps, cancellation, corruption, and truncation.
  The 480-bin eight-hour fixture fetched 4,873 records versus 28,813 one-second
  records after source validation. Record counts are not SD timing measurements.
- `history_cache_test.c` covers the byte budget, LRU behavior, pinned-night
  retention, copies, filter/source identity, oversized entries, and concurrency.
- `history_service_cache_test.py` executes production cache adapters against
  counted mutable sources, including repeated requests, invalidation, cancelled
  reads, oversized event nights, and allocation failure.

Cold first-graph latency and physical SD/RGB acceptance remain open.
