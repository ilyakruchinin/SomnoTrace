# Native feature validation

Run `bash scripts/test-host.sh`, then build the physical targets with the IDF wrapper and `scripts/build-7b.sh`. Build QEMU with `scripts/build-qemu.sh --board 7b` or `--board 154`; run emulator checks sequentially against that checkout. See [the QEMU guide](qemu-ui.md) for boot, touch, navigation and capture commands.

Host fixtures exercise production code with cancellation, allocation and I/O failures. QEMU receipts identify the source, firmware and guest framebuffer. Neither establishes physical timing, card durability, radio services, touch-controller recovery or OTA flashing.

Physical History tearing, cold first-graph latency, optional absent-channel EDF refusal, and interrupted shared-file publication remain acceptance checks. The [Rev C contract](../spec/0013-touch-ui-rev-c.md) records feature scope and hardware requirements.
