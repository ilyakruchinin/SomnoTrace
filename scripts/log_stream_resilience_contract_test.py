#!/usr/bin/env python3
"""Structural and state-model contracts for log-stream failure recovery."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/log_stream.c").read_text(encoding="utf-8")
HEADER = (ROOT / "main/log_stream.h").read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if not match:
        raise AssertionError(f"missing function: {name}")
    depth = 1
    cursor = match.end()
    while cursor < len(source) and depth:
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
        cursor += 1
    if depth:
        raise AssertionError(f"unterminated function: {name}")
    return source[match.end(): cursor - 1]


flush = function_body(SOURCE, "log_flush_once")
acknowledge = function_body(SOURCE, "writebuf_acknowledge")
archive = function_body(SOURCE, "stream_persistent_log_files")
history = function_body(SOURCE, "logs_history_handler")
download = function_body(SOURCE, "logs_download_handler")
initialise = function_body(SOURCE, "log_stream_init")
recent = function_body(SOURCE, "logs_recent_handler")
ws_handler = function_body(SOURCE, "logs_ws_handler")
ws_json = function_body(SOURCE, "log_stream_ws_send_json")
ws_raw = function_body(SOURCE, "log_stream_ws_send_json_raw")
ws_task = function_body(SOURCE, "ws_forwarder_task")
ws_exit = function_body(SOURCE, "ws_forwarder_exit")
ws_start = function_body(SOURCE, "ws_forwarder_start")
publish = function_body(SOURCE, "retained_publish_snapshot")
save = function_body(SOURCE, "log_stream_retained_save_to_sd")

# Flush owns the export lease across every file operation and acknowledges
# only the prefix accepted by fwrite. A partial write leaves its suffix queued.
lease_at = flush.find("sd_storage_lease_acquire(SD_LEASE_EXPORT")
open_at = flush.find("fopen(path, \"ab\")")
write_at = flush.find("fwrite(temporary, 1, chunk, file)")
ack_at = flush.find("writebuf_acknowledge(written)")
release_at = flush.rfind("sd_storage_lease_release_unchanged(SD_LEASE_EXPORT)")
assert -1 not in (lease_at, open_at, write_at, ack_at, release_at)
assert lease_at < open_at < write_at < ack_at < release_at
assert "s_writebuf_tail += chunk" not in flush
assert "s_writebuf_tail += written" in acknowledge
assert "if (written > pending) written = pending" in acknowledge

# Browser history and download share one leased archive reader. The lease
# spans open/read/close and the pending RAM boundary snapshot.
archive_lease = archive.find("sd_storage_lease_acquire(SD_LEASE_EXPORT")
archive_open = archive.find('fopen(path, "rb")')
archive_read = archive.find("fread(chunk")
archive_snapshot = archive.find("snapshot_pending_log_bytes")
archive_release = archive.rfind("sd_storage_lease_release_unchanged(SD_LEASE_EXPORT)")
assert -1 not in (
    archive_lease, archive_open, archive_read, archive_snapshot, archive_release
)
assert archive_lease < archive_open < archive_read < archive_snapshot < archive_release
assert "stream_complete_log_archive" in history
assert "stream_complete_log_archive" in download

# Initialisation reports failure, remains idempotent, and every consumer which
# can touch the byte ring or WS mutex has an explicit degraded-state guard.
assert "esp_err_t log_stream_init(void)" in HEADER
assert "if (s_init_attempted) return s_init_result" in initialise
assert "return s_init_result" in initialise
assert "if (!s_ringbuf)" in recent
assert "if (!s_ringbuf || !s_ws_mutex)" in ws_handler
assert "if (!s_ringbuf || !s_ws_mutex)" in ws_json
assert "if (!s_ringbuf || !s_ws_mutex)" in ws_raw

# Publication never deletes the last good export first. A failed final rename
# rolls the backup back, and the temporary source remains caller-cleanable.
assert "remove(final_path)" not in save
assert "rename(final_path, backup_path)" in publish
assert "rename(temporary_path, final_path)" in publish
assert "rename(backup_path, final_path)" in publish
assert "retained_publish_snapshot(tmp_path, final_path, backup_path)" in save

# A self-terminating forwarder clears its published task handle before using
# the WithCaps deletion helper; startup is serialised so reconnect can retry.
assert "ws_forwarder_clear_current()" in ws_exit
assert ws_exit.find("ws_forwarder_clear_current()") < ws_exit.find(
    "psram_task_delete(NULL)"
)
assert "s_ws_fwd_starting = true" in ws_start
assert "s_ws_fwd_starting = false" in ws_start
assert "if (task) s_ws_fwd_task = task" in ws_start
assert ws_task.count("ws_forwarder_exit()") >= 2


# State model: partial writes advance by the exact accepted prefix only, so a
# later successful flush produces the original byte sequence without loss.
pending = bytearray(b"abcdefghij")
persisted = bytearray()
for accepted in (4, 0, 6):
    offered = bytes(pending)
    written = min(accepted, len(offered))
    persisted.extend(offered[:written])
    del pending[:written]
assert bytes(persisted) == b"abcdefghij"
assert pending == b""


# State model: publication failure restores the previous final; success swaps
# in the new snapshot and discards only the now-obsolete backup.
def publish_model(final: bytes | None, temporary: bytes, fail_publish: bool):
    backup = final
    final = None
    if fail_publish:
        final = backup
        return final, temporary, None
    final = temporary
    temporary = None
    backup = None
    return final, temporary, backup


final, temporary, backup = publish_model(b"last-good", b"partial", True)
assert final == b"last-good" and temporary == b"partial" and backup is None
final, temporary, backup = publish_model(b"last-good", b"complete", False)
assert final == b"complete" and temporary is None and backup is None

print("log stream resilience contracts passed")
