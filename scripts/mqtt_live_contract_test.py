#!/usr/bin/env python3
# SomnoTrace - Live MQTT statistics, discovery and scheduling contract tests
# Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
#
# This file is part of SomnoTrace.
#
# SomnoTrace is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
#
# SomnoTrace is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
# A PARTICULAR PURPOSE. See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with
# this program. If not, see <https://www.gnu.org/licenses/>.
#
# ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
# attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
# (https://github.com/ilyakruchinin)." See the NOTICE file for details.

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / os.environ.get("MAIN_DIR", "main")
display = (MAIN / "bsp_display.c").read_text()
mqtt = (MAIN / "mqtt_manager.c").read_text()
writer = (MAIN / "session_writer.c").read_text()


def function(source, name):
    match = re.search(r"^(?:static )?\w+ " + name + r"\([^;]+?\)\n\{", source, re.M)
    if not match:
        raise AssertionError(f"missing function: {name}")
    end = source.index("\n}", match.end()) + 2
    return source[match.start():end]


start = display.index("static float    s_leak_lpm")
end = display.index("/* ── Public state-mutating API", start)
code = r'''
#include <assert.h>
#include <math.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "bsp_display.h"
#include "leak_roll.h"
#define portMAX_DELAY 0
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define DISP_MODE_INFO 1
#define MQTT_STATE_CONNECTED 1
static int s_state_mutex = 1, s_mtx = 2;
static int s_state_locks, s_mqtt_locks;
static int s_display_task = 1, s_mode = 1;
static bool s_therapy_active, s_as11_paired, s_status_dirty;
static int64_t now_us = 1000000;
static int64_t esp_timer_get_time(void) { return now_us; }
static void xSemaphoreTake(int mutex, int delay) {
    (void)delay;
    if (mutex == s_state_mutex) s_state_locks++;
    else s_mqtt_locks++;
}
static void xSemaphoreGive(int mutex) {
    if (mutex == s_state_mutex) s_state_locks--;
    else s_mqtt_locks--;
}
static void xTaskNotifyGive(int task) { (void)task; }
bool bsp_display_is_therapy_active(void) { return s_therapy_active; }
static int s_client = 1, s_conn_state = 1;
static char s_live_topic[64] = "test/live", s_lwt_topic[64] = "test/status";
static char s_device_id[32] = "somnotrace_test";
static cJSON *last_live, *discovery[4];
static int disc_count;
static int64_t live_times[32], state_times[32];
static int live_count, state_count;
static int esp_mqtt_client_publish(int client, const char *topic, const char *data,
                                   int length, int qos, int retain) {
    (void)client; (void)length;
    assert(s_state_locks == 0);
    assert(s_mqtt_locks == 1);
    assert(strcmp(topic, s_live_topic) == 0);
    assert(qos == 0 && retain == 0);
    cJSON_Delete(last_live);
    last_live = cJSON_Parse(data);
    assert(last_live);
    if (live_count < 32) live_times[live_count++] = now_us / 1000;
    return 0;
}
static void publish_entity_discovery(const char *component, const char *key, cJSON *cfg) {
    (void)key;
    assert(strcmp(component, "sensor") == 0);
    assert(disc_count < 4);
    discovery[disc_count++] = cfg;
}
static void do_publish_state(void) { state_times[state_count++] = now_us / 1000; }
typedef int mqtt_cmd_t;
static int s_cmd_q;
static int xQueueReceive(int queue, mqtt_cmd_t *cmd, int wait) {
    (void)queue; (void)cmd; (void)wait; return 0;
}
static void execute_command(mqtt_cmd_t cmd) { (void)cmd; }
static int mqtt_manager_get_state(void) { return s_conn_state; }
static jmp_buf stop_worker;
static int wake_index;
static int64_t forced_wakes[] = { 5000, 7000, 15000 };
static uint32_t ulTaskNotifyTake(int clear, int64_t wait);
'''
code += display[start:end]
for name in ("bsp_display_push_leak", "bsp_display_push_pressure", "bsp_display_get_live_stats", "bsp_display_set_therapy_start_time", "bsp_display_set_as11_paired"):
    code += "\n" + function(display, name)
for name in ("TELEMETRY_INTERVAL_MS", "LIVE_INTERVAL_MS", "LIVE_EXPIRE_S"):
    code += "\n" + re.search(r"^#define " + name + r"[^\n]+", mqtt, re.M).group(0)
for name in ("do_publish_live", "telemetry_wait_ms", "mqtt_telemetry_task", "publish_live_sensor"):
    code += "\n" + function(mqtt, name)
