# QEMU display previews

Use `scripts/build-qemu.sh --board 7b` for the 1024×600 status display or `--board 154` for the original 240×240 renderer. Run `scripts/run-qemu-ui.sh --board 7b|154` after building. The default profile is 7b.

`scripts/test-qemu-ui.sh --board 7b|154` retains a boot receipt with source provenance, firmware hashes, QEMU identity, UART output and a framebuffer. A stale, mismatched or foreign checkout build is rejected. `scripts/test-qemu-worktree.py` checks launcher ownership and duplicate refusal.

The compact profile uses its actual renderer and a virtual panel adapter. UART `0`/`1`/`2`/`3` select status, flow, info and notice scenes; `r` rotates and `b` switches the backlight. `scripts/test-qemu-154.py` captures those states and verifies the guest identity. `scripts/capture-qemu-ui.py` captures the minimal 7B status display through QMP.

Linked checkouts mount their absolute Git common directory read-only in the toolchain container. Firmware outputs stay local to each worktree. `scripts/test-platform5.sh` tests both profiles and artifact rejection without a firmware build.

All preview data is simulated. QEMU cannot validate physical BLE, storage, touch-controller recovery, RGB timing, panel tearing or backlight electronics. Each topic requires fresh captures after its own build; integration screenshots do not validate an extracted topic.

Home arrives with the native shell and elapsed-time flow presentation. Capture
active Home with `scripts/capture-qemu-ui.py --screen home`, stopped therapy
with `--screen home-idle`, or the status tray with `--screen status`.
`scripts/test-qemu-touch.py` checks Screen off, the black framebuffer and a
wake press directly over the Screen off control, rejecting a leaked second action.
These commands retain the selected build identity with the resulting images.

## Retained Logs

Open Manage and select Logs to view the retained log stream.
The synthetic feed exercises pause, search, filtering, paging, clear/retry and
save progress through the native controller. Run
`python3 scripts/test-qemu-logs.py` after building this exact checkout, or capture
`--interaction-state logs` with `scripts/capture-qemu-ui.py`.
QEMU save and disconnect fixtures do not establish physical card persistence.

## First-run setup

Tap the QEMU clock or capture `--interaction-state setup-wifi` to open a fresh
simulated setup run. Normal emulator boot seeds finished setup so ordinary
captures remain deterministic. Setup owns its worker, durable state and native
screen independently of the later Manage configuration and maintenance views.
The host runner generates `main/zones.json` before timezone catalogue tests.

## Manage configuration and Devices

Manage now exposes Devices, Connectivity, Alerts, Uploads and Logs. Run
`python3 scripts/test-qemu-rev-c.py` against the current build to exercise native
configuration, redaction and disabled controls; this stage does not expose
Storage, System or Advanced. Capture individual screens through
`--interaction-state devices`, `connectivity`, `alerts` or `uploads`.
Host validation runs production configuration and SMB-probe adapters with
controlled dependencies. Simulated receipts do not verify physical pairing or
external delivery.
