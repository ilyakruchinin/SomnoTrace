#!/usr/bin/env python3
"""Structural and model contract for the bounded Manage detail lifecycle."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main" / "bsp_display_7b.c").read_text()


def function(name: str) -> str:
    match = re.search(rf"static [^;\n]+\b{name}\([^;]*?\)\s*\{{", SOURCE, re.S)
    assert match, f"missing {name}"
    start = match.start()
    brace = SOURCE.index("{", match.start())
    depth = 0
    for offset in range(brace, len(SOURCE)):
        if SOURCE[offset] == "{":
            depth += 1
        elif SOURCE[offset] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[start : offset + 1]
    raise AssertionError(f"unterminated {name}")


build_page = function("build_manage_page")
assert "s_manage_detail_host = make_card" in build_page
for eager in (
    "build_devices_section(",
    "build_connectivity_section(",
    "build_alerts_section(",
    "build_uploads_section(",
    "build_storage_section(",
    "build_system_section(",
    "build_advanced_section(",
):
    assert eager not in build_page, f"boot still eagerly invokes {eager}"

selection = function("set_manage_section")
same_tap = selection.index("section == s_active_manage_section")
teardown = selection.index("teardown_rendered_manage_destination()")
assert same_tap < teardown, "same-section tap must return before teardown"

release = function("teardown_rendered_manage_destination")
for close in ("close_keyboard_sheet(true)", "close_manage_dialog()"):
    assert release.index(close) < release.index("clear_manage_section_pointers(section)")
assert release.count("clear_manage_section_pointers(section)") >= 2
assert release.index("clear_manage_section_pointers(section)") < release.index("lv_obj_del(root)")
assert "s_manage_retired_logs_section = root" in release
assert release.index("touch_maintenance_ui_destroy()") < release.index(
    "clear_manage_section_pointers(section)"
)

refresh = function("refresh_secondary_pages")
assert "section != s_active_manage_section" in refresh
assert "!s_manage_sections[section]" in refresh
assert "active_scroll && lv_obj_is_scrolling(active_scroll)" in refresh
for destination in (
    "MANAGE_DEVICES",
    "MANAGE_CONNECTIVITY",
    "MANAGE_ALERTS",
    "MANAGE_UPLOADS",
    "MANAGE_STORAGE",
    "MANAGE_SYSTEM",
    "MANAGE_LOGS",
):
    assert f"section == {destination}" in refresh

clear = function("clear_manage_section_pointers")
for pointer in (
    "s_as11_row = NULL",
    "s_wifi_ssid = NULL",
    "s_alert_status = NULL",
    "memset(s_upload_rows",
    "s_storage_status = NULL",
    "s_reboot_button = NULL",
):
    assert pointer in clear, f"destination pointer not invalidated: {pointer}"

assert "roots=%u objects=%u internal=%u psram=%u" in SOURCE


class DetailOwnership:
    """Small deterministic model of the C ownership state machine."""

    LOGS = 6

    def __init__(self):
        self.active = None
        self.retired_logs = False
        self.generation = 0
        self.max_roots = 0
        self.builds = 0
        self.destroys = 0

    def roots(self):
        return int(self.active is not None) + int(self.retired_logs)

    def validate(self):
        assert self.roots() <= 2
        assert not (self.active == self.LOGS and self.retired_logs)
        self.max_roots = max(self.max_roots, self.roots())

    def select(self, section, logs_worker_busy):
        if section == self.active:
            before = self.generation
            self.validate()
            assert self.generation == before
            return
        if self.active is not None:
            old = self.active
            self.active = None
            self.destroys += 1
            self.generation += 1
            if old == self.LOGS and logs_worker_busy:
                self.retired_logs = True
        if section == self.LOGS and self.retired_logs:
            self.retired_logs = False
        self.active = section
        self.builds += 1
        self.generation += 1
        self.validate()

    def reap(self):
        if self.retired_logs:
            self.retired_logs = False
            self.generation += 1
        self.validate()


model = DetailOwnership()
sequence = [index % 8 for index in range(64)]  # eight full round trips
for transition, destination in enumerate(sequence):
    model.select(destination, logs_worker_busy=(transition % 3 != 0))
    if transition % 5 == 0:
        model.reap()
    # Exercise the no-op path after every real transition.
    generation = model.generation
    model.select(destination, logs_worker_busy=True)
    assert model.generation == generation

model.reap()
assert len(sequence) >= 50
assert model.max_roots <= 2
assert model.builds == len(sequence)
assert model.destroys == len(sequence) - 1

print(
    "Manage detail lifecycle contract passed: "
    f"{len(sequence)} transitions, max {model.max_roots} detail roots"
)
