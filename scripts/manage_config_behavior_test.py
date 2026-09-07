#!/usr/bin/env python3
"""Run Manage's C tests through the canonical shim-based host runner."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
runner = root / "scripts/run_host_tests.sh"
for test in ("manage_config_behavior_test", "upload_probe_behavior_test"):
    subprocess.run(["bash", str(runner), "--only", test, "--quiet"], cwd=root, check=True)
