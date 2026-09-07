"""Disposable build/launch/boot fixtures used by qemu_worktree_test.py."""
import importlib.util
import json
import shutil
import subprocess

from qemu_targets import target


def command(*args, env=None, success=True):
    result = subprocess.run(args, env=env, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=20)
    assert (result.returncode == 0) == success, result.stdout
    return result.stdout


def rejects(fn, phrase):
    try:
        fn()
    except RuntimeError as error:
        assert phrase in str(error), error
    else:
        raise AssertionError(f"accepted invalid {phrase}")


DOCKER = r'''#!/usr/bin/env python3
import hashlib, json, os, shlex, sys
from pathlib import Path
argv = sys.argv[1:]
root = Path(next(arg[:-9] for arg in argv if arg.endswith(':/project')))
args = argv[argv.index('/project/scripts/idf-command.sh') + 1:]
with open(os.environ['QEMU_TEST_CALLS'], 'a') as output:
    output.write(json.dumps(args) + '\n')
if args[0] == 'idf.py':
    build = args[args.index('-B') + 1]
    config = next(arg.split('=', 1)[1] for arg in args if arg.startswith('SDKCONFIG='))
    assert (build, config) in [('build-qemu', 'sdkconfig.qemu'), ('build-qemu-154', 'sdkconfig.qemu-154')]
    folder = root / build
    folder.mkdir(exist_ok=True)
    if args[-1] == 'reconfigure':
        defaults = next(arg.split('=', 1)[1] for arg in args if arg.startswith('SDKCONFIG_DEFAULTS='))
        (root / config).write_text('CONFIG_SOMNOTRACE_BOARD_QEMU=y\nCONFIG_IDF_TARGET="esp32s3"\n' +
            ('CONFIG_SOMNOTRACE_QEMU_DISPLAY_154=y\n' if build.endswith('-154') else
             '# CONFIG_SOMNOTRACE_QEMU_DISPLAY_154 is not set\n'))
        (folder / 'project_description.json').write_text(json.dumps({
            'target': 'esp32s3', 'project_path': '/project', 'build_dir': '/project/' + build,
            'config_file': '/project/' + config,
            'config_defaults': ';'.join('/project/' + name for name in defaults.split(';')),
            'git_revision': 'v5.5.1-dirty', 'idf_path': '/opt/esp/idf', 'c_compiler': '/opt/esp/gcc'}))
    elif args[-1] == 'build':
        (folder / 'somnotrace.elf').write_bytes(('fixture-elf-' + build).encode())
    elif args[-1] != 'fullclean':
        raise AssertionError(args)
else:
    assert args[:2] == ['bash', '-lc'], args
    words = shlex.split(args[2])
    assert words[0] == 'cd' and words[2] == '&&', words
    folder = root / words[1].removeprefix('/project/')
    tag = b'qemu-154' if folder.name.endswith('-154') else b'qemu-ui'
    (folder / 'qemu_flash.bin').write_bytes(b'\0' * (0x10000 + 288) +
        b'SomnoTraceTarget' + tag.ljust(16, b'\0') +
        b'fixture-flash-' + (folder / 'somnotrace.elf').read_bytes())
'''

