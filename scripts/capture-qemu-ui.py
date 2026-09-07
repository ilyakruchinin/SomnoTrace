#!/usr/bin/env python3
"""Capture chrome-free 1024x600 UI frames from Espressif QEMU via QMP."""

import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
from qemu_runtime import qmp_socket as private_qmp_socket
import time


ROOT = Path(__file__).resolve().parents[1]
SCREEN_SCENARIOS = {"home": (0, (330, 563)), "home-idle": (0, (330, 563)), "status": (0, (330, 563))}
FATAL_MARKERS = (
    "Guru Meditation Error",
    "assert failed",
    "abort() was called",
    "Invalid drawing area",
)


def qmp_command(stream, execute, arguments=None):
    payload = {"execute": execute}
    if arguments is not None:
        payload["arguments"] = arguments
    stream.write(json.dumps(payload).encode() + b"\r\n")
    while True:
        reply = json.loads(stream.readline())
        if "error" in reply:
            raise RuntimeError(f"QMP {execute} failed: {reply['error']}")
        if "return" in reply:
            return reply["return"]


def wait_for_log(process, log_path, phrase, timeout, start_offset=0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited with status {process.returncode}")
        log = log_path.read_text(errors="replace") if log_path.exists() else ""
        if any(marker in log for marker in FATAL_MARKERS):
            raise RuntimeError("QEMU reported a firmware/display failure")
        if phrase in log[start_offset:]:
            return
        time.sleep(0.1)
    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    raise TimeoutError(f"QEMU did not log {phrase!r}\n{log}")


def wait_healthy(process, log_path, duration):
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited with status {process.returncode}")
        log = log_path.read_text(errors="replace") if log_path.exists() else ""
        if any(marker in log for marker in FATAL_MARKERS):
            raise RuntimeError("QEMU reported a firmware/display failure")
        time.sleep(0.1)


def failure_excerpt(log):
    for marker in FATAL_MARKERS:
        offset = log.find(marker)
        if offset >= 0:
            return log[offset:offset + 4000]
    return log[-4000:]


def connect_qmp(path, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        client = socket.socket(socket.AF_UNIX)
        try:
            client.connect(str(path))
            return client
        except (FileNotFoundError, ConnectionRefusedError):
            client.close()
            time.sleep(0.05)
    raise TimeoutError(f"QMP socket did not become ready: {path}")


def log_character_offset(log_path):
    """Return an offset compatible with wait_for_log's decoded text slices."""
    if not log_path.exists():
        return 0
    return len(log_path.read_text(errors="replace"))


def click(process, log_path, stream, x, y):
    scaled_x = round(x * 32767 / 1023)
    scaled_y = round(y * 32767 / 599)
    log_offset = log_character_offset(log_path)
    qmp_command(stream, "input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": scaled_x}},
        {"type": "abs", "data": {"axis": "y", "value": scaled_y}},
        {"type": "btn", "data": {"button": "left", "down": True}},
    ]})
    try:
        # Keep the pointer down until the guest has actually sampled it. A
        # fixed wall-clock hold can be missed when a full redraw runs slowly.
        wait_for_log(
            process, log_path, "emulated touch at", 3,
            start_offset=log_offset,
        )
        time.sleep(0.05)
    finally:
        release_offset = log_character_offset(log_path)
        qmp_command(stream, "input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": False}},
        ]})
    wait_for_log(process, log_path, "QEMU touch release sampled", 3,
                 start_offset=release_offset)
    # Let LVGL sample the release before another scripted press. Page redraws
    # can otherwise make two adjacent clicks look like one held gesture.
    time.sleep(0.08)


