#!/usr/bin/env python3
"""Exercise Docker path mapping and reject stale/cross-checkout QEMU output."""

import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
from qemu_runtime import qmp_socket
from qemu_variant_test import exercise_variants

ROOT = Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    return subprocess.check_output(args, stderr=subprocess.STDOUT, **kwargs)



with tempfile.TemporaryDirectory(prefix="qemu-worktree-") as temp:
    base = Path(temp).resolve()
    primary = base / "main checkout"
    child = base / ("long child [rev c] " * 8)
    primary.mkdir()
    run("git", "init", "-q", str(primary))
    run("git", "-C", str(primary), "config", "user.name", "Test")
    run("git", "-C", str(primary), "config", "user.email", "test@example.invalid")
    (primary / "scripts").mkdir()
    shutil.copy(ROOT / "scripts/idf.sh", primary / "scripts/idf.sh")
    (primary / ".gitignore").write_text("build-*/\nsdkconfig*\n!sdkconfig*.defaults\n__pycache__/\n")
    (primary / "CMakeLists.txt").write_text("primary source\n")
    run("git", "-C", str(primary), "add", ".")
    run("git", "-C", str(primary), "commit", "-qm", "fixture")
    # Disposable test repos are independent of Orca workspace ownership.
    run("git", "-C", str(primary), "worktree", "add", "-qb", "child", str(child))
    fake_bin = base / "bin"
    fake_bin.mkdir()
    fake = fake_bin / "docker"
    fake.write_text("#!/usr/bin/env python3\nimport json,sys\nprint(json.dumps(sys.argv[1:]))\n")
    fake.chmod(0o755)
    env = {**os.environ, "PATH": f"{fake_bin}:{os.environ['PATH']}",
           "IDF_PORT": str(base / "no-serial")}
    argv = json.loads(run(str(child / "scripts/idf.sh"), "exec", "git", "describe", env=env))
    assert f"{child}:/project" in argv
    assert f"{primary}/.git:{primary}/.git:ro" in argv
    assert f"{primary}:/project" not in argv
    assert argv[-2:] == ["git", "describe"]
    assert "GIT_CONFIG_VALUE_0=/project" in argv
    main_argv = json.loads(run(str(primary / "scripts/idf.sh"), "build", env=env))
    assert f"{primary}:/project" in main_argv
    assert main_argv[-2:] == ["idf.py", "build"]

    build = child / "build-qemu"
    build.mkdir()
    assert len(str(build)) > 104
    with qmp_socket(build) as endpoint:
        assert len(str(endpoint).encode()) < 100
        with socket.socket(socket.AF_UNIX) as server:
            server.bind(str(endpoint))
            server.listen(1)
            with socket.socket(socket.AF_UNIX) as client:
                client.connect(str(endpoint))
                accepted, _ = server.accept()
                accepted.close()
        assert endpoint.exists()
    assert not endpoint.exists(), "private QMP socket was not cleaned up"
    exercise_variants(ROOT, child, base, env)

print("QEMU worktree path/provenance and isolated variant tests passed")
