# Native 7-inch configuration and maintenance — Rev C

Status: implementation contract; acceptance must be reported separately.

## Source and scope

The visual source is the `Handoff Rev C` package supplied on 2026-09-05:
`00 Delta - read first.html`, `Manage - configuration.html`, and `Logs.html`.
The package defines 21 Manage frames and six Logs frames. Rev B3 remains the
contract for Home, History, Devices, first-run setup, and the shared keyboard
unless this document explicitly replaces a behavior.

Rev C extends the existing `codex/rev-b3-touch-parity` implementation. Its
eight-destination rail, retained Logs, setup controllers, and storage/therapy
arbitration are existing foundations. A feature pictured as new in the design
may already exist in firmware; preserve it and implement its actual delta.

This scope does not include merging upstream changes, flashing a connected
board, sending real test notifications, installing firmware on hardware, or
performing destructive maintenance on existing recordings during development.

## Shared presentation and ownership

- Keep the 1024×600 landscape shell: 64 px header, 74 px bottom navigation,
  212 px Manage rail, eight 46 px destination rows, and a 768×450 detail host.
- The rail remains visible while navigating within a destination. Devices,
  Connectivity, Alerts, Uploads, Storage, System, Logs, and Advanced retain
  their order. Display preferences live under System.
- Ordinary actions respond on touch-down. Compact controls have a minimum
  44×44 px hit area. Destructive operations use an uninterrupted hold, with
  visible progress and release-to-cancel behavior.
- Show the current value on each settings row. Configuration summaries fit
  in the detail pane; System diagnostics may scroll. Longer collections such
  as files, history, and timezone results use bounded paging or reused rows.
- When the keyboard opens, retain the edited field and useful validation
  context, such as a UNC preview. Restore the form when editing ends.
- Use monospace for measurements, addresses, paths, dates, and durations;
  proportional text for descriptions. Retain existing licensed fonts.
- Health dots describe observed status. Numeric badges count actionable
  items, such as failed upload destinations or unacknowledged alerts, rather
  than arbitrary degraded metrics.
- Build each detail surface lazily. Snapshot data is bounded and owns its
  strings; callbacks never retain temporary form pointers. Navigation must
  not leave a worker referencing destroyed LVGL objects.

## Connectivity and time

Manage all four saved Wi-Fi slots: add, edit, forget, drag to reorder, and an
explicit move-to-primary action. Saved order is the fallback priority. Keep
out-of-range saved networks visible. Distinguish a saved order change from an
active reconnect, and explain when an operation interrupts the network link.
Use the existing shared radio arbitration during therapy and other radio work.

Show active SSID, IP, RSSI when available, MAC, hostname, and addressing mode.
Support validated DHCP/static configuration through the same persisted service
used by setup and HTTP; do not create a second configuration store.

Time includes the existing searchable IANA catalogue, NTP server, hostname,
current source, and the last successful NTP sync as both timestamp and age.
Never-synced and unset-zone states remain distinguishable. Label UTC when no
zone is set, explain the consequence for night dates, and do not imply that
changing the zone retroactively renames stored nights. Only show measured
clock offsets; a successful sync alone does not establish an offset value.

## Alerts

Group the current configuration into WHEN and HOW: enabled setting, alert
window, initial delay, escalation delay, delivery mode, ntfy server/topic, and
priority. Retain actual settings bounds and update the running alert service
through its owner-task lifecycle.

Separate enabled, configured, test pending, accepted by the push service,
failed, and acknowledged states. A successful HTTP submission does not prove
that a phone received or displayed a notification. Changing the destination
invalidates its old validation receipt. No priority setting may claim that it
bypasses a phone's silent mode.

The 7B has no compatible alert speaker. Credit screen-only alerts as partial
delivery and state the limits of missing push configuration. A topic is a
shared secret; regenerating it must explain that phone subscriptions need to
change. Do not include topics, passwords, or tokens in diagnostic exports.

Recent alerts retain dated outcomes and explicit acknowledgements for up to
30 days, with a bounded storage policy and truthful handling of unavailable
history. Distinguish an unacknowledged accepted push from a failed send. Do
not synthesize acknowledgements, delivery receipts, or historical records.

## Uploads and storage

SMB and SleepHQ retain independent enable/configuration, tests, status, and
retry actions. SMB includes host, share, path, username, password, and a live
UNC preview. Test progress identifies only stages the backend actually
observes; unobserved stages cannot be marked successful. SleepHQ uses the
existing client-ID/secret configuration and credential test.

FTP is the existing local file server, not a third outgoing upload backend.
Expose its enable/authentication settings and local address, identify the
download-server role, and disclose any restart requirement. Keep it separate
from outgoing upload progress and retry counts.

