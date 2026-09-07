#!/usr/bin/env python3
"""Exercise the original compact renderer using emulator-only UART fixtures.

Retains UART, QMP launch command, source provenance and actual 240x240 frames.
This does not emulate the physical ST7789 bus, buttons, battery or touch chip.
"""
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

from qemu_runtime import qmp_socket


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-qemu-154"
READY = "240x240 original-board UI preview ready"
FATAL = ("Guru Meditation Error", "assert failed", "abort() was called",
         "Invalid drawing area", "CORRUPT HEAP", "Stack canary watchpoint triggered")
SIZE = 240


def qmp(stream, command, arguments=None):
    request = {"execute": command}
    if arguments is not None:
        request["arguments"] = arguments
    stream.write(json.dumps(request).encode() + b"\r\n")
    while True:
        line = stream.readline()
        if not line:
            raise RuntimeError("QMP disconnected")
        reply = json.loads(line)
        if "error" in reply:
            raise RuntimeError(reply["error"])
        if "return" in reply:
            return reply["return"]


def clockwise(pixels):
    """Rotate packed RGB pixels independently of the guest transport."""
    rotated = bytearray(len(pixels))
    for y in range(SIZE):
        for x in range(SIZE):
            source = (y * SIZE + x) * 3
            destination = (x * SIZE + SIZE - 1 - y) * 3
            rotated[destination:destination + 3] = pixels[source:source + 3]
    return bytes(rotated)


