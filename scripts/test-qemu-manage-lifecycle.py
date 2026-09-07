#!/usr/bin/env python3
"""Stress 64 Manage destinations through real QEMU touch and capture the result."""

import argparse
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
from qemu_runtime import qmp_socket as private_qmp_socket
import time


ROOT = Path(__file__).resolve().parents[1]
FATAL_MARKERS = (
    "Guru Meditation Error",
    "assert failed",
    "abort() was called",
    "LoadProhibited",
    "Stack smashing",
    "Invalid drawing area",
)
LIFECYCLE = re.compile(
    r"manage lifecycle (?P<action>[a-z-]+) gen=(?P<generation>\d+) "
    r"selected=(?P<selected>-?\d+) rendered=(?P<rendered>-?\d+) "
    r"roots=(?P<roots>\d+) objects=(?P<objects>\d+) "
    r"internal=(?P<internal>\d+) psram=(?P<psram>\d+)"
)


def qmp(stream, execute, arguments=None):
    message = {"execute": execute}
    if arguments is not None:
        message["arguments"] = arguments
    stream.write(json.dumps(message).encode() + b"\r\n")
    while True:
        reply = json.loads(stream.readline())
        if "error" in reply:
            raise RuntimeError(reply["error"])
        if "return" in reply:
            return reply["return"]


def text(log_path):
    return log_path.read_text(errors="replace") if log_path.exists() else ""


def assert_healthy(process, log_path):
    if process.poll() is not None:
        raise RuntimeError(f"QEMU exited with status {process.returncode}")
    log = text(log_path)
    for marker in FATAL_MARKERS:
        if marker in log:
            # Let the guest finish its backtrace before terminating our process.
            # This is failure collection, not extra time for a passing action.
            time.sleep(0.35)
            log = text(log_path) or log
            position = log.index(marker)
            excerpt = log[max(0, position - 1200) : position + 5000]
            raise RuntimeError(f"QEMU reported {marker}\n{excerpt}")


def wait_log(process, log_path, phrase, timeout=5, offset=0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        assert_healthy(process, log_path)
        current = text(log_path)
        if phrase in current[offset:]:
            return current[offset:]
        time.sleep(0.05)
    raise TimeoutError(f"missing log {phrase!r}\n{text(log_path)[-5000:]}")


def connect(path):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        client = socket.socket(socket.AF_UNIX)
        try:
            client.connect(str(path))
            return client
        except (FileNotFoundError, ConnectionRefusedError):
            client.close()
            time.sleep(0.05)
    raise TimeoutError(f"QMP socket unavailable: {path}")


def tap(process, log_path, stream, x, y, expected=None):
    offset = len(text(log_path))
    qmp(stream, "input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": round(x * 32767 / 1023)}},
        {"type": "abs", "data": {"axis": "y", "value": round(y * 32767 / 599)}},
        {"type": "btn", "data": {"button": "left", "down": True}},
    ]})
    try:
        wait_log(process, log_path, "emulated touch at", 3, offset)
        time.sleep(0.05)
    finally:
        release_offset = len(text(log_path))
        qmp(stream, "input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": False}},
        ]})
    wait_log(process, log_path, "QEMU touch release sampled", 3, release_offset)
    if expected:
        wait_log(process, log_path, expected, 5, offset)
    time.sleep(0.09)
    return offset


