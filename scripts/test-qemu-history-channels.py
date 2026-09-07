#!/usr/bin/env python3
"""Prove History channel pills select and repaint distinct QEMU traces."""

import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
from qemu_runtime import qmp_socket as private_qmp_socket
import time


ROOT = Path(__file__).resolve().parents[1]
WIDTH = 1024
HEIGHT = 600
FRAME_BYTES = WIDTH * HEIGHT * 3
HISTORY_NAV = (512, 559)
CHANNEL_TAPS = {
    "Flow": (802, 276),
    "SpO2": (866, 276),
    "Leak": (929, 276),
}
CHANNEL_LOGS = {
    "Flow": "emulated touch selected history channel 0",
    "SpO2": "emulated touch selected history channel 1",
    "Leak": "emulated touch selected history channel 2",
}
# Insets avoid rounded edges while retaining enough button surface to tell the
# light selected pill from the two dark unselected pills without reading text.
CHANNEL_SURFACES = {
    "Flow": (780, 265, 824, 287),
    "SpO2": (842, 265, 892, 287),
    "Leak": (910, 265, 948, 287),
}
# The chart alone: controls, timestamps, and all changing global chrome are
# deliberately excluded so pixel differences must come from the trace repaint.
CHART_BOUNDS = (664, 306, 966, 414)
HISTORY_VISUAL_BOUNDS = (664, 262, 966, 414)
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


def log_offset(path):
    return len(path.read_text(errors="replace")) if path.exists() else 0


def check_process(process, log_path):
    if process.poll() is not None:
        raise RuntimeError(f"QEMU exited with status {process.returncode}")
    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    for marker in FATAL_MARKERS:
        if marker in log:
            raise RuntimeError(f"QEMU reported {marker!r}\n{log[-4000:]}")


def wait_for_log(process, log_path, phrase, timeout=5, start_offset=0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        check_process(process, log_path)
        log = log_path.read_text(errors="replace") if log_path.exists() else ""
        for line in log[start_offset:].splitlines():
            if phrase in line:
                return line.strip()
        time.sleep(0.05)
    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    raise TimeoutError(f"QEMU did not log {phrase!r}\n{log[-4000:]}")


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


def tap(process, log_path, stream, point, expected_log=None, settle=0.08):
    x, y = point
    scaled_x = round(x * 32767 / (WIDTH - 1))
    scaled_y = round(y * 32767 / (HEIGHT - 1))
    start_offset = log_offset(log_path)
    qmp_command(stream, "input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": scaled_x}},
        {"type": "abs", "data": {"axis": "y", "value": scaled_y}},
        {"type": "btn", "data": {"button": "left", "down": True}},
    ]})
    try:
        wait_for_log(
            process, log_path, "emulated touch at", 3,
            start_offset=start_offset,
        )
        time.sleep(0.05)
    finally:
        release_offset = log_offset(log_path)
        qmp_command(stream, "input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": False}},
        ]})
    wait_for_log(process, log_path, "QEMU touch release sampled", 3,
                 start_offset=release_offset)
    if expected_log:
        wait_for_log(
            process, log_path, expected_log, 3,
            start_offset=start_offset,
        )
    time.sleep(settle)
    return start_offset


def frame_payload(stream, path):
    path.unlink(missing_ok=True)
    qmp_command(stream, "screendump", {"filename": str(path)})
    deadline = time.monotonic() + 3
    while not path.exists() and time.monotonic() < deadline:
        time.sleep(0.025)
    if not path.exists():
        raise TimeoutError(f"QEMU did not create {path}")
    raw = path.read_bytes()
    if len(raw) < FRAME_BYTES:
        raise AssertionError(f"truncated framebuffer capture: {len(raw)} bytes")
    payload = raw[-FRAME_BYTES:]
    if len(payload) != FRAME_BYTES:
        raise AssertionError("unexpected QEMU framebuffer dimensions")
    return payload


def crop(payload, bounds):
    x1, y1, x2, y2 = bounds
    rows = []
    for y in range(y1, y2):
        offset = (y * WIDTH + x1) * 3
        rows.append(payload[offset:offset + (x2 - x1) * 3])
    return b"".join(rows)


def selected_surface_fraction(payload, bounds):
    surface = crop(payload, bounds)
    light = 0
    pixels = len(surface) // 3
    for offset in range(0, len(surface), 3):
        red, green, blue = surface[offset:offset + 3]
        if min(red, green, blue) >= 150:
            light += 1
    return light / pixels


def assert_selected_pill(payload, selected):
    fractions = {
        name: selected_surface_fraction(payload, bounds)
        for name, bounds in CHANNEL_SURFACES.items()
    }
    if fractions[selected] < 0.55:
        raise AssertionError(
            f"{selected} pill did not paint selected: {fractions}"
        )
    for name, fraction in fractions.items():
        if name != selected and fraction > 0.35:
            raise AssertionError(
                f"{name} pill remained selected while {selected} was active: "
                f"{fractions}"
            )
    return fractions


def stable_selected_frame(process, log_path, stream, frame_path, selected,
                          timeout=5):
    """Wait for two identical chart/control captures with `selected` active."""
    deadline = time.monotonic() + timeout
    previous_visual = None
    last_fractions = None
    while time.monotonic() < deadline:
        check_process(process, log_path)
        payload = frame_payload(stream, frame_path)
        try:
            last_fractions = assert_selected_pill(payload, selected)
        except AssertionError:
            previous_visual = None
            time.sleep(0.1)
            continue
        visual = crop(payload, HISTORY_VISUAL_BOUNDS)
        if visual == previous_visual:
            return payload, last_fractions
        previous_visual = visual
        time.sleep(0.1)
    raise AssertionError(
        f"{selected} did not reach a stable selected framebuffer; "
        f"last surfaces={last_fractions}"
    )