QEMU = r'''#!/usr/bin/env python3
import hashlib, json, os, signal, socket, sys
from pathlib import Path
args = sys.argv[1:]
flash = Path(next(arg.split(',')[0][5:] for arg in args if arg.startswith('file=') and ',if=mtd,' in arg))
build = flash.parent
board = '154' if build.name.endswith('-154') else '7b'
assert '-snapshot' in args
assert Path(os.environ['TMPDIR']).is_relative_to(build)
if '-qmp' not in args:
    print(json.dumps({'args': args, 'tmpdir': os.environ['TMPDIR']}))
    sys.exit(0)
serial = Path(args[args.index('-serial') + 1].removeprefix('file:'))
ready = ('240x240 original-board UI preview ready' if board == '154' else
         '1024x600 interactive UI preview ready')
elf = hashlib.sha256((build / 'somnotrace.elf').read_bytes()).hexdigest()[:9]
if os.environ.get('QEMU_TEST_BAD_ELF'): elf = '0' * 9
serial.write_text('ELF file SHA256:  ' + elf + '\n')
if os.environ.get('QEMU_TEST_FATAL'):
    with serial.open('a') as output: output.write('Guru Meditation Error\n')
endpoint = args[args.index('-qmp') + 1].split(',')[0].removeprefix('unix:')
ready_written = False
with socket.socket(socket.AF_UNIX) as server:
    server.bind(endpoint)
    server.listen(1)
    connection, _ = server.accept()
    with connection, connection.makefile('rwb') as stream:
        stream.write(b'{"QMP": {}}\n'); stream.flush()
        for line in stream:
            request = json.loads(line)
            if request['execute'] == 'screendump':
                dimensions = (240, 240) if board == '154' else (1024, 600)
                if os.environ.get('QEMU_TEST_BAD_FRAME'): dimensions = (800, 600)
                width, height = dimensions
                pixels = (bytes(range(256)) * (width * height * 3 // 256 + 1))[:width * height * 3]
                Path(request['arguments']['filename']).write_bytes(f'P6\n{width} {height}\n255\n'.encode() + pixels)
                # Model the real headless device: a guest cannot finish its
                # first synchronous RGB flush until a host refresh occurs.
                if not ready_written:
                    with serial.open('a') as output: output.write(ready + '\n')
                    ready_written = True
            stream.write(json.dumps({'return': {}, 'id': request['id']}).encode() + b'\n'); stream.flush()
signal.pause()  # A real emulator stays alive after QMP disconnects.
'''


