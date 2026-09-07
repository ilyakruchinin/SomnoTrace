#!/usr/bin/env python3
"""Boot the QEMU UI and verify an absolute host click reaches LVGL."""

import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
from qemu_runtime import qmp_socket as private_qmp_socket
import time


ROOT = Path(__file__).resolve().parents[1]


def qmp_command(stream, execute, arguments=None):
    payload = {"execute": execute}
    if arguments is not None:
        payload["arguments"] = arguments
    stream.write(json.dumps(payload).encode() + b"\r\n")
    while True:
        reply = json.loads(stream.readline())
        if "error" in reply:
            raise RuntimeError(reply["error"])
        if "return" in reply:
            return reply["return"]


def log_character_offset(log_path):
    """Return an offset compatible with decoded serial-log text slices."""
    if not log_path.exists():
        return 0
    return len(log_path.read_text(errors="replace"))


def wait_for_log(process, log_path, phrase, timeout, start_offset=0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited with status {process.returncode}")
        if log_path.exists():
            log = log_path.read_text(errors="replace")
            for line in log[start_offset:].splitlines():
                if phrase in line:
                    return line.strip()
        time.sleep(0.1)
    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    raise TimeoutError(f"QEMU did not log {phrase!r}\n{log}")


def tap(process, log_path, stream, pixel_x, pixel_y):
    """Send one touch and hold it until LVGL has sampled the press edge."""
    scaled_x = round(pixel_x * 32767 / 1023)
    scaled_y = round(pixel_y * 32767 / 599)
    start_offset = log_character_offset(log_path)
    qmp_command(stream, "input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": scaled_x}},
        {"type": "abs", "data": {"axis": "y", "value": scaled_y}},
        {"type": "btn", "data": {"button": "left", "down": True}},
    ]})
    try:
        observed = wait_for_log(
            process, log_path, "emulated touch at", 3,
            start_offset=start_offset,
        )
        time.sleep(0.05)
    finally:
        release_offset = log_character_offset(log_path)
        qmp_command(stream, "input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": False}},
        ]})
    wait_for_log(process, log_path, "QEMU touch release sampled", 3,
                 start_offset=release_offset)
    # Give LVGL a polling interval to observe release before another press.
    time.sleep(0.08)
    return observed, start_offset