def checksum(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    subprocess.run(["python3", ROOT / "scripts/qemu-artifacts.py",
                    "verify", "--board", "154"], check=True)
    qemu = subprocess.check_output([ROOT / "scripts/setup-qemu-macos.sh"], text=True).strip()
    output = BUILD / "captures/compact-runtime"
    output.mkdir(parents=True, exist_ok=True)
    provenance = json.loads((BUILD / "provenance.json").read_text())
    (output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    serial = output / "serial.log"
    serial.write_text("")
    result = {"passed": False, "actions": [], "frames": {},
              "qemu_sha256": checksum(qemu),
              "test_sha256": checksum(__file__),
              "runtime_helper_sha256": checksum(ROOT / "scripts/qemu_runtime.py"),
              "scope": "Original compact renderer and virtual panel; simulated UART fixtures only"}
    with tempfile.TemporaryDirectory(prefix="compact-", dir=BUILD) as temporary, qmp_socket(temporary) as endpoint:
        uart_endpoint = endpoint.with_name("uart")
        command = [qemu, "-M", "esp32s3", "-snapshot", "-S", "-m", "8M",
                   "-drive", f"file={BUILD / 'qemu_flash.bin'},if=mtd,format=raw",
                   "-drive", f"file={BUILD / 'qemu_efuse.bin'},if=none,format=raw,id=efuse",
                   "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
                   "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
                   "-nic", "user,model=open_eth", "-display", "none",
                   "-qmp", f"unix:{endpoint},server=on,wait=off",
                   "-chardev", f"socket,id=uart,path={uart_endpoint},server=on,wait=off,logfile={serial}",
                   "-serial", "chardev:uart", "-monitor", "none"]
        (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        with (output / "qemu-stderr.log").open("w") as errors:
            process = subprocess.Popen(command, cwd=ROOT, env={**os.environ, "TMPDIR": temporary},
                                       stdout=subprocess.DEVNULL, stderr=errors)
            client = uart = stream = None
            try:
                def healthy():
                    if uart is not None:
                        # QEMU's socket UART needs a reader even though its
                        # logfile retains the bytes. Otherwise the backend
                        # fills and blocks guest logging during scene changes.
                        while True:
                            try:
                                received = uart.recv(65536)
                            except BlockingIOError:
                                break
                            assert received, "UART disconnected"
                    assert process.poll() is None, "guest exited"
                    log = serial.read_text(errors="replace")
                    assert not any(marker in log for marker in FATAL), log[-4000:]
                    assert log.count(READY) <= 1, "guest restarted"
                    return log

                def wait_log(phrase, offset=0, timeout=8):
                    deadline = time.monotonic() + timeout
                    while time.monotonic() < deadline:
                        # With -display none there is no GUI refresh timer.
                        # A screendump advances the virtual panel update and
                        # releases the guest's synchronous RGB flush.
                        qmp(stream, "screendump", {"filename": str(output / "progress.ppm")})
                        log = healthy()
                        if phrase in log[offset:]:
                            return
                        time.sleep(0.05)
                    raise TimeoutError(f"Missing guest log: {phrase}\n{healthy()[-4000:]}")

                deadline = time.monotonic() + 10
                while not endpoint.exists() or not uart_endpoint.exists():
                    healthy()
                    if time.monotonic() >= deadline:
                        raise TimeoutError("QEMU control sockets did not appear")
                    time.sleep(0.05)
                client = socket.socket(socket.AF_UNIX)
                client.settimeout(10)
                client.connect(str(endpoint))
                stream = client.makefile("rwb", buffering=0)
                json.loads(stream.readline())
                qmp(stream, "qmp_capabilities")
                uart = socket.socket(socket.AF_UNIX)
                uart.settimeout(10)
                uart.connect(str(uart_endpoint))
                uart.setblocking(False)
                # Attach UART before execution so initial boot identity is not
                # discarded by a disconnected socket backend.
                qmp(stream, "cont")
                wait_log(READY, timeout=30)
                assert f"ELF file SHA256:  {provenance['artifacts']['somnotrace.elf'][:9]}" in healthy()

                def action(key, phrase):
                    offset = len(healthy())
                    uart.sendall(key.encode())
                    wait_log(phrase, offset)
                    result["actions"].append(key)

                def capture(name, expected=None, dark=False):
                    path = output / f"{name}.ppm"
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline:
                        healthy()
                        qmp(stream, "screendump", {"filename": str(path)})
                        with path.open("rb") as frame:
                            assert frame.readline().strip() == b"P6"
                            assert frame.readline().strip() == b"240 240"
                            assert frame.readline().strip() == b"255"
                            pixels = frame.read()
                        assert len(pixels) == SIZE * SIZE * 3
                        visible = max(pixels) > 128
                        if ((not visible if dark else visible) and
                                (expected is None or expected == pixels)):
                            if dark:
                                assert max(pixels) == 0, "blank frame must be fully black"
                            result["frames"][name] = hashlib.sha256(pixels).hexdigest()
                            return pixels
                        time.sleep(0.05)
                    raise AssertionError(f"{name}: frame did not reach expected pixels")

                status = capture("status")
                action("1", "QEMU 1.54 scene ready: 1")
                graph = capture("graph")
                assert status != graph, "graph fixture did not replace status"

                # The real flow trace is mint green. The grid and text cannot
                # satisfy this test, including in the intentionally empty gap.
                has_trace = []
                trace_pixels = 0
                for x in range(40, 235):
                    count = 0
                    for y in range(20, 225):
                        at = (y * SIZE + x) * 3
                        r, g, b = graph[at:at + 3]
                        count += g > 170 and g > r * 1.5 and g > b * 1.2
                    trace_pixels += count
                    has_trace.append(count > 0)
                longest_gap = consecutive = 0
                for present in has_trace:
                    consecutive = 0 if present else consecutive + 1
                    longest_gap = max(longest_gap, consecutive)
                assert trace_pixels > 200, "mint trace missing or RGB565 colors incorrect"
                assert sum(has_trace) > 100, "too little of the waveform is visible"
                assert longest_gap >= 8, "missing-data interval was bridged by a trace"
                result["trace_pixels"] = trace_pixels
                result["longest_gap_columns"] = longest_gap

                expected = graph
                for angle in (90, 180, 270, 0):
                    action("r", f"QEMU 1.54 frame ready: scene=1 rotation={angle} backlight=on")
                    expected = clockwise(expected)
                    capture(f"graph-rotation-{angle}", expected=expected)
                action("b", "QEMU 1.54 frame ready: scene=1 rotation=0 backlight=off")
                capture("screen-off", dark=True)
                action("b", "QEMU 1.54 frame ready: scene=1 rotation=0 backlight=on")
                capture("screen-restored", expected=graph)

                action("2", "QEMU 1.54 scene ready: 2")
                info = capture("info")
                action("3", "QEMU 1.54 scene ready: 3")
                notice = capture("notice")
                assert len({status, graph, info, notice}) == 4, "fixtures share a stale frame"
                amber = sum(r > 160 and g > 100 and b < 65
                            for r, g, b in zip(notice[::3], notice[1::3], notice[2::3]))
                assert amber > 30, "amber notice missing or RGB565 byte order wrong"

                # Revisit all scenes repeatedly to catch stale state and ensure
                # renderer publication remains deterministic after rotations.
                frames = (status, graph, info, notice)
                for cycle in range(4):
                    for scene, reference in enumerate(frames):
                        action(str(scene), f"QEMU 1.54 scene ready: {scene}")
                        capture(f"revisit-{scene}", expected=reference)
                assert healthy().count(READY) == 1
                subprocess.run(["python3", ROOT / "scripts/qemu-artifacts.py",
                                "verify", "--board", "154"], check=True)
                assert json.loads((BUILD / "provenance.json").read_text()) == provenance, (
                    "build provenance changed during the test")
                result["passed"] = True
                print(json.dumps(result))
            finally:
                (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
                if stream:
                    stream.close()
                if client:
                    client.close()
                if uart:
                    uart.close()
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


if __name__ == "__main__":
    main()
