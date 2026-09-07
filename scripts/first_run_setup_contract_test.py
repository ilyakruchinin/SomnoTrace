#!/usr/bin/env python3
"""Static integration contracts for first-run setup persistence."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "main/first_run_setup.h").read_text(encoding="utf-8")
MODEL = (ROOT / "main/first_run_setup_model.c").read_text(encoding="utf-8")
SERVICE = (ROOT / "main/first_run_setup.c").read_text(encoding="utf-8")
MAIN = (ROOT / "main/main.c").read_text(encoding="utf-8")
QEMU = (ROOT / "main/main_qemu.c").read_text(encoding="utf-8")
CMAKE = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
HOST = (ROOT / "scripts/test-host.sh").read_text(encoding="utf-8")


def require(text: str, pattern: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(f"missing first-run setup contract: {description}")


for name in ("WIFI", "TIME", "AIRSENSE", "CARD", "ALERTS", "UPLOADS"):
    require(HEADER, rf"FIRST_RUN_SETUP_STEP_{name}", f"named {name} step")

require(HEADER, r"FIRST_RUN_SETUP_SCHEMA_VERSION\s+1U", "schema version")
require(HEADER, r"completed_mask", "completed-step mask")
require(HEADER, r"skipped_mask", "skipped-step mask")
require(HEADER, r"continue_without_recording", "explicit no-recording state")
require(HEADER, r"first_run_setup_snapshot\s*\(", "snapshot API")
require(HEADER, r"first_run_setup_update\s*\(", "transition API")
require(HEADER, r"first_run_setup_reset\s*\(", "reset API")
require(HEADER, r"first_run_setup_reconcile\s*\(", "reconciliation API")
assert "skip_all" not in HEADER.lower() and "skip_setup" not in HEADER.lower(), (
    "a generic whole-setup skip API must not be introduced"
)

require(
    MODEL,
    r"FIRST_RUN_SETUP_UPDATE_SKIP:.*?first_run_setup_step_can_skip\(step\)",
    "skip is validated per named step",
)
require(
    MODEL,
    r"case FIRST_RUN_SETUP_STEP_CARD:.*?return false;",
    "Card cannot be marked skipped",
)
require(
    MODEL,
    r"!had_persisted_state\s*&&\s*observed->established_installation",
    "legacy auto-resolution only applies without persisted progress",
)
require(
    MODEL,
    r"step == FIRST_RUN_SETUP_STEP_CARD.*?continue_without_recording = true",
    "legacy missing Card uses explicit no-recording resolution",
)

require(SERVICE, r'#define NVS_KEY_SCHEMA\s+"schema"', "stable schema key")
require(SERVICE, r'#define NVS_KEY_STATE\s+"state"', "stable state key")
require(
    SERVICE,
    r"nvs_set_u16\s*\(.*?FIRST_RUN_SETUP_SCHEMA_VERSION.*?"
    r"nvs_set_blob\s*\(.*?nvs_commit",
    "schema and state are committed together",
)
require(
    SERVICE,
    r"do_save_nvs.*?const first_run_setup_state_t state\s*=.*?nvs_open",
    "writer callback copies input before opening NVS",
)
require(
    SERVICE,
    r"nvs_close\(h\).*?\*\(first_run_setup_state_t \*\)arg = decoded",
    "reader callback copies output only after closing NVS",
)
require(
    SERVICE,
    r"first_run_setup_load\(void\).*?nvs_writer_init\(\).*?"
    r"nvs_writer_run\(do_load_nvs",
    "load initializes the proxy before any possible PSRAM-stack call",
)
require(
    SERVICE,
    r"nvs_writer_run\(do_load_nvs,\s*&s_nvs_work\)",
    "loads run through the internal-stack NVS worker",
)
require(
    SERVICE,
    r"nvs_writer_run\(do_save_nvs,\s*&s_nvs_work\)",
    "writes run through the internal-stack NVS worker",
)
require(
    SERVICE,
    r"nvs_writer_run\(do_reset_nvs,\s*NULL\)",
    "reset runs through the internal-stack NVS worker",
)

# Physical boot integration.  Load the versioned record before collecting
# setup-facing facts, reconcile only after storage and the pairing cache are
# known, and publish that coherent state before touch services become live.
require(MAIN, r'#include "first_run_setup\.h"', "boot includes setup service")
assert MAIN.index("nvs_writer_init();") < MAIN.index("first_run_setup_load();"), (
    "boot must initialise the NVS writer before loading first-run state"
)
assert MAIN.index("first_run_setup_load();") < MAIN.index(
    "netprov_load_config(&cfg)"
), "setup record must be loaded before boot-time fact probes"
assert MAIN.index("sd_storage_init();") < MAIN.index(
    "reconcile_first_run_setup(&setup_facts);"
), "card readiness must be known before setup reconciliation"
assert MAIN.index("as11_ble_init()") < MAIN.index(
    "reconcile_first_run_setup(&setup_facts);"
), "the durable AirSense pair cache must be loaded before reconciliation"
assert MAIN.index("reconcile_first_run_setup(&setup_facts);") < MAIN.index(
    "bsp_display_enable_touch_services(as11_ready, oximeter_ready);"
), "touch services must not become interactive before setup is coherent"

require(
    MAIN,
    r"\.established_installation\s*=\s*facts->setup_state_missing\s*&&\s*"
    r"boot_has_legacy_setup_evidence\(facts\)",
    "legacy migration only applies when the setup record was absent",
)
legacy_fn = re.search(
    r"static bool boot_has_legacy_setup_evidence\([^)]*\)\s*\{(.*?)\n\}",
    MAIN,
    re.MULTILINE | re.DOTALL,
)
assert legacy_fn, "legacy-evidence helper is missing"
assert "card_ready" not in legacy_fn.group(1), (
    "an inserted card alone must never classify a fresh device as established"
)
for fact, observed in (
    ("wifi_configured", "wifi_configured"),
    ("timezone_saved", "time_configured"),
    ("airsense_paired", "airsense_paired"),
    ("card_ready", "card_present"),
    ("alert_config_saved", "alerts_configured"),
    ("upload_destination_configured", "uploads_configured"),
):
    require(
        MAIN,
        rf"\.{observed}\s*=\s*facts->{fact}",
        f"truthful {observed} boot fact",
    )
require(
    MAIN,
    r"if\s*\(setup_reconcile_needed\(&snapshot,\s*&observed\)\)\s*\{.*?"
    r"first_run_setup_reconcile\(&observed\)",
    "unchanged setup state is not rewritten on every boot",
)
require(
    MAIN,
    r"uploader_has_destination.*?smb_enabled.*?smb_host.*?smb_share.*?"
    r"shq_enabled.*?shq_client_id.*?shq_client_secret",
    "Uploads only resolves for a usable SMB or SleepHQ destination",
)

# QEMU is a deterministic normal-shell preview.  It repairs only its
# disposable first-run namespace, resolves every simulated fact, and does so
# before LVGL builds the shell.
require(QEMU, r"seed_finished_setup_preview\s*\(", "QEMU setup seed")
assert QEMU.index("seed_finished_setup_preview();") < QEMU.index(
    "bsp_display_init()"
), "QEMU setup state must be final before the shell is constructed"
require(
    QEMU,
    r"first_run_setup_load\(\).*?first_run_setup_reset\(\).*?"
    r"first_run_setup_reconcile\(&observed\)",
    "QEMU repairs stale disposable state and reconciles its fixture",
)
for fact in (
    "wifi_configured",
    "time_configured",
    "airsense_paired",
    "card_present",
    "alerts_configured",
    "uploads_configured",
):
    require(QEMU, rf"\.{fact}\s*=\s*true", f"QEMU finished {fact} fixture")
require(
    QEMU,
    r"first_run_setup_is_finished\(&snapshot\.state\)",
    "QEMU verifies the finished fixture",
)

for source in ("first_run_setup.c", "first_run_setup_model.c"):
    assert f'"{source}"' in CMAKE, f"{source} is not registered in CMake"
require(HOST, r"first_run_setup_test\.c.*?first_run_setup_model\.c", "host unit test")
require(HOST, r"python3 scripts/first_run_setup_contract_test\.py", "contract test")

print("first-run setup persistence contract passed")
