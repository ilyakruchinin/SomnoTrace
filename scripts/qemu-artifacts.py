#!/usr/bin/env python3
"""Bind QEMU firmware to the checkout and source state that built it."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from qemu_targets import BOARDS, target

ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = ("qemu_flash.bin", "qemu_efuse.bin", "somnotrace.elf",
             "project_description.json")


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args])


def source_state(board="7b"):
    # Hash actual firmware inputs, including staged and unstaged edits.
    # Also include untracked source files used by component globs. Build/cache
    # outputs are ignored by Git. Capture tooling and docs need no recompilation.
    digest = hashlib.sha256()
    paths = git("ls-files", "-z", "--cached", "--others", "--exclude-standard")
    for raw in sorted(set(paths.split(b"\0")) - {b""}):
        name = raw.decode()
        if not (name.startswith(("main/", "components/", "third_party/", "assets/"))
                or name in ("CMakeLists.txt", "dependencies.lock", "scripts/idf.sh",
                            "scripts/idf-command.sh", "scripts/idf-sdk-patches.py")
                or name.startswith(("sdkconfig", "partitions"))):
            continue
        path = ROOT / name
        digest.update(raw + b"\0")
        if path.is_file():
            digest.update(path.read_bytes())
        else:
            digest.update(b"<absent>")
    return {"root": str(ROOT), **target(board),
            "revision": git("rev-parse", "HEAD").decode().strip(),
            "source_sha256": digest.hexdigest()}


def checksum(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def configuration_identity(board):
    profile = target(board)
    sdkconfig = ROOT / profile["sdkconfig"]
    settings = dict(line.split("=", 1) for line in sdkconfig.read_text().splitlines()
                    if line.startswith("CONFIG_") and "=" in line)
    if settings.get("CONFIG_SOMNOTRACE_BOARD_QEMU") != "y":
        raise RuntimeError("QEMU configuration does not select CONFIG_SOMNOTRACE_BOARD_QEMU")
    compact = settings.get("CONFIG_SOMNOTRACE_QEMU_DISPLAY_154") == "y"
    if compact != (board == "154"):
        raise RuntimeError(f"QEMU configuration board differs from requested {board}")
    if settings.get("CONFIG_IDF_TARGET") != '"esp32s3"':
        raise RuntimeError("QEMU configuration target must be esp32s3")
    # partitions.csv places the first application at 0x10000. Its custom
    # descriptor follows the 24-byte image header, 8-byte segment header and
    # 256-byte esp_app_desc_t, as checked by firmware_target.c during OTA.
    board_tag = b"qemu-154" if board == "154" else b"qemu-ui"
    with (ROOT / profile["build_dir"] / "qemu_flash.bin").open("rb") as flash:
        flash.seek(0x10000 + 24 + 8 + 256)
        descriptor = flash.read(32)
    if descriptor != b"SomnoTraceTarget" + board_tag.ljust(16, b"\0"):
        raise RuntimeError(f"QEMU firmware board descriptor differs from requested {board}")
    description = json.loads((ROOT / profile["build_dir"] / "project_description.json").read_text())
    # idf.sh mounts this checkout at /project, including linked worktrees.
    for key, expected in {
        "target": "esp32s3", "project_path": "/project",
        "build_dir": f"/project/{profile['build_dir']}",
        "config_file": f"/project/{profile['sdkconfig']}",
        "config_defaults": ";".join(f"/project/{name}" for name in profile["defaults"].split(";")),
    }.items():
        if description.get(key) != expected:
            raise RuntimeError(f"QEMU build {key} differs from requested {board} profile")
    sdk = {key: description[key] for key in ("git_revision", "idf_path", "c_compiler")}
    if not all(isinstance(value, str) and value for value in sdk.values()):
        raise RuntimeError("QEMU SDK identity is incomplete")
    return {"sdkconfig_sha256": checksum(sdkconfig), "sdk": sdk}


def record(expected, board="7b"):
    current = source_state(board)
    if current != expected:
        raise RuntimeError("Sources changed during QEMU build; rebuild this checkout")
    build = ROOT / target(board)["build_dir"]
    data = {**current, **configuration_identity(board),
            "artifacts": {name: checksum(build / name) for name in ARTIFACTS}}
    (build / "provenance.json").write_text(json.dumps(data, indent=2) + "\n")
    return data


def verify(board="7b"):
    build = ROOT / target(board)["build_dir"]
    manifest = build / "provenance.json"
    if not manifest.exists():
        raise RuntimeError(f"QEMU provenance is missing; run ./scripts/build-qemu.sh --board {board} here")
    data = json.loads(manifest.read_text())
    for key, value in source_state(board).items():
        if data.get(key) != value:
            raise RuntimeError(f"QEMU {key} differs from this checkout; rebuild here")
    for name in ARTIFACTS:
        if checksum(build / name) != data["artifacts"].get(name):
            raise RuntimeError(f"QEMU artifact changed: {name}; rebuild here")
    for key, value in configuration_identity(board).items():
        if data.get(key) != value:
            raise RuntimeError(f"QEMU configuration {key} changed; rebuild here")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("source-state", "record", "verify"))
    parser.add_argument("--source-state")
    parser.add_argument("--board", choices=BOARDS, default="7b")
    args = parser.parse_args()
    try:
        if args.action == "source-state":
            print(json.dumps(source_state(args.board)))
        elif args.action == "record":
            if args.source_state is None:
                parser.error("record requires --source-state from before the build")
            record(json.loads(args.source_state), args.board)
        else:
            data = verify(args.board)
            print(f"QEMU board: {data['board']} ({data['width']}x{data['height']})")
            print(f"QEMU source: {data['root']} @ {data['revision']}")
            print(f"QEMU flash SHA256: {data['artifacts']['qemu_flash.bin']}")
    except (RuntimeError, OSError, KeyError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
