#!/usr/bin/env python3
"""Contracts for reclaiming PSRAM-backed task stacks and internal TCBs."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"
HELPER_C = (MAIN / "psram_task.c").read_text(encoding="utf-8")
HELPER_H = (MAIN / "psram_task.h").read_text(encoding="utf-8")
HOST_TEST = (ROOT / "scripts/test-host.sh").read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"^[\w][\w\s*]*\b{name}\s*\([^;{{}}]*\)\s*\{{",
        source,
        re.MULTILINE,
    )
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


# The ESP-IDF WithCaps API owns both buffers and has the matching deletion API.
# Plain xTaskCreateStaticPinnedToCore leaves caller-provided buffers allocated
# forever after vTaskDelete(), which was the original leak. ESP-IDF's NULL
# deletion path creates a temporary internal-stack task and aborts on OOM, so a
# retained static reaper must always invoke the non-self deletion path.
create = function_body(HELPER_C, "psram_task_create")
delete = function_body(HELPER_C, "psram_task_delete")
init = function_body(HELPER_C, "psram_task_init")
reaper = function_body(HELPER_C, "psram_task_reaper")
assert "xTaskCreatePinnedToCoreWithCaps" in create
assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in create
assert "heap_caps_malloc(stack_size" not in create
assert "vTaskDeleteWithCaps(task)" in delete
assert "vTaskDeleteWithCaps(victim)" in reaper
assert "xTaskCreateStaticPinnedToCore" in init
assert "xQueueCreateStatic" in init
assert "vTaskDeleteWithCaps(NULL)" not in HELPER_C
assert "xQueueSend(s_reaper_queue, &current, portMAX_DELAY)" in delete
assert "vTaskSuspend(NULL)" in delete
assert "psram_task_init" in (MAIN / "main.c").read_text(encoding="utf-8")
assert "psram_task_delete(TaskHandle_t task)" in HELPER_H

# Audit every concrete helper call. A WithCaps task must not use plain
# vTaskDelete(), because that unlinks the task without reclaiming its PSRAM
# stack or internal TCB. The 7B storage worker is a deliberate dual-mode
# exception: persistent WithCaps on hardware, ordinary xTaskCreate/vTaskDelete
# under QEMU.
created = set()
sources = {}
for path in sorted(MAIN.glob("*.c")):
    source = path.read_text(encoding="utf-8")
    sources[path.name] = source
    for match in re.finditer(
        r"\bpsram_task_create\s*\(\s*([A-Za-z_]\w*)\s*,", source
    ):
        created.add((path.name, match.group(1)))

assert created, "no psram_task_create callers found"
raw_delete_exception = {("bsp_display_7b.c", "storage_status_task")}
for filename, task_name in sorted(created):
    body = function_body(sources[filename], task_name)
    if (filename, task_name) not in raw_delete_exception:
        assert "vTaskDelete(" not in body, (
            f"{filename}:{task_name} deletes a WithCaps task without reclaiming it"
        )

# These tasks have finite/error exits today and therefore must exercise the
# reclaiming path. Persistent workers intentionally have no deletion call.
self_deleting = {
    ("as11_ble.c", "pair_task"),
    ("as11_ble.c", "confirm_task"),
    ("as11_ble.c", "reconnect_task"),
    ("as11_ble.c", "auto_reconnect_task"),
    ("bsp_power.c", "battery_monitor_task"),
    ("log_stream.c", "ws_forwarder_task"),
    ("net_provision.c", "wifi_scan_task"),
    ("net_provision.c", "rebuild_day_task"),
    ("net_provision.c", "format_sd_task"),
    ("net_provision.c", "netprov_dns_task"),
    ("oximeter.c", "auto_pair_task"),
    ("oximeter_legacy.c", "pair_task"),
    ("oximeter_legacy.c", "pull_task"),
    ("oximeter_oxyii.c", "pair_task"),
    ("oximeter_oxyii.c", "canonical_migration_task"),
    ("oximeter_oxyii.c", "pull_task"),
}
assert self_deleting <= created, "self-deleting task missing from helper audit"
for filename, task_name in sorted(self_deleting):
    body = function_body(sources[filename], task_name)
    if (filename, task_name) == ("log_stream.c", "ws_forwarder_task"):
        exit_helper = function_body(
            sources[filename], "ws_forwarder_exit"
        )
        assert "ws_forwarder_exit()" in body
        assert "ws_forwarder_clear_current()" in exit_helper
        assert "psram_task_delete(NULL)" in exit_helper
    else:
        assert "psram_task_delete(NULL)" in body, (
            f"{filename}:{task_name} has no reclaiming self-delete path"
        )

# OTA URL uses ordinary xTaskCreate and therefore self-deletes normally.  The
# streamed flash worker is deliberately parent-joined: after publishing its
# result it parks, and the owning HTTP task deletes it before freeing the
# shared event group and buffers.
ota_url = function_body(sources["net_provision.c"], "ota_url_task")
assert "vTaskDelete(NULL)" in ota_url
assert "psram_task_delete" not in ota_url

assert "psram_task_lifecycle_contract_test.py" in HOST_TEST

print(f"PSRAM task lifecycle contract passed ({len(created)} callers audited)")
