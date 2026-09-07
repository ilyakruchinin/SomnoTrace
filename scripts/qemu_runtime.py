"""Private short QMP endpoints, independent of linked checkout path length."""
from contextlib import contextmanager
from pathlib import Path
import tempfile


@contextmanager
def qmp_socket(runtime_dir):
    # macOS sockaddr_un is bounded to 104 bytes. Keep only this control socket
    # in a short private directory; disks, UART and captures stay in the tree.
    with tempfile.TemporaryDirectory(prefix="stq-", dir="/tmp") as short:
        endpoint = Path(short) / "qmp"
        (Path(runtime_dir) / "qmp-location.txt").write_text(str(endpoint) + "\n")
        yield endpoint