def differing_pixels(left, right):
    if len(left) != len(right):
        raise AssertionError("trace crops have different sizes")
    return sum(
        left[offset:offset + 3] != right[offset:offset + 3]
        for offset in range(0, len(left), 3)
    )


def capture_channel(process, log_path, stream, frame_path, channel):
    start_offset = tap(
        process, log_path, stream, CHANNEL_TAPS[channel],
        expected_log=CHANNEL_LOGS[channel], settle=0,
    )
    wait_for_log(
        process, log_path,
        f"emulated history channel {list(CHANNEL_TAPS).index(channel)} "
        "frame published",
        3, start_offset=start_offset,
    )
    check_process(process, log_path)
    payload, fractions = stable_selected_frame(
        process, log_path, stream, frame_path, channel
    )
    trace = crop(payload, CHART_BOUNDS)
    return trace, fractions


def main():
    qemu = subprocess.check_output(
        [ROOT / "scripts/setup-qemu-macos.sh"], text=True
    ).strip()
    # This is a source-level acceptance test, so never silently exercise a
    # stale firmware image left by an earlier build.
    subprocess.run([ROOT / "scripts/build-qemu.sh"], check=True)
    flash = ROOT / "build-qemu/qemu_flash.bin"
    efuse = ROOT / "build-qemu/qemu_efuse.bin"
    if not flash.exists() or not efuse.exists():
        raise FileNotFoundError("QEMU build did not produce flash artifacts")

    subprocess.run(["python3", ROOT / "scripts/qemu-artifacts.py", "verify"], check=True)

    with tempfile.TemporaryDirectory(prefix="run-", dir=ROOT / "build-qemu") as temp, private_qmp_socket(temp) as endpoint:
        temporary = Path(temp)
        qmp_socket = endpoint
        serial_log = temporary / "serial.log"
        frame_path = temporary / "frame.ppm"
        command = [
            qemu,
            "-M", "esp32s3", "-snapshot",
            "-m", "8M",
            "-drive", f"file={flash},if=mtd,format=raw",
            "-drive", f"file={efuse},if=none,format=raw,id=efuse",
            "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
            "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
            "-nic", "user,model=open_eth",
            "-display", "sdl,show-cursor=off",
            "-qmp", f"unix:{qmp_socket},server=on,wait=off",
            "-serial", f"file:{serial_log}",
            "-monitor", "none",
        ]
        process = subprocess.Popen(
            command, cwd=ROOT, env={**os.environ, "TMPDIR": str(temp)}, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        client = None
        try:
            wait_for_log(process, serial_log, "interactive UI preview ready", 15)
            wait_for_log(process, serial_log, "QEMU UI first frame published", 15)
            client = connect_qmp(qmp_socket)
            stream = client.makefile("rwb", buffering=0)
            greeting = json.loads(stream.readline())
            if "QMP" not in greeting:
                raise RuntimeError(f"unexpected QMP greeting: {greeting}")
            qmp_command(stream, "qmp_capabilities")

            nav_offset = log_offset(serial_log)
            tap(process, serial_log, stream, HISTORY_NAV)
            wait_for_log(
                process, serial_log, "emulated touch selected page 1", 3,
                start_offset=nav_offset,
            )
            time.sleep(0.4)

            traces = {}
            fractions = {}
            for channel in ("Flow", "SpO2", "Leak"):
                traces[channel], fractions[channel] = capture_channel(
                    process, serial_log, stream, frame_path, channel
                )

            comparisons = {}
            minimum_difference = 250
            for left, right in (("Flow", "SpO2"), ("Flow", "Leak"),
                                ("SpO2", "Leak")):
                changed = differing_pixels(traces[left], traces[right])
                comparisons[f"{left}/{right}"] = changed
                if changed < minimum_difference:
                    raise AssertionError(
                        f"{left} and {right} traces repaint only {changed} "
                        f"pixels; expected at least {minimum_difference}"
                    )

            # Exercise faster-than-redraw input. The final SpO2 selection must
            # win and repaint back to the exact earlier SpO2 chart pixels.
            rapid_offset = 0
            for channel in ("Flow", "Leak", "Flow", "SpO2"):
                rapid_offset = tap(
                    process, serial_log, stream, CHANNEL_TAPS[channel],
                    expected_log=CHANNEL_LOGS[channel], settle=0.02,
                )
            wait_for_log(
                process, serial_log,
                "emulated history channel 1 frame published", 3,
                start_offset=rapid_offset,
            )
            rapid_payload, rapid_fractions = stable_selected_frame(
                process, serial_log, stream, frame_path, "SpO2"
            )
            rapid_trace = crop(rapid_payload, CHART_BOUNDS)
            if rapid_trace != traces["SpO2"]:
                changed = differing_pixels(rapid_trace, traces["SpO2"])
                raise AssertionError(
                    f"rapid taps left a stale/partial trace ({changed} pixels "
                    "differ from the settled SpO2 repaint)"
                )

            digests = {
                channel: hashlib.sha256(trace).hexdigest()[:12]
                for channel, trace in traces.items()
            }
            print(
                "QEMU History channel acceptance passed: "
                f"digests={digests}; changed_pixels={comparisons}; "
                f"selected_surface={fractions}; rapid_final={rapid_fractions}"
            )
        except Exception:
            log = serial_log.read_text(errors="replace") \
                if serial_log.exists() else ""
            if log:
                print(log[-4000:])
            raise
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
