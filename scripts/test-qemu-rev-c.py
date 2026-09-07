#!/usr/bin/env python3
"""Assert integrated Rev C editor masking and cancellation through real touch."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
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
output = ROOT / "build-qemu/captures/rev-c-behavior"
output.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="run-", dir=ROOT / "build-qemu") as temp, private_qmp_socket(temp) as endpoint:
    uart = output / "serial.log"
    uart.write_text("")  # QEMU appends; readiness must belong to this boot.
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
        provenance = json.loads((ROOT / "build-qemu/provenance.json").read_text())
        elf_prefix = provenance["artifacts"]["somnotrace.elf"][:9]
        assert f"ELF file SHA256:  {elf_prefix}" in uart.read_text(errors="replace"), "wrong guest ELF"
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

        def expected_frame(name, predicate, message):
            # A sampled release precedes LVGL's next full-frame publication.
            # Require the expected pixels within the same three-second
            # transition bound used by native rendered-view markers.
            deadline = time.monotonic() + 3
            while True:
                frame = shot(name)
                if predicate(frame):
                    return frame
                if time.monotonic() >= deadline:
                    raise AssertionError(message)
                wait(0.1)

        def restored_frame(name, reference, box, message):
            return expected_frame(name,
                lambda frame: region(frame, box) == region(reference, box), message)

        wait(3.5)
        tap(694, 563)
        tap(110, 150)
        overview = shot("connectivity-overview")
        tap(550, 175)
        form = (260, 149, 988, 390)
        title_box = (252, 76, 745, 112)
        network = expected_frame("network-before",
            lambda frame: region(frame, title_box) != region(overview, title_box),
            "saved network did not open")
        tap(690, 223)
        def editor_visible(frame):
            if region(frame, title_box) == region(network, title_box):
                return False
            try:
                capture.validate_interaction_frame("connectivity-password-keyboard", frame)
                capture.validate_persistent_shell("connectivity-password-keyboard", frame, False, True)
                return True
            except AssertionError:
                return False

        empty = expected_frame("password-empty", editor_visible,
                               "complete password editor did not appear")
        # Disposable editor text, never saved or used for a connection attempt.
        for x, y in ((660, 363), (687, 303), (756, 422), (552, 303)):
            tap(x, y)
        wait(1.0)
        text_box = (285, 169, 305, 193)  # Middle glyphs; exclude empty/end cursor.
        masked = expected_frame("password-masked",
            lambda frame: region(frame, text_box) != region(empty, text_box),
            "keyboard entered no text")
        tap(925, 240)
        expected_frame("password-revealed",
            lambda frame: region(frame, text_box) != region(masked, text_box),
            "Show did not reveal text")
        tap(925, 240)
        wait(1.0)
        restored_frame("password-remasked", masked, text_box, "Hide did not remask text")
        tap(820, 90)  # Cancel editor: no credential save.
        restored_frame("password-cancelled", network, form, "Cancel changed saved network form")
        tap(690, 223)
        restored_frame("password-reopened", empty, text_box,
                       "cancelled secret survived editor teardown")
        tap(820, 90)
        tap(690, 373)  # Forget prompt only; never confirm.
        expected_frame("forget-confirmation",
            lambda frame: region(frame, title_box) != region(network, title_box),
            "Forget bypassed confirmation")
        tap(365, 399)
        restored_frame("forget-cancelled", network, form,
                       "Forget Cancel did not restore the unchanged saved network")
        tap(940, 90)  # Back to network overview.
        restored_frame("connectivity-restored", overview, (260, 145, 988, 383),
                       "network priority/list changed during cancelled edits")

        # Stop only simulated therapy via the public Home input before opening
        # the destructive hold prompt. Do not press or hold its action control.
        tap(330, 563)
        tap(858, 458)
        wait(3.4)
        tap(694, 563)
        advanced_offset = capture.log_character_offset(uart)
        tap(110, 462)
        capture.wait_for_log(process, uart, "QEMU maintenance frame view=advanced", 3,
                             start_offset=advanced_offset)
        advanced = shot("advanced-before")
        hold_offset = capture.log_character_offset(uart)
        tap(550, 348)
        capture.wait_for_log(process, uart, "QEMU maintenance frame view=hold", 3,
                             start_offset=hold_offset)
        hold = shot("advanced-delete-confirmation")
        assert region(hold, form) != region(advanced, form), "destructive action has no confirmation view"
        cancel_offset = capture.log_character_offset(uart)
        tap(875, 362)  # Cancel, never the hold target.
        capture.wait_for_log(process, uart, "QEMU maintenance frame view=advanced", 3,
                             start_offset=cancel_offset)
        restored_frame("advanced-cancelled", advanced, form,
                       "maintenance Cancel did not restore Advanced")
        wait(1.0)
        print("Rev C native password input/masking/cancel, secret teardown, forget cancellation, and destructive-prompt cancellation passed")
    finally:
        if client is not None:
            client.close()
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