def drag(process, log_path, stream, start, end, steps=8, step_seconds=0.04):
    """Send a sampled touch drag, used for the scrollable System pane."""
    start_offset = log_character_offset(log_path)
    sx, sy = start
    ex, ey = end
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
        observed = wait_for_log(
            process, log_path, "emulated touch at", 3,
            start_offset=start_offset,
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
    return observed, start_offset


def assert_emulated_backlight_frame(stream, path, dark):
    """Require QEMU's framebuffer to visibly match the backlight state."""
    deadline = time.monotonic() + 2
    brightest = None
    while time.monotonic() < deadline:
        qmp_command(stream, "screendump", {"filename": str(path)})
        with path.open("rb") as image:
            magic = image.readline().strip()
            dimensions = image.readline().strip()
            maximum = image.readline().strip()
            pixels = image.read()
        if magic != b"P6" or dimensions != b"1024 600" or maximum != b"255":
            raise AssertionError(f"unexpected QEMU screendump format in {path}")
        if len(pixels) != 1024 * 600 * 3:
            raise AssertionError(f"incomplete QEMU screendump in {path}")
        brightest = max(pixels)
        if (dark and brightest <= 8) or (not dark and brightest >= 128):
            return
        time.sleep(0.1)
    if dark:
        raise AssertionError(
            f"screen-off framebuffer still contains lit pixels ({brightest})"
        )
    raise AssertionError(
        f"wake did not restore the visible framebuffer ({brightest})"
    )


def sleep_from_header_then_wake_over(
        process, log_path, stream, screen_name, wake_point, destination_page):
    """Exercise shared-header Off now and prove its wake press is consumed."""
    _, off_offset = tap(process, log_path, stream, 566, 35)
    backlight_off = wait_for_log(
        process, log_path, "backlight off", 3,
        start_offset=off_offset,
    )
    assert_emulated_backlight_frame(
        stream, log_path.parent / f"{screen_name}-off.ppm", True
    )

    _, wake_offset = tap(process, log_path, stream, *wake_point)
    backlight_on = wait_for_log(
        process, log_path, "backlight on", 3,
        start_offset=wake_offset,
    )
    time.sleep(0.25)
    assert_emulated_backlight_frame(
        stream, log_path.parent / f"{screen_name}-awake.ppm", False
    )
    wake_end = log_character_offset(log_path)
    wake_log = log_path.read_text(errors="replace")[wake_offset:wake_end]
    leaked_selection = "backlight off"
    if leaked_selection in wake_log:
        raise AssertionError(
            "first touch after shared-header screen-off activated Screen off a second time"
        )
    return backlight_off, backlight_on


def main():
    qemu = subprocess.check_output(
        [ROOT / "scripts/setup-qemu-macos.sh"], text=True
    ).strip()
    flash = ROOT / "build-qemu/qemu_flash.bin"
    efuse = ROOT / "build-qemu/qemu_efuse.bin"
    if not flash.exists() or not efuse.exists():
        subprocess.run([ROOT / "scripts/build-qemu.sh"], check=True)

    subprocess.run(["python3", ROOT / "scripts/qemu-artifacts.py", "verify"], check=True)

    output = ROOT / "build-qemu/captures/touch-smoke"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="run-", dir=ROOT / "build-qemu") as temporary, private_qmp_socket(temporary) as endpoint:
        qmp_socket = endpoint
        serial_log = Path(temporary) / "serial.log"
        serial_log.write_text("")
        command = [
            qemu,
            "-M", "esp32s3", "-snapshot",
            "-m", "8M",
            "-drive", f"file={flash},if=mtd,format=raw",
            "-drive", f"file={efuse},if=none,format=raw,id=efuse",
            "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
            "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
            "-nic", "user,model=open_eth",
            "-display", "sdl,show-cursor=on",
            "-qmp", f"unix:{qmp_socket},server=on,wait=off",
            "-serial", f"file:{serial_log}",
            "-monitor", "none",
        ]
        (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        client = None
        stream = None
        process = subprocess.Popen(
            command,
            cwd=ROOT,
            env={**os.environ, "TMPDIR": temporary},
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            wait_for_log(process, serial_log, "interactive UI preview ready", 12)
            wait_for_log(process, serial_log, "QEMU UI first frame published", 15)
            provenance = json.loads((ROOT / "build-qemu/provenance.json").read_text())
            elf_prefix = provenance["artifacts"]["somnotrace.elf"][:9]
            if f"ELF file SHA256:  {elf_prefix}" not in serial_log.read_text(errors="replace"):
                raise AssertionError("guest ELF differs from this checkout build")
            client = socket.socket(socket.AF_UNIX)
            client.connect(str(qmp_socket))
            stream = client.makefile("rwb", buffering=0)
            json.loads(stream.readline())
            qmp_command(stream, "qmp_capabilities")

            time.sleep(3.5)
            home_off, home_on = sleep_from_header_then_wake_over(
                process, serial_log, stream, "home", (566, 35), 0)
            print(f"Home screen-off/wake passed: {home_off}; {home_on}")
        finally:
            if stream is not None:
                try:
                    qmp_command(stream, "screendump", {
                        "filename": str(Path(temporary) / "final.ppm"),
                    })
                except (OSError, ValueError, RuntimeError):
                    pass  # Preserve the original failure when QEMU has exited.
            if client is not None:
                client.close()
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            for artifact in Path(temporary).iterdir():
                if artifact.suffix in (".log", ".ppm"):
                    (output / artifact.name).write_bytes(artifact.read_bytes())


if __name__ == "__main__":
    main()
