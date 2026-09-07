#!/usr/bin/env python3
"""Assert real-touch Logs pause, buffer, paging and confirmation behavior."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
from qemu_runtime import qmp_socket as private_qmp_socket

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("capture", ROOT / "scripts/capture-qemu-ui.py")
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


def region(payload, box):
    x1, y1, x2, y2 = box
    return b"".join(payload[(y*1024+x1)*3:(y*1024+x2)*3] for y in range(y1, y2))


subprocess.run(["python3", ROOT / "scripts/qemu-artifacts.py", "verify"], check=True)
qemu = subprocess.check_output([ROOT / "scripts/setup-qemu-macos.sh"], text=True).strip()
output = ROOT / "build-qemu/captures/logs-behavior"
output.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="run-", dir=ROOT / "build-qemu") as temp, private_qmp_socket(temp) as endpoint:
    uart = output / "serial.log"
    # QEMU appends to file chardevs; readiness must come from this guest boot.
    uart.write_text("")
    qmp = endpoint
    command = [qemu, "-M", "esp32s3", "-m", "8M", "-snapshot",
        "-drive", f"file={ROOT}/build-qemu/qemu_flash.bin,if=mtd,format=raw",
        "-drive", f"file={ROOT}/build-qemu/qemu_efuse.bin,if=none,format=raw,id=efuse",
        "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
        "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
        "-nic", "user,model=open_eth", "-display", "sdl,show-cursor=off",
        "-qmp", f"unix:{qmp},server=on,wait=off", "-monitor", "none",
        "-serial", f"file:{uart}"]
    (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    process = subprocess.Popen(command, cwd=ROOT, env={**os.environ, "TMPDIR": temp},
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    client = None
    try:
        capture.wait_for_log(process, uart, "QEMU UI first frame published", 25)
        client = capture.connect_qmp(qmp)
        stream = client.makefile("rwb", buffering=0)
        assert "QMP" in json.loads(stream.readline())
        capture.qmp_command(stream, "qmp_capabilities")

        def wait(seconds=0.8):
            capture.wait_healthy(process, uart, seconds)

        def tap(x, y):
            capture.click(process, uart, stream, x, y)
            wait()

        def shot(name):
            path = output / f"{name}.ppm"
            capture.qmp_command(stream, "screendump", {"filename": str(path)})
            assert capture.ppm_dimensions(path) == (1024, 600)
            return path.read_bytes()[-1024*600*3:]

        wait(3.5)
        tap(694, 563)
        tap(130, 420)
        wait(1.2)
        tap(698, 100)
        before = shot("paused-before")
        wait(3.0)
        after = shot("paused-buffered")
        rows = (275, 200, 980, 402)
        assert region(before, rows) == region(after, rows), "paused rows moved"
        assert region(before, (270, 105, 600, 123)) != region(after, (270, 105, 600, 123)), "buffer count did not advance"
        tap(625, 440)
        newest = shot("jump-newest")
        assert region(newest, rows) != region(after, rows), "jump failed to re-anchor"
        assert capture.bright_samples(newest, (664, 85, 735, 115), threshold=180) > 180, "jump silently resumed"
        tap(800, 100)
        confirmed = shot("clear-prompt")
        assert region(confirmed, rows) != region(newest, rows), "clear confirmation absent"
        tap(647, 389)  # Cancel. No retained log deletion during this UI test.
        cancelled = shot("clear-cancelled")
        assert region(cancelled, rows) == region(newest, rows), "cancel changed retained rows"
        capture.drag(process, uart, stream, (960, 385), (960, 220))
        wait()
        older = shot("older-page")
        assert region(older, rows) != region(cancelled, rows), "older gesture did not page"
        tap(625, 440)
        tap(330, 88)  # Explicit QEMU-only retained-view fault input.
        disconnected = shot("disconnected")
        tap(625, 421)
        recovered = shot("recovered")
        assert region(disconnected, (365, 288, 884, 443)) != region(recovered, (365, 288, 884, 443)), "recovery failed"
        assert "simulated retained-view recovered" in uart.read_text(errors="replace")
        tap(400, 158)
        tap(377, 430)
        tap(377, 430)
        tap(942, 214)
        empty = shot("empty-query")
        tap(625, 423)  # Clear filter, not the header's retained-buffer Clear.
        cleared = shot("filter-cleared")
        assert region(empty, (365, 288, 884, 443)) != region(cleared, (365, 288, 884, 443)), "empty Clear filter did not work"
        print("Logs touch pause/buffering, explicit jump, clear cancellation, paging, recovery and empty-filter recovery passed")
    finally:
        if client is not None:
            client.close()
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
