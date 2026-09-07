#!/usr/bin/env python3
"""Execute production render/touch transitions with deterministic LVGL inputs."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/touch_manage_config_ui.c").read_text()


def function(name):
    match = re.search(rf"static [^;\n]+\b{name}\([^;]*?\)\s*\{{", source, re.S)
    assert match, name
    start = match.start()
    brace = source.index("{", match.start())
    depth = 0
    for offset in range(brace, len(source)):
        depth += (source[offset] == "{") - (source[offset] == "}")
        if depth == 0:
            return source[start:offset + 1]
    raise AssertionError(name)


preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#define LV_STATE_PRESSED 1
#define LV_EVENT_PRESSED 1
#define LV_EVENT_RELEASED 2
#define MC_WIFI_MOVE 1
#define V_WIFI 1
#define V_WIFI_EDIT 2
#define V_EDITOR 3
#define V_CONFIRM 4
#define V_TEST 5
#define V_SCAN 6
typedef struct object { bool pressed; unsigned count; struct object *children[2]; } lv_obj_t;
typedef struct { int x, y; } lv_point_t;
typedef struct { int unused; } lv_timer_t;
typedef struct { int code; intptr_t slot; } lv_event_t;
typedef struct { lv_point_t point; } lv_indev_t;
typedef struct {
    bool busy;
    struct { unsigned state, completed_mask, failed_mask; } test;
    struct { unsigned state; } scan;
} snapshot_t;
typedef struct {
    lv_obj_t *root;
    snapshot_t live;
    bool dirty, was_busy;
    int view, slot, drag_slot, drag_y;
    unsigned result_until, drawn_test_state, drawn_test_mask, drawn_scan_state;
} ui_t;
static ui_t *s_ui;
static lv_indev_t input;
static snapshot_t published;
static bool snapshot_available = true;
static unsigned renders, moves;
static int move_target;
static bool lv_obj_has_state(lv_obj_t *o, int state) { (void)state; return o->pressed; }
static unsigned lv_obj_get_child_cnt(lv_obj_t *o) { return o->count; }
static lv_obj_t *lv_obj_get_child(lv_obj_t *o, unsigned i) { return o->children[i]; }
static lv_indev_t *lv_indev_get_act(void) { return &input; }
static void lv_indev_get_point(lv_indev_t *i, lv_point_t *p) { *p = i->point; }
static void *lv_event_get_user_data(lv_event_t *e) { return (void *)e->slot; }
static int lv_event_get_code(lv_event_t *e) { return e->code; }
static bool touch_manage_config_snapshot(snapshot_t *out) {
    if (!snapshot_available) return false;
    *out = published; return true;
}
static unsigned lv_tick_get(void) { return 200; }
static void refresh_dynamic(void) {} /* label refresh cannot retire native targets */
static void render(void) { ++renders; s_ui->dirty = false; }
static void request(int command, const char *value, int index) {
    assert(command == MC_WIFI_MOVE); (void)value;
    ++moves; move_target = index; s_ui->dirty = true;
}
'''
tests = r'''
int main(void) {
    lv_obj_t row = {0}, other = {0};
    lv_obj_t nested = {.count = 1, .children = {&other}};
    lv_obj_t root = {.count = 2, .children = {&row, &nested}};
    ui_t ui = {.root = &root, .view = V_WIFI, .was_busy = true};
    s_ui = &ui;
    lv_event_t press = {.code = LV_EVENT_PRESSED, .slot = 0};
    lv_event_t release = {.code = LV_EVENT_RELEASED, .slot = 0};
    input.point.y = 175;
    row.pressed = true;
    drag(&press);
    /* Refresh completion occurs while the actual row awaits RELEASED. */
    published.busy = false;
    tick(NULL);
    assert(ui.dirty && !ui.was_busy && renders == 0 && ui.view == V_WIFI);
    tick(NULL);
    assert(ui.dirty && renders == 0);
    row.pressed = false;
    drag(&release);
    assert(ui.slot == 0 && ui.view == V_WIFI_EDIT && moves == 0);
    tick(NULL);
    assert(renders == 1 && !ui.dirty && ui.view == V_WIFI_EDIT);
    /* Release keeps drag semantics too, despite pending refresh work. */
    ui.view = V_WIFI; ui.was_busy = true; input.point.y = 175;
    row.pressed = true; drag(&press); tick(NULL);
    input.point.y = 233; row.pressed = false; drag(&release);
    assert(moves == 1 && move_target == 1 && ui.view == V_WIFI);
    tick(NULL); assert(renders == 2 && !ui.dirty);
    /* Nested editor/key targets also retain deferred navigation. */
    ui.view = V_EDITOR; ui.dirty = true; other.pressed = true;
    snapshot_available = false;
    tick(NULL); assert(renders == 2 && ui.dirty);
    other.pressed = false;
    tick(NULL); assert(renders == 3 && !ui.dirty);
    puts("PASS: refresh completion preserves held row, release tap, drag and nested targets");
}
'''
with tempfile.TemporaryDirectory(prefix="somno-config-touch-") as tmp:
    path = Path(tmp) / "test.c"
    path.write_text(preamble + function("subtree_pressed") + function("tick") + function("drag") + tests)
    binary = Path(tmp) / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
