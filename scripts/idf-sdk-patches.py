#!/usr/bin/env python3
"""Apply the checked SDK async-request unwind fix inside the build container."""
from pathlib import Path
import os

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "third_party/esp-idf-patches/httpd_async_v5_5_1.c"
OLD = """    if (async_aux->resp_hdrs == NULL) {
        free(async_aux);"""
NEW = """    if (async_aux->resp_hdrs == NULL) {
        free(async_aux->scratch);
        free(async_aux);"""


def functions(source):
    start = source.index("esp_err_t httpd_req_async_handler_begin(")
    complete = source.index("esp_err_t httpd_req_async_handler_complete(", start)
    end = source.index("\n}", complete) + 2
    return source[start:end]


def apply(path):
    source = path.read_text()
    expected = functions(REFERENCE.read_text())
    fixed = expected.replace(OLD, NEW)
    assert fixed != expected and expected.count(OLD) == 1
    actual = functions(source)
    if actual == fixed:
        return False
    if actual != expected:
        raise RuntimeError("ESP-IDF HTTP async implementation changed; review the SDK patch and fault test")
    path.write_text(source.replace(actual, fixed, 1))
    return True


if __name__ == "__main__":
    target = Path(os.environ["IDF_PATH"]) / "components/esp_http_server/src/httpd_txrx.c"
    if apply(target):
        print("Applied checked ESP-IDF HTTP async allocation cleanup")
