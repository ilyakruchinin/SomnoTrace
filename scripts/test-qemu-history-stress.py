#!/usr/bin/env python3
"""Exercise real LVGL History inputs; retain guest UART, frames and provenance."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from qemu_runtime import qmp_socket

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("touch_smoke", ROOT / "scripts/test-qemu-touch.py")
touch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(touch)
FATAL = ("Guru Meditation Error", "assert failed", "abort() was called",
         "Invalid drawing area", "CORRUPT HEAP", "Stack canary watchpoint triggered")


def main():
    subprocess.run(["python3", ROOT / "scripts/qemu-artifacts.py", "verify"], check=True)
    qemu = subprocess.check_output([ROOT / "scripts/setup-qemu-macos.sh"], text=True).strip()
    output = ROOT / "build-qemu/captures/history-stress"
    output.mkdir(parents=True, exist_ok=True)
    provenance = json.loads((ROOT / "build-qemu/provenance.json").read_text())
    (output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    serial = output / "serial.log"
    serial.write_text("")
    actions = []
    with tempfile.TemporaryDirectory(prefix="history-stress-", dir=ROOT / "build-qemu") as temporary, qmp_socket(temporary) as endpoint:
        command = [qemu, "-M", "esp32s3", "-snapshot", "-m", "8M",
                   "-drive", f"file={ROOT / 'build-qemu/qemu_flash.bin'},if=mtd,format=raw",
                   "-drive", f"file={ROOT / 'build-qemu/qemu_efuse.bin'},if=none,format=raw,id=efuse",
                   "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
                   "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
                   "-nic", "user,model=open_eth", "-display", "sdl,show-cursor=off",
                   "-qmp", f"unix:{endpoint},server=on,wait=off",
                   "-serial", f"file:{serial}", "-monitor", "none"]
        (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        with (output / "qemu-stderr.log").open("w") as errors:
            process = subprocess.Popen(command, cwd=ROOT, env={**os.environ, "TMPDIR": temporary},
                                       stdout=subprocess.DEVNULL, stderr=errors)
            client = stream = None
            try:
                touch.wait_for_log(process, serial, "QEMU UI first frame published", 25)
                assert f"ELF file SHA256:  {provenance['artifacts']['somnotrace.elf'][:9]}" in serial.read_text()
                client = socket.socket(socket.AF_UNIX)
                client.connect(str(endpoint))
                stream = client.makefile("rwb", buffering=0)
                json.loads(stream.readline())
                touch.qmp_command(stream, "qmp_capabilities")

                def healthy():
                    assert process.poll() is None, "guest exited"
                    log = serial.read_text(errors="replace")
                    assert not any(marker in log for marker in FATAL), log[-4000:]
                    assert log.count("QEMU UI first frame published") == 1, "guest restarted"
                    return log

                def tap(label, x, y, expected=None):
                    start = time.monotonic()
                    _, offset = touch.tap(process, serial, stream, x, y)
                    if expected:
                        touch.wait_for_log(process, serial, expected, 4, start_offset=offset)
                    actions.append({"action": label, "x": x, "y": y,
                                    "host_seconds": round(time.monotonic() - start, 4)})
                    healthy()

                def capture(name):
                    path = output / f"{name}.ppm"
                    touch.qmp_command(stream, "screendump", {"filename": str(path)})
                    with path.open("rb") as frame:
                        assert frame.readline().strip() == b"P6"
                        assert frame.readline().strip() == b"1024 600"
                        assert frame.readline().strip() == b"255"
                        pixels = frame.read()
                    assert len(pixels) == 1024 * 600 * 3
                    assert max(pixels) > 128, "screen unexpectedly dark"
                    return b"".join(pixels[(y * 1024 + 360) * 3:(y * 1024 + 998) * 3]
                                    for y in range(335, 450))

                tap("History", 512, 563, "emulated touch selected page 1")
                time.sleep(2.5)
                initial = capture("initial")
                tap("Initial zoom", 918, 302)
                time.sleep(0.5)
                zoomed = capture("zoomed")
                assert initial != zoomed, "zoom control did not repaint the trace"
                tap("Initial Fit", 976, 302)
                # Current native 992x450 History surface at screen (16,68).
                # Every tap waits for guest press/release sampling, without
                # waiting for the previous graph request to finish.
                for cycle in range(20):
                    tap("Flow", 400, 204)
                    tap("Zoom in", 918, 302)
                    tap("Zoom in", 918, 302)
                    tap("Zoom out", 870, 302)
                    tap("Pressure", 575, 204)
                    tap("Leak", 750, 204)
                    tap("Fit", 976, 302)
                    if cycle % 4 == 0:
                        touch.drag(process, serial, stream, (650, 400), (800, 400), steps=6)
                        actions.append({"action": "pan", "cycle": cycle})
                        tap("Calendar", 232, 90)
                        tap("Previous month", 38, 135)
                        tap("Next month", 280, 135)
                        tap("List", 90, 90)
                        tap("Second night", 150, 250)
                        tap("First night", 150, 190)
                        tap("Manage", 694, 563, "emulated touch selected page 2")
                        tap("History return", 512, 563, "emulated touch selected page 1")
                    if cycle in (4, 9, 14, 19):
                        capture(f"cycle-{cycle + 1:02d}")
                tap("Final Flow", 400, 204)
                tap("Final zoom", 918, 302)
                time.sleep(0.5)
                final = capture("final")
                log = healthy()
                assert log.count("begin job=3 ") >= 40, "inputs did not exercise graph requests"
                assert log.count("queued Calendar month=") >= 5, "calendar navigation was not exercised"
                assert initial != final, "graph did not visibly repaint"
                result = {"passed": True, "sampled_actions": len(actions),
                          "view_jobs": log.count("begin job=3 "),
                          "initial_graph_sha256": hashlib.sha256(initial).hexdigest(),
                          "final_graph_sha256": hashlib.sha256(final).hexdigest(),
                          "scope": "QEMU synthetic History, real LVGL/QMP input; no physical SD/touch/RGB timing"}
                (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
                print(json.dumps(result))
            finally:
                (output / "actions.json").write_text(json.dumps(actions, indent=2) + "\n")
                if stream:
                    stream.close()
                if client:
                    client.close()
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


if __name__ == "__main__":
    main()
