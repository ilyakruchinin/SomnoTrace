#!/usr/bin/env python3
"""Retain a headless QEMU boot receipt and measure the guest framebuffer."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time

from qemu_runtime import qmp_socket
from qemu_targets import BOARDS, target

ROOT = Path(__file__).resolve().parents[1]
FATAL = ("Invalid drawing area", "assert failed", "Guru Meditation Error", "abort() was called")


def healthy_log(process, log):
    text = log.read_text(errors="replace") if log.exists() else ""
    if any(marker in text for marker in FATAL):
        raise RuntimeError("QEMU reported a display or firmware failure")
    if process.poll() is not None:
        raise RuntimeError(f"QEMU exited during the boot smoke test ({process.returncode})")
    return text


def validate_uart(text, profile, provenance):
    if profile["ready_log"] not in text:
        raise RuntimeError(f"QEMU did not publish the {profile['board']} ready marker")
    hashes = re.findall(r"ELF file SHA256:\s+([0-9a-fA-F]{9,64})", text)
    expected = provenance["artifacts"]["somnotrace.elf"]
    if not hashes or any(not expected.startswith(value.lower()) for value in hashes):
        raise RuntimeError("QEMU guest ELF identity differs from the selected build")


def validate_frame(path, profile):
    with path.open("rb") as image:
        tokens = []
        while len(tokens) < 4:
            line = image.readline()
            if not line:
                raise RuntimeError("Incomplete QEMU framebuffer header")
            tokens.extend(line.split(b"#", 1)[0].split())
        pixels = image.read()
    if len(tokens) != 4 or tokens[0] != b"P6" or tokens[3] != b"255":
        raise RuntimeError("Unexpected QEMU framebuffer format")
    dimensions = tuple(map(int, tokens[1:3]))
    expected = (profile["width"], profile["height"])
    if dimensions != expected:
        raise RuntimeError(f"QEMU framebuffer dimensions {dimensions} differ from {expected}")
    if len(pixels) != expected[0] * expected[1] * 3:
        raise RuntimeError("Incomplete QEMU framebuffer pixels")
    colours = {pixels[index:index + 3] for index in range(0, len(pixels), 3)}
    if len(colours) < 8:
        raise RuntimeError("QEMU framebuffer is blank or has too few colours")
    return {"width": dimensions[0], "height": dimensions[1],
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def qmp_reply(stream, expected_id=None):
    while True:
        line = stream.readline()
        if not line:
            raise RuntimeError("QEMU closed QMP before replying")
        reply = json.loads(line)
        if expected_id is None and "QMP" in reply:
            return reply
        if expected_id is not None and reply.get("id") == expected_id:
            if "error" in reply:
                raise RuntimeError(f"QMP failed: {reply['error']}")
            return reply


def qmp_command(stream, command, arguments=None):
    request = {"execute": command, "id": command}
    if arguments is not None:
        request["arguments"] = arguments
    stream.write(json.dumps(request).encode() + b"\n")
    stream.flush()
    return qmp_reply(stream, command)


def boot(qemu, board):
    profile = target(board)
    spec = importlib.util.spec_from_file_location("qemu_artifacts", ROOT / "scripts/qemu-artifacts.py")
    artifacts = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(artifacts)
    provenance = artifacts.verify(board)
    build = ROOT / profile["build_dir"]
    retained = Path(tempfile.mkdtemp(prefix="boot-smoke-", dir=build))
    print(f"QEMU boot evidence: {retained}", flush=True)
    (retained / "tested-provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    log = retained / "uart.log"
    with qmp_socket(retained) as endpoint:
        command = [str(qemu), "-M", "esp32s3", "-snapshot", "-S", "-m", "8M",
                   "-drive", f"file={build / 'qemu_flash.bin'},if=mtd,format=raw",
                   "-drive", f"file={build / 'qemu_efuse.bin'},if=none,format=raw,id=efuse",
                   "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
                   "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
                   "-nic", "user,model=open_eth", "-display", "none", "-monitor", "none",
                   "-serial", f"file:{log}", "-qmp", f"unix:{endpoint},server=on,wait=off"]
        (retained / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        with (retained / "qemu.log").open("w") as output:
            process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.DEVNULL,
                                       stdout=output, stderr=subprocess.STDOUT,
                                       env={**os.environ, "TMPDIR": str(retained)})
            try:
                deadline = time.monotonic() + 10
                while not endpoint.exists():
                    healthy_log(process, log)
                    if time.monotonic() >= deadline:
                        raise RuntimeError("Timed out waiting for QEMU control socket")
                    time.sleep(0.05)
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.settimeout(5)
                    connection.connect(str(endpoint))
                    with connection.makefile("rwb") as stream:
                        qmp_reply(stream)
                        qmp_command(stream, "qmp_capabilities")
                        qmp_command(stream, "cont")
                        frame = retained / "frame.ppm"
                        # Headless QEMU has no GUI refresh listener. Pump its
                        # virtual RGB device before waiting for a guest flush
                        # receipt, otherwise both sides wait indefinitely.
                        deadline = time.monotonic() + 30
                        while profile["ready_log"] not in healthy_log(process, log):
                            if time.monotonic() >= deadline:
                                raise RuntimeError(f"Timed out waiting for {profile['ready_log']}")
                            qmp_command(stream, "screendump", {"filename": str(frame)})
                            time.sleep(0.05)
                        # Retain the post-startup health window while continuing
                        # to service display requests from the running guest.
                        deadline = time.monotonic() + 2
                        while time.monotonic() < deadline:
                            healthy_log(process, log)
                            qmp_command(stream, "screendump", {"filename": str(frame)})
                            time.sleep(0.05)
                        validate_uart(healthy_log(process, log), profile, provenance)
                        qmp_command(stream, "screendump", {"filename": str(frame)})
                        measured = validate_frame(frame, profile)
                healthy_log(process, log)
                if artifacts.verify(board) != provenance:
                    raise RuntimeError("QEMU provenance changed during boot smoke")
                (retained / "result.json").write_text(json.dumps({"board": board,
                    "frame": measured, "guest_elf_sha256": provenance["artifacts"]["somnotrace.elf"]}, indent=2) + "\n")
            except Exception as error:
                (retained / "failure.txt").write_text(str(error) + "\n")
                raise
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
    print(f"QEMU UI boot smoke test passed: {board} ({profile['width']}x{profile['height']})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", choices=BOARDS, default="7b")
    parser.add_argument("--qemu", required=True)
    args = parser.parse_args()
    try:
        boot(args.qemu, args.board)
    except (RuntimeError, OSError, ValueError, KeyError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
