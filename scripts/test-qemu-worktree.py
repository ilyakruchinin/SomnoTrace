#!/usr/bin/env python3
"""Boot this worktree through its public launcher and check process ownership."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("capture", ROOT / "scripts/capture-qemu-ui.py")
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)

subprocess.run(["python3", ROOT / "scripts/qemu-artifacts.py", "verify"], check=True)
manifest = json.loads((ROOT / "build-qemu/provenance.json").read_text())
flash = ROOT / "build-qemu/qemu_flash.bin"
with tempfile.TemporaryDirectory(prefix="run-", dir=ROOT / "build-qemu") as temp:
    uart = ROOT / "build-qemu/launcher.serial.log"
    with uart.open("w") as output:
        process = subprocess.Popen([ROOT / "scripts/run-qemu-ui.sh"], cwd=ROOT,
            env={**os.environ, "TMPDIR": temp}, stdin=subprocess.DEVNULL,
            stdout=output, stderr=subprocess.STDOUT)
        try:
            capture.wait_for_log(process, uart, "QEMU UI first frame published", 25)
            prefix = manifest["artifacts"]["somnotrace.elf"][:9]
            assert f"ELF file SHA256:  {prefix}" in uart.read_text(errors="replace")
            command = subprocess.check_output(["ps", "-p", str(process.pid), "-o", "command="], text=True)
            assert f"file={flash},if=mtd,format=raw" in command
            assert "-snapshot" in command
            (ROOT / "build-qemu/launcher.command.txt").write_text(command)
            duplicate = subprocess.run([ROOT / "scripts/run-qemu-ui.sh"], cwd=ROOT,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
            assert duplicate.returncode != 0 and "already running" in duplicate.stdout, duplicate.stdout
            capture.wait_healthy(process, uart, 1)
        finally:
            # Signal only the child returned by Popen, never a process-name group.
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
assert hashlib.sha256(flash.read_bytes()).hexdigest() == manifest["artifacts"]["qemu_flash.bin"]
print("Child launcher boot, ELF identity, duplicate refusal and immutable flash passed")