Expose scheduler state, history-window days, and per-destination retry without
disturbing the other destination's progress. Distinguish backoff from failure.
Only show a next-check time when the scheduler supplies it. Counts of queued
nights must come from the index, not a count of individual files or attempts.

Storage shows exact known free/total space and per-night files with size and
contents. A capacity-in-nights estimate must identify its basis and O2
qualification; omit a numeric estimate when representative source data is
unavailable. Listing, estimating, and browsing run asynchronously under the
shared SD lease and yield to recording. Prevent traversal outside approved
card roots and never interpret untrusted filenames as markup or commands.

## System, firmware, and Advanced

System reports actual build/target, network, internal free/minimum/largest
allocation, PSRAM free/minimum, and task count. Per-service rows identify both
the service and its cause, and navigate to the relevant destination. Weak
Wi-Fi signal alone does not establish a failed service. Controller error
history and diagnostic card exports describe observed evidence, with unknown
or unavailable fields labeled accordingly.

Firmware checks are explicit and timestamped, with release notes when
available. Support network installation, an advanced URL, and an image chosen
from `somnotrace-*.bin` files in the card root. The backend validates the image
and target before selecting it for boot. UI fixtures never authorize hardware
installation.

Progress comes from real stages and byte counts. If total size is unknown,
show activity and transferred bytes without inventing a percentage. Only
offer cancellation while the backend can honor it atomically before boot
selection commits. Distinguish writing the inactive OTA slot from selecting
that slot for boot; do not say that no flash was written during a download
that already streamed bytes into it. Failure/cancellation leaves the running
version selected unless the backend explicitly reports otherwise.

Preserve internal-stack flash safety, resource admission, boot rollback
health checks, therapy preemption, SD lease ownership, and controlled-restart
fencing. UI navigation does not revoke an ongoing operation's backend
ownership. Therapy becoming active must be rechecked at the final operation
boundary, not merely when a button was drawn.

Advanced separates safe maintenance from irreversible actions:

| Operation | Data contract |
|---|---|
| Reset upload state | Forget upload receipts; retain recordings and exports. |
| Recreate EDF | Rebuild generated exports from retained session data. |
| Delete EDF | Delete generated exports only; retain source session data. |
| Reset all data | Remove recordings, settings, and device pairing; return to first-run setup. |
| Format card | Remove card contents; state the loss and retained device settings. |

Destructive confirmation uses actual known nights/pending-upload counts when
available and names what survives. Never substitute a fabricated count when
the index cannot be read. Release, navigation, cancellation, and an invalid
precondition reset the hold. Completion dispatch revalidates the operation
and acquires the same backend ownership as HTTP maintenance. Slow operations
never run in LVGL handlers.

## Logs

Refine the existing ten-row retained viewer to the six Rev C states. Keep
fixed time/level/tag/message columns, tag truncation, and three severity cues:
accent bar, level text, and row tint. Debug is off by default; level buttons
are independent of tag/message substring search.

Pause freezes the viewport while buffering continues. Search focus pauses
before opening the keyboard; dismissal does not silently resume. Show the
new-line count and provide a jump-to-newest action. Empty results name the
query and relevant active levels. Describe retention in lines and observed
time span without extrapolating an invented duration.

Clear affects only the retained RAM view. Save to card writes a named
snapshot through the shared SD lease and reports real progress. A disconnected
viewer retains its last readable lines and offers recovery. Statements about
therapy and recording remaining unaffected require known independent status.

## Explicitly deferred by the design

The Information-only Home layout, Home stale-value/session-state corrections,
day/night visual theme, and larger-text treatment require a separate Home
pass. Do not expose working-looking selectors for unavailable layouts. Touch
calibration is conditional on physical evidence. Rotation and speaker volume
are removed requirements for the fixed-landscape 7B.

## Acceptance

All 21 Manage frames and six Logs frames are reference states, not sufficient
evidence by themselves. Validate entry, editing, cancellation, failure,
recovery, and leaving/re-entering each implemented destination with native
touch input. Preserve the existing Home/History/setup regressions.

QEMU must compile and boot the linked worktree's own firmware, resolve its
Git revision inside the build environment, and write its build products and
captures within that worktree's chosen output directory. Temporary flash and
sockets use invocation-specific paths; short temporary socket paths outside
the checkout are permitted. Shared toolchain/emulator caches are permitted; a launcher must
not borrow the main checkout's firmware or stop unrelated QEMU processes.

Record deterministic host tests, board/QEMU compilation, framebuffer/touch
checks, and unresolved physical-board gates separately. QEMU cannot verify
BLE, radio reconnect behavior, SDMMC, flash power loss, backlight, GT911, or
RGB panel timing.
