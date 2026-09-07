#!/usr/bin/env python3
"""Run production C validators and journal with deterministic host storage."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="somno-config-test-") as tmp:
    binary=str(Path(tmp)/"config-test")
    subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-Dtime=config_test_time",
        "-I"+str(root/"scripts/config_test_stubs"),"-I"+str(root/"main"),
        "-I"+str(root/"components/therapy_alert"),
        str(root/"main/net_config_model.c"),str(root/"components/therapy_alert/alert_config_model.c"),
        str(root/"components/therapy_alert/alert_history.c"),str(root/"scripts/manage_config_behavior_test.c"),"-o",binary],check=True)
    subprocess.run([binary],check=True)

with tempfile.TemporaryDirectory(prefix="somno-probe-test-") as tmp:
    binary=str(Path(tmp)/"probe-test")
    subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror",
        "-I"+str(root/"scripts/config_test_stubs"),"-I"+str(root/"components/uploader"),
        str(root/"components/uploader/upload_probe_smb.c"),
        str(root/"scripts/upload_probe_behavior_test.c"),"-o",binary],check=True)
    subprocess.run([binary],check=True)