def exercise_variants(source_root, root, base, environment):
    scripts = root / "scripts"
    for name in ("build-qemu.sh", "run-qemu-ui.sh", "test-qemu-ui.sh", "qemu_targets.py",
                 "qemu-artifacts.py", "qemu_boot_smoke.py", "qemu_runtime.py",
                 "idf-command.sh", "idf-sdk-patches.py"):
        shutil.copy(source_root / "scripts" / name, scripts / name)
    for name in ("sdkconfig.defaults", "sdkconfig.qemu.defaults", "sdkconfig.qemu-154.defaults"):
        shutil.copy(source_root / name, root / name)
    patch = root / "third_party/esp-idf-patches/fixture.c"
    patch.parent.mkdir(parents=True)
    patch.write_text("SDK async allocation unwind fixture\n")
    fake_bin = base / "bin"
    (fake_bin / "docker").write_text(DOCKER)
    emulator = fake_bin / "qemu-system-xtensa"
    emulator.write_text(QEMU)
    emulator.chmod(0o755)
    setup = scripts / "setup-qemu-macos.sh"
    setup.write_text("#!/usr/bin/env python3\nimport os\nprint(os.environ['QEMU_TEST_BINARY'])\n")
    setup.chmod(0o755)
    ps = fake_bin / "ps"
    ps.write_text("#!/usr/bin/env python3\nimport os\nprint(os.environ.get('QEMU_TEST_PS', ''))\n")
    ps.chmod(0o755)
    calls_path = base / "calls.jsonl"
    env = {**environment, "QEMU_TEST_CALLS": str(calls_path), "QEMU_TEST_BINARY": str(emulator)}
    command("git", "-C", str(root), "add", ".")
    command("git", "-C", str(root), "commit", "-qm", "variant fixtures")
    spec = importlib.util.spec_from_file_location("provenance", scripts / "qemu-artifacts.py")
    artifacts = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(artifacts)
    artifacts.ROOT = root

    for board in ("7b", "154"):
        profile = target(board)
        build = root / profile["build_dir"]
        calls_path.write_text("")
        options = [] if board == "7b" else ["--board", board]
        command(str(scripts / "build-qemu.sh"), "--clean", *options, env=env)
        calls = [json.loads(line) for line in calls_path.read_text().splitlines()]
        assert [call[-1] for call in calls[:3]] == ["fullclean", "reconfigure", "build"]
        for call in calls[:3]:
            assert call[call.index("-B") + 1] == profile["build_dir"]
            assert f"SDKCONFIG={profile['sdkconfig']}" in call
        for call in calls[1:3]:
            assert f"SDKCONFIG_DEFAULTS={profile['defaults']}" in call
        assert f"/project/{profile['build_dir']}" in calls[3][-1]
        assert (build / "qemu_efuse.bin").read_bytes()[38] == 12
        receipt = artifacts.verify(board)
        assert (receipt["board"], receipt["width"], receipt["height"]) == (
            board, profile["width"], profile["height"])
        assert receipt["sdk"]["git_revision"] == "v5.5.1-dirty"
        launch = json.loads(command(str(scripts / "run-qemu-ui.sh"), *options, env=env).splitlines()[-1])
        assert f"file={build}/qemu_flash.bin,if=mtd,format=raw" in launch["args"]
        assert launch["tmpdir"] == str(build)
        # A running other-board process must not block this board; the same image must.
        other = target("154" if board == "7b" else "7b")
        other_process = f"42 qemu-system-xtensa -drive file={root / other['build_dir']}/qemu_flash.bin,if=mtd,format=raw"
        command(str(scripts / "run-qemu-ui.sh"), *options, env={**env, "QEMU_TEST_PS": other_process})
        same_process = f"42 qemu-system-xtensa -drive file={build}/qemu_flash.bin,if=mtd,format=raw"
        output = command(str(scripts / "run-qemu-ui.sh"), *options,
                         env={**env, "QEMU_TEST_PS": same_process}, success=False)
        assert "already running" in output
        calls_path.write_text("")
        command(str(scripts / "run-qemu-ui.sh"), "--build", *options, env=env)
        assert len(calls_path.read_text().splitlines()) == 3
        (build / "qemu_flash.bin").unlink()
        calls_path.write_text("")
        command(str(scripts / "run-qemu-ui.sh"), *options, env=env)
        assert len(calls_path.read_text().splitlines()) == 3
        calls_path.write_text("")
        output = command(str(scripts / "test-qemu-ui.sh"), *options, "--build", env=env)
        assert len(calls_path.read_text().splitlines()) == 3
        assert f"test passed: {board} ({profile['width']}x{profile['height']})" in output
        evidence = sorted(build.glob("boot-smoke-*/result.json"))
        assert len(evidence) == 1
        observed = json.loads(evidence[0].read_text())
        assert observed["frame"]["width"] == profile["width"]
        assert observed["guest_elf_sha256"] == receipt["artifacts"]["somnotrace.elf"]
        artifacts.verify(board)

    # Both configurations coexist. Rebuilding either one leaves the other's receipt valid.
    artifacts.verify("7b")
    artifacts.verify("154")
    for board in ("7b", "154"):
        profile = target(board)
        build = root / profile["build_dir"]
        manifest = build / "provenance.json"
        original = manifest.read_text()
        for key, changed in (("root", str(base)), ("board", "other"), ("width", 800),
                             ("height", 600 if board == "154" else 240),
                             ("sdk", {}), ("sdkconfig", "sdkconfig.foreign")):
            data = json.loads(original)
            data[key] = changed
            manifest.write_text(json.dumps(data))
            rejects(lambda: artifacts.verify(board), key)
        manifest.write_text(original)
        foreign = root / target("154" if board == "7b" else "7b")["build_dir"] / "provenance.json"
        manifest.write_text(foreign.read_text())
        rejects(lambda: artifacts.verify(board), "board")
        manifest.write_text(original)
        for name in artifacts.ARTIFACTS:
            path = build / name
            before = path.read_bytes()
            path.write_bytes(before + b"foreign artifact")
            rejects(lambda: artifacts.verify(board), "artifact changed")
            path.write_bytes(before)
        flash = build / "qemu_flash.bin"
        before = flash.read_bytes()
        for descriptor in (b"\0" * 32,
                           b"SomnoTraceTarget" + b"waveshare-7b".ljust(16, b"\0")):
            at = 0x10000 + 288
            flash.write_bytes(before[:at] + descriptor + before[at + 32:])
            rejects(lambda: artifacts.record(artifacts.source_state(board), board),
                    "firmware board descriptor")
        flash.write_bytes(before)
        config = root / profile["sdkconfig"]
        before = config.read_text()
        config.write_text(before + "CONFIG_LOG_DEFAULT_LEVEL=4\n")
        rejects(lambda: artifacts.verify(board), "configuration sdkconfig_sha256")
        config.write_text('CONFIG_SOMNOTRACE_BOARD_QEMU=y\nCONFIG_IDF_TARGET="esp32s3"\n' +
                          ("CONFIG_SOMNOTRACE_QEMU_DISPLAY_154=y\n" if board == "7b" else ""))
        rejects(lambda: artifacts.record(artifacts.source_state(board), board), "configuration board")
        config.write_text(before.replace("CONFIG_SOMNOTRACE_BOARD_QEMU=y", "CONFIG_SOMNOTRACE_BOARD_QEMU=n"))
        rejects(lambda: artifacts.record(artifacts.source_state(board), board), "CONFIG_SOMNOTRACE_BOARD_QEMU")
        config.write_text(before.replace('"esp32s3"', '"esp32"'))
        rejects(lambda: artifacts.record(artifacts.source_state(board), board), "target must")
        config.write_text(before)
        description = build / "project_description.json"
        before = description.read_text()
        for key in ("target", "build_dir", "config_file", "config_defaults", "project_path"):
            data = json.loads(before)
            data[key] = "foreign"
            description.write_text(json.dumps(data))
            rejects(lambda: artifacts.record(artifacts.source_state(board), board), key)
        description.write_text(before)
        assert manifest.read_text() == original, "rejected record overwrote a good receipt"
        artifacts.verify(board)

    for path in (root / "CMakeLists.txt", scripts / "idf.sh", scripts / "idf-command.sh",
                 scripts / "idf-sdk-patches.py", patch):
        before = path.read_bytes()
        expected = artifacts.source_state("154")
        path.write_bytes(before + b"changed source")
        for board in ("7b", "154"):
            rejects(lambda: artifacts.verify(board), "source_sha256")
        rejects(lambda: artifacts.record(expected, "154"), "Sources changed during")
        path.write_bytes(before)

    for flag, phrase in (("QEMU_TEST_BAD_FRAME", "framebuffer dimensions"),
                         ("QEMU_TEST_BAD_ELF", "guest ELF identity"),
                         ("QEMU_TEST_FATAL", "firmware failure")):
        output = command(str(scripts / "test-qemu-ui.sh"), "--board", "154",
                         env={**env, flag: "1"}, success=False)
        assert phrase in output, output
    failures = list((root / "build-qemu-154").glob("boot-smoke-*/failure.txt"))
    assert len(failures) == 3
    # Invalid options fail before any builder, emulator setup, or launch side effect.
    calls_path.write_text("")
    for script in ("build-qemu.sh", "run-qemu-ui.sh", "test-qemu-ui.sh"):
        assert "Usage:" in command(str(scripts / script), "--help", env=env)
        for args in (("--board", "bogus"), ("--board",), ("--unknown",)):
            command(str(scripts / script), *args, env=env, success=False)
    assert calls_path.read_text() == ""
    print("QEMU variants: isolated builds/launches, measured smoke fixtures, stale/foreign rejection passed")