def drag(process, log_path, stream, start, end, steps=8, step_seconds=0.04):
    """Send one sampled finger drag through the QEMU touch bridge."""
    sx, sy = start
    ex, ey = end
    log_offset = log_character_offset(log_path)
    qmp_command(stream, "input-send-event", {"events": [
        {"type": "abs", "data": {
            "axis": "x", "value": round(sx * 32767 / 1023),
        }},
        {"type": "abs", "data": {
            "axis": "y", "value": round(sy * 32767 / 599),
        }},
        {"type": "btn", "data": {"button": "left", "down": True}},
    ]})
    try:
        wait_for_log(
            process, log_path, "emulated touch at", 3,
            start_offset=log_offset,
        )
        for step in range(1, steps + 1):
            x = round(sx + (ex - sx) * step / steps)
            y = round(sy + (ey - sy) * step / steps)
            qmp_command(stream, "input-send-event", {"events": [
                {"type": "abs", "data": {
                    "axis": "x", "value": round(x * 32767 / 1023),
                }},
                {"type": "abs", "data": {
                    "axis": "y", "value": round(y * 32767 / 599),
                }},
            ]})
            time.sleep(step_seconds)
    finally:
        release_offset = log_character_offset(log_path)
        qmp_command(stream, "input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": False}},
        ]})
    wait_for_log(process, log_path, "QEMU touch release sampled", 3,
                 start_offset=release_offset)
    time.sleep(0.15)


def ppm_dimensions(path):
    with path.open("rb") as image:
        tokens = []
        while len(tokens) < 4:
            line = image.readline()
            if not line:
                break
            if line.startswith(b"#"):
                continue
            tokens.extend(line.split())
    if len(tokens) < 4 or tokens[0] != b"P6" or tokens[3] != b"255":
        raise AssertionError(f"unexpected screendump format in {path}")
    return int(tokens[1]), int(tokens[2])