code += r'''
static uint32_t ulTaskNotifyTake(int clear, int64_t wait) {
    (void)clear;
    assert(wait > 0);
    int64_t next = now_us / 1000 + wait;
    uint32_t notified = 0;
    if (wake_index < 3 && forced_wakes[wake_index] <= next) {
        next = forced_wakes[wake_index++];
        notified = 1;
    }
    if (next > 65000) longjmp(stop_worker, 1);
    now_us = next * 1000;
    bsp_display_push_leak(2.0f);
    bsp_display_push_pressure(8.0f);
    return notified;
}
static cJSON *field(cJSON *object, const char *key) {
    cJSON *item = cJSON_GetObjectItem(object, key);
    assert(item);
    return item;
}
static void text(cJSON *object, const char *key, const char *expected) {
    cJSON *item = field(object, key);
    assert(cJSON_IsString(item));
    assert(strcmp(item->valuestring, expected) == 0);
}
int main(void) {
    bsp_live_stats_t snapshot;
    bsp_display_get_live_stats(&snapshot);
    assert(!snapshot.active && !snapshot.leak_valid && !snapshot.pressure_valid);
    bsp_display_push_leak(10.0f);
    assert(s_leak_count == 0);
    s_therapy_active = s_as11_paired = true;
    bsp_display_set_therapy_start_time(now_us);
    bsp_display_push_leak(0.0f);
    bsp_display_push_leak(12.0f);
    bsp_display_push_pressure(8.4f);
    bsp_display_get_live_stats(&snapshot);
    assert(snapshot.active && snapshot.leak_valid && snapshot.pressure_valid);
    assert(snapshot.leak_lpm == 12.0f && snapshot.leak_avg_lpm == 6.0f);
    assert(fabsf(snapshot.pressure_cmh2o - 8.4f) < 0.001f);
    /* Both pushes landed in the same second: the rolling ring holds no
     * completed bucket yet, so the window is neither populated nor full. */
    assert(snapshot.leak_roll_lpm == 0.0f && !snapshot.leak_roll_ready);
    bsp_display_push_leak(NAN);
    bsp_display_push_leak(INFINITY);
    bsp_display_push_leak(-1.0f);
    bsp_display_push_pressure(NAN);
    bsp_display_push_pressure(-1.0f);
    assert(s_leak_count == 2);
    /* Cross a second boundary: the accumulated (0+12)/2 bucket flushes.
     * Pressure is refreshed too so both channels share one update time
     * for the staleness assertions below. */
    now_us += 1000000;
    bsp_display_push_leak(6.0f);
    bsp_display_push_pressure(8.4f);
    bsp_display_get_live_stats(&snapshot);
    assert(fabsf(snapshot.leak_roll_lpm - 6.0f) < 0.001f);
    assert(!snapshot.leak_roll_ready);   /* one bucket is not a window */
    now_us += BSP_LIVE_STALE_US - 1;
    bsp_display_get_live_stats(&snapshot);
    assert(snapshot.leak_valid && snapshot.pressure_valid);
    now_us++;
    bsp_display_get_live_stats(&snapshot);
    assert(!snapshot.leak_valid && !snapshot.pressure_valid);
    bsp_display_push_leak(6.0f);
    bsp_display_get_live_stats(&snapshot);
    assert(snapshot.leak_valid && !snapshot.pressure_valid);
    assert(snapshot.leak_avg_lpm == 6.0f);
    bsp_display_set_as11_paired(false);
    bsp_display_get_live_stats(&snapshot);
    assert(!snapshot.leak_valid && !snapshot.pressure_valid);
    bsp_display_set_as11_paired(true);
    bsp_display_get_live_stats(&snapshot);
    assert(!snapshot.leak_valid && !snapshot.pressure_valid);
    assert(s_leak_count == 4);
    s_therapy_active = false;
    bsp_display_push_leak(99.0f);
    assert(s_leak_count == 4);
    do_publish_live();
    assert(cJSON_IsNull(field(last_live, "leak_rate")));
    assert(cJSON_IsNull(field(last_live, "leak_avg")));
    assert(cJSON_IsNull(field(last_live, "leak_roll")));
    assert(cJSON_IsNull(field(last_live, "pressure")));
    assert(cJSON_IsFalse(field(last_live, "leak_valid")));
    s_therapy_active = true;
    bsp_display_set_therapy_start_time(now_us);
    bsp_display_get_live_stats(&snapshot);
    assert(!snapshot.leak_valid && !snapshot.pressure_valid);
    assert(s_leak_count == 0 && s_leak_lpm == 0.0f);
    bsp_display_push_leak(0.0f);
    bsp_display_push_pressure(0.0f);
    do_publish_live();
    assert(cJSON_IsTrue(field(last_live, "leak_valid")));
    assert(field(last_live, "leak_rate")->valuedouble == 0.0);
    assert(field(last_live, "pressure")->valuedouble == 0.0);
    live_count = 0;
    s_conn_state = 0;
    do_publish_live();
    assert(live_count == 0);
    s_conn_state = 1;
    publish_live_sensor("leak_rate", "Leak Rate", "L/min", "leak_valid", "mdi:air-filter");
    publish_live_sensor("leak_avg", "Session Average Leak", "L/min", "leak_valid", "mdi:air-filter");
    publish_live_sensor("leak_roll", "Leak Rate (2-min Avg)", "L/min", "leak_valid", "mdi:air-filter");
    publish_live_sensor("pressure", "Mask Pressure", "cmH2O", "pressure_valid", "mdi:gauge");
    assert(disc_count == 4);
    const char *keys[] = { "leak_rate", "leak_avg", "leak_roll", "pressure" };
    for (int i = 0; i < 4; i++) {
        cJSON *cfg = discovery[i];
        char expected[100];
        snprintf(expected, sizeof(expected), "somnotrace_test_%s", keys[i]);
        text(cfg, "unique_id", expected);
        text(cfg, "state_topic", "test/live");
        snprintf(expected, sizeof(expected), "{{ value_json.%s }}", keys[i]);
        text(cfg, "value_template", expected);
        text(cfg, "unit_of_measurement", i == 3 ? "cmH2O" : "L/min");
        text(cfg, "state_class", "measurement");
        text(cfg, "availability_mode", "all");
        assert(field(cfg, "expire_after")->valueint == 30);
        cJSON *availability = field(cfg, "availability");
        assert(cJSON_GetArraySize(availability) == 2);
        text(cJSON_GetArrayItem(availability, 0), "topic", "test/status");
        text(cJSON_GetArrayItem(availability, 1), "topic", "test/live");
        text(cJSON_GetArrayItem(availability, 1), "value_template", i == 3 ?
             "{{ 'online' if value_json.pressure_valid else 'offline' }}" :
             "{{ 'online' if value_json.leak_valid else 'offline' }}");
        cJSON_Delete(cfg);
    }
    assert(telemetry_wait_ms(0, 60000, 10000, true) == 10000);
    assert(telemetry_wait_ms(0, 60000, 10000, false) == 60000);
    assert(telemetry_wait_ms(20000, 60000, 10000, true) == 1);
    now_us = 0;
    reset_live_stats();
    live_count = state_count = 0;
    if (setjmp(stop_worker) == 0) mqtt_telemetry_task(NULL);
    assert(state_count == 4);
    assert(state_times[0] == 5000 && state_times[1] == 7000 && state_times[2] == 15000);
    assert(state_times[3] == 60000);
    assert(live_count == 9);
    int64_t expected_times[] = { 5000, 7000, 15000, 25000, 35000, 45000, 55000, 60000, 65000 };
    for (int i = 0; i < live_count; i++) assert(live_times[i] == expected_times[i]);
    assert(s_state_locks == 0 && s_mqtt_locks == 0);
    cJSON_Delete(last_live);
    puts("mqtt_live_contract_test: statistics, discovery, freshness and deadlines passed");
    return 0;
}
'''

