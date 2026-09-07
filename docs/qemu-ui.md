# QEMU display previews

Use `scripts/build-qemu.sh --board 7b` for the 1024×600 status display or `--board 154` for the original 240×240 renderer. Run `scripts/run-qemu-ui.sh --board 7b|154` after building. The default profile is 7b.

`scripts/test-qemu-ui.sh --board 7b|154` retains a boot receipt with source provenance, firmware hashes, QEMU identity, UART output and a framebuffer. A stale, mismatched or foreign checkout build is rejected. `scripts/test-qemu-worktree.py` checks launcher ownership and duplicate refusal.

The compact profile uses its actual renderer and a virtual panel adapter. UART `0`/`1`/`2`/`3` select status, flow, info and notice scenes; `r` rotates and `b` switches the backlight. `scripts/test-qemu-154.py` captures those states and verifies the guest identity. `scripts/capture-qemu-ui.py` captures the minimal 7B status display through QMP.

Linked checkouts mount their absolute Git common directory read-only in the toolchain container. Firmware outputs stay local to each worktree. `scripts/test-platform5.sh` tests both profiles and artifact rejection without a firmware build.

All preview data is simulated. QEMU cannot validate physical BLE, storage, touch-controller recovery, RGB timing, panel tearing or backlight electronics. Each topic requires fresh captures after its own build; integration screenshots do not validate an extracted topic.