def ppm_dimensions(path):
    with path.open("rb") as image:
        tokens = []
        while len(tokens) < 4:
            line = image.readline()
            if not line:
                break
            if not line.startswith(b"#"):
                tokens.extend(line.split())
    return tuple(map(int, tokens[1:3])) if len(tokens) >= 4 else (0, 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        default=str(ROOT / "build-qemu" / "captures" /
                    "manage-lifecycle-stress.ppm"),
    )
    args = parser.parse_args()
    output = Path(args.output).expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    flash = ROOT / "build-qemu" / "qemu_flash.bin"
    efuse = ROOT / "build-qemu" / "qemu_efuse.bin"
    if not flash.exists() or not efuse.exists():
        subprocess.run([ROOT / "scripts" / "build-qemu.sh"], check=True)
    subprocess.run(["python3", ROOT / "scripts/qemu-artifacts.py", "verify"], check=True)
    qemu_binary = subprocess.check_output(
        [ROOT / "scripts" / "setup-qemu-macos.sh"], text=True
    ).strip()

    with tempfile.TemporaryDirectory(prefix="run-", dir=ROOT / "build-qemu") as temp, private_qmp_socket(temp) as endpoint:
        qmp_path = endpoint
        serial = output.with_suffix(".serial.log")
        serial.write_text("")  # Do not mistake a prior failed run for this boot.
        command = [
            qemu_binary,
            "-M", "esp32s3", "-snapshot",
            "-m", "8M",
            "-drive", f"file={flash},if=mtd,format=raw",
            "-drive", f"file={efuse},if=none,format=raw,id=efuse",
            "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
            "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
            "-nic", "user,model=open_eth",
            "-display", "sdl,show-cursor=off",
            "-qmp", f"unix:{qmp_path},server=on,wait=off",
            "-serial", f"file:{serial}",
            "-monitor", "none",
        ]
        output.with_suffix(".command.json").write_text(json.dumps(command, indent=2) + "\n")
        process = subprocess.Popen(command, cwd=ROOT,
            env={**os.environ, "TMPDIR": str(temp)},
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        client = None
        try:
            wait_log(process, serial, "QEMU UI first frame published", 18)
            client = connect(qmp_path)
            stream = client.makefile("rwb", buffering=0)
            greeting = json.loads(stream.readline())
            assert "QMP" in greeting
            qmp(stream, "qmp_capabilities")

            tap(process, serial, stream, 694, 563,
                "emulated touch selected page 2")
            wait_log(process, serial, "manage lifecycle build")

            # A same-section press is a strict no-op: no free/rebuild cycle.
            same_offset = tap(process, serial, stream, 130, 108)
            time.sleep(0.3)
            assert "manage lifecycle" not in text(serial)[same_offset:], (
                "same-section tap unexpectedly changed detail ownership"
            )

            row_y = (108, 160, 212, 264, 316, 368, 420, 472)
            # The first configuration visit starts its asynchronous service
            # worker and initially draws a loading placeholder. Initialize
            # every lazy service once before comparing loaded view sizes.
            for section in (1, 2, 3, 4, 5, 6, 7, 0):
                tap(process, serial, stream, 130, row_y[section],
                    "manage lifecycle build")
            measured_offset = len(text(serial))

            # Eight measured round trips (64 real destination changes).
            expected_sections = [index % 8 for index in range(1, 65)]
            for section in expected_sections:
                tap(process, serial, stream, 130, row_y[section],
                    "manage lifecycle build")

            time.sleep(0.8)
            assert_healthy(process, serial)
            output.unlink(missing_ok=True)
            qmp(stream, "screendump", {"filename": str(output)})
            deadline = time.monotonic() + 4
            while not output.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            assert output.exists(), "QEMU did not write the Manage capture"
            assert ppm_dimensions(output) == (1024, 600)
            assert output.stat().st_size >= 1024 * 600 * 3

            all_records = [match.groupdict() for match in LIFECYCLE.finditer(text(serial))]
            assert max(int(record["roots"]) for record in all_records) <= 2
            records = [match.groupdict() for match in
                       LIFECYCLE.finditer(text(serial)[measured_offset:])]
            builds = [record for record in records if record["action"] == "build"]
            assert len(builds) == 64, f"expected 64 measured builds, found {len(builds)}"
            per_section = {index: set() for index in range(8)}
            for record in records:
                roots = int(record["roots"])
                assert roots <= 2, f"unbounded detail roots: {record}"
                assert int(record["internal"]) > 0 and int(record["psram"]) > 0
                if record["action"] == "build":
                    selected = int(record["selected"])
                    rendered = int(record["rendered"])
                    assert selected == rendered
                    assert roots in (1, 2)
                    assert int(record["objects"]) > 0
                    per_section[selected].add(int(record["objects"]))
            for section, counts in per_section.items():
                assert len(counts) == 1, (
                    f"section {section} object count grew across visits: {counts}"
                )

            internal = [int(record["internal"]) for record in builds]
            psram = [int(record["psram"]) for record in builds]
            print(
                "QEMU Manage lifecycle passed: "
                f"64 transitions after one warm-up round, "
                f"max roots {max(int(r['roots']) for r in all_records)}, "
                f"internal {min(internal)}..{max(internal)} bytes free, "
                f"PSRAM {min(psram)}..{max(psram)} bytes free; capture {output}"
            )
        finally:
            if client is not None:
                client.close()
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    main()