assert "if (!cJSON_GetObjectItem(cfg, \"availability\"))" in mqtt
assert "if (!duplicate) bsp_display_set_therapy_start_time(now_us);" in writer
assert "bsp_display_push_pressure(pld_vals[0] / 100.0f)" in writer
assert "for (int j = 0; j < leak_n; j++)" in writer
assert writer.index("bsp_display_push_leak(leak_vals[j]") > writer.index("THERAPY detected via")
assert "s_leak_updated_us = 0;" in function(display, "bsp_display_set_as11_paired")

cjson_dir = os.environ.get("CJSON_DIR")
if cjson_dir:
    includes = [f"-I{cjson_dir}"]
    library = [str(Path(cjson_dir) / "cJSON.c")]
else:
    includes = ["-I/usr/include/cjson"]
    library = ["-lcjson"]
with tempfile.TemporaryDirectory(prefix="somnotrace-mqtt-live-") as tmp:
    source = Path(tmp) / "test.c"
    binary = Path(tmp) / "test"
    source.write_text(code)
    command = shlex.split(os.environ.get("CC", "gcc")) + [
        "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-function", "-O1", "-g",
        *includes, f"-I{MAIN}", f"-I{ROOT / 'scripts/test_include'}", str(source),
        str(MAIN / "leak_roll.c"), *library,
        "-lm", "-o", str(binary),
    ]
    if not os.environ.get("SNT_NO_SANITIZE"):
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