def capture_screen(
    qemu, flash, efuse, output_dir, temporary, name, tab_index, point,
    settle_seconds, representative, interaction=False, socket_path=None,
):
    if socket_path is None:
        with private_qmp_socket(temporary) as endpoint:
            return capture_screen(qemu, flash, efuse, output_dir, temporary,
                name, tab_index, point, settle_seconds, representative,
                interaction=interaction, socket_path=endpoint)
    # macOS limits Unix-domain socket paths to roughly one hundred bytes and
    # its per-user temporary directory can already be long. Keep the private
    # QMP filename short even when the descriptive capture name is not.
    qmp_socket = socket_path
    serial_log = Path(temporary) / f"{name}.serial.log"
    serial_log.write_text("")  # Repeated scenario names still need a fresh boot log.
    command = [
        qemu,
        "-M", "esp32s3", "-snapshot",
        "-m", "8M",
        "-drive", f"file={flash},if=mtd,format=raw",
        "-drive", f"file={efuse},if=none,format=raw,id=efuse",
        "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
        "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
        "-nic", "user,model=open_eth",
        # SDL instantiates QEMU's pointer input handler. screendump still reads
        # the guest console surface directly, so the PPM contains no host
        # window border, title bar, or cursor.
        "-display", "sdl,show-cursor=off",
        "-qmp", f"unix:{qmp_socket},server=on,wait=off",
        "-serial", f"file:{serial_log}",
        "-monitor", "none",
    ]
    process = subprocess.Popen(
        command, cwd=ROOT, env={**os.environ, "TMPDIR": str(temporary)}, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    (output_dir / f"{name}.command.json").write_text(json.dumps(command, indent=2) + "\n")
    client = None
    try:
        wait_for_log(process, serial_log, "interactive UI preview ready", 15)
        # app_main returns to its simulator loop before LVGL has necessarily
        # finished the initial 1024x600 paint. Never inject a tap into that
        # first render merely because the application banner was printed.
        wait_for_log(process, serial_log, "QEMU UI first frame published", 15)
        # Prove the guest booted the ELF from this checkout, not just a path
        # bearing its name. ESP-IDF prints the beginning of the ELF digest.
        provenance = json.loads((ROOT / "build-qemu/provenance.json").read_text())
        elf_prefix = provenance["artifacts"]["somnotrace.elf"][:9]
        if f"ELF file SHA256:  {elf_prefix}" not in serial_log.read_text(errors="replace"):
            raise AssertionError("guest ELF digest differs from this checkout's build")
        client = connect_qmp(qmp_socket)
        stream = client.makefile("rwb", buffering=0)
        greeting = json.loads(stream.readline())
        if "QMP" not in greeting:
            raise RuntimeError(f"unexpected QMP greeting: {greeting}")
        qmp_command(stream, "qmp_capabilities")
        # The QEMU firmware deliberately announces simulated data in a
        # three-second notice. Let that transient state clear so captures show
        # the persistent header and page geometry beneath it.
        wait_healthy(process, serial_log, settle_seconds)

        if representative or name == "home-idle":
            click(process, serial_log, stream, 858, 458)
            wait_healthy(process, serial_log, 3.5)
        if name == "status":
            click(process, serial_log, stream, 900, 35)
            wait_healthy(process, serial_log, 0.5)
        # Leave at least one complete post-release refresh period before the
        # first attempt. The validation loop below handles slower host loads.
        wait_healthy(process, serial_log, 0.10 if name == "logs-save-progress" else 0.75)
        destination = output_dir / f"{name}.ppm"
        last_validation_error = None
        for attempt in range(8):
            destination.unlink(missing_ok=True)
            qmp_command(stream, "screendump", {"filename": str(destination)})
            deadline = time.monotonic() + 3
            while not destination.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            if not destination.exists():
                raise TimeoutError(f"QEMU did not create {destination}")
            dimensions = ppm_dimensions(destination)
            if dimensions != (1024, 600):
                raise AssertionError(
                    f"{destination.name} is {dimensions[0]}x{dimensions[1]}, expected 1024x600"
                )
            if destination.stat().st_size < 1024 * 600 * 3:
                raise AssertionError(f"truncated framebuffer capture: {destination}")
            payload = destination.read_bytes()[-1024 * 600 * 3:]
            sampled_colours = {
                payload[offset:offset + 3]
                for offset in range(0, len(payload) - 2, 3 * 997)
            }
            if len(sampled_colours) < 8:
                last_validation_error = AssertionError(
                    f"blank or nearly uniform framebuffer capture: {destination}"
                )
            else:
                try:
                    validate_persistent_shell(name, payload)
                    last_validation_error = None
                    break
                except AssertionError as error:
                    last_validation_error = error
            if attempt < 7:
                wait_healthy(process, serial_log, 0.4)
        if last_validation_error is not None:
            raise last_validation_error
        print(f"Captured {name}: {destination} (1024x600 PPM)", flush=True)
    except Exception:
        log = serial_log.read_text(errors="replace") if serial_log.exists() else ""
        if log:
            print(failure_excerpt(log))
        raise
    finally:
        if serial_log.exists():
            (output_dir / f"{name}.serial.log").write_bytes(serial_log.read_bytes())
        if client is not None:
            client.close()
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def validate_persistent_shell(name, payload):
    anchors = {
        "clock": ((20, 8, 205, 58), 80),
        "status": ((724, 5, 1010, 65), 45),
        "Home navigation": ((244, 531, 416, 594), 35),
    }
    for label, (bounds, minimum) in anchors.items():
        x1, y1, x2, y2 = bounds
        bright = sum(max(payload[(y*1024+x)*3:(y*1024+x)*3+3]) >= 80
                     for y in range(y1,y2,2) for x in range(x1,x2,2))
        if bright < minimum:
            raise AssertionError(f"persistent {label} is absent from {name}.ppm")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_dir", nargs="?", default=str(ROOT / "build-qemu/captures"))
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--settle-seconds", type=float, default=3.5)
    parser.add_argument("--screen", action="append", choices=tuple(SCREEN_SCENARIOS))
    parser.add_argument("--representative", action="store_true")
    args = parser.parse_args()
    if args.build:
        subprocess.run([ROOT / "scripts/build-qemu.sh"], check=True)
    subprocess.run(["python3", ROOT / "scripts/qemu-artifacts.py", "verify"], check=True)
    qemu = subprocess.check_output([ROOT / "scripts/setup-qemu-macos.sh"], text=True).strip()
    output = Path(args.output_dir).resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "tested-provenance.json").write_bytes((ROOT / "build-qemu/provenance.json").read_bytes())
    with tempfile.TemporaryDirectory(prefix="capture-", dir=ROOT / "build-qemu") as temporary:
        for name in args.screen or ["home"]:
            capture_screen(qemu, ROOT / "build-qemu/qemu_flash.bin", ROOT / "build-qemu/qemu_efuse.bin", output, temporary, name, 0, None, args.settle_seconds, args.representative)

if __name__ == "__main__":
    main()
