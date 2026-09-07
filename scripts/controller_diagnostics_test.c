#include "controller_diagnostics.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int64_t s_now;
int64_t esp_timer_get_time(void) { return s_now; }

static void record(controller_operation_t operation, esp_err_t result)
{
    s_now += 1000;
    controller_diagnostics_record(operation, result);
}

int main(void)
{
    controller_diagnostics_snapshot_t before, snap;
    controller_diagnostics_get_snapshot(&snap);
    assert(!snap.initialized && !snap.operations[CONTROLLER_TOUCH_READ].observed);
    record(CONTROLLER_TOUCH_READ, ESP_FAIL);
    controller_diagnostics_get_snapshot(&snap);
    assert(!snap.initialized && snap.history_count == 0);

    controller_diagnostics_init(false);
    record(CONTROLLER_TOUCH_INIT, ESP_OK);
    record(CONTROLLER_TOUCH_READ, ESP_FAIL);
    record(CONTROLLER_PANEL_SUBMIT, ESP_ERR_INVALID_STATE);
    record(CONTROLLER_TOUCH_READ, ESP_FAIL);
    controller_diagnostics_get_snapshot(&snap);
    assert(snap.initialized && !snap.simulated && snap.history_count == 2);
    assert(!snap.operations[CONTROLLER_PANEL_HANDOFF].observed);
    assert(snap.history[0].operation == CONTROLLER_TOUCH_READ);
    assert(snap.history[0].occurrences == 2);
    assert(snap.history[0].last_us - snap.history[0].first_us == 2000);
    assert(snap.operations[CONTROLLER_TOUCH_READ].error_count == 2);

    /* Recovery updates current health without erasing dated failure evidence. */
    before = snap;
    record(CONTROLLER_TOUCH_READ, ESP_OK);
    controller_diagnostics_get_snapshot(&snap);
    assert(snap.operations[CONTROLLER_TOUCH_READ].last_result == ESP_OK);
    assert(snap.operations[CONTROLLER_TOUCH_READ].error_count == 2);
    assert(memcmp(snap.history, before.history, sizeof(snap.history)) == 0);
    record(CONTROLLER_TOUCH_READ, ESP_FAIL);
    controller_diagnostics_get_snapshot(&snap);
    assert(snap.history_count == 3 && snap.history[0].occurrences == 1);
    assert(snap.history[1].operation == CONTROLLER_TOUCH_READ);
    assert(snap.history[1].occurrences == 2);

    /* More episodes evict only old evidence and preserve lifetime counts. */
    for (unsigned i = 0; i < 20; ++i) {
        record(CONTROLLER_TOUCH_READ, ESP_OK);
        record(CONTROLLER_TOUCH_READ, ESP_FAIL);
    }
    controller_diagnostics_get_snapshot(&snap);
    assert(snap.history_count == CONTROLLER_DIAGNOSTICS_HISTORY_MAX);
    assert(snap.operations[CONTROLLER_TOUCH_READ].error_count == 23);
    for (size_t i = 1; i < snap.history_count; ++i)
        assert(snap.history[i - 1].last_us > snap.history[i].last_us);

    before = snap;
    record((controller_operation_t)-1, ESP_FAIL);
    record(CONTROLLER_OPERATION_COUNT, ESP_FAIL);
    controller_diagnostics_get_snapshot(&snap);
    assert(memcmp(&before, &snap, sizeof(snap)) == 0);
    controller_diagnostics_get_snapshot(NULL);
    assert(strcmp(controller_diagnostics_operation_name(CONTROLLER_TOUCH_READ),
                  "Touch read") == 0);

    /* Simulated observations never masquerade as physical-board readings. */
    controller_diagnostics_init(true);
    controller_diagnostics_get_snapshot(&snap);
    assert(snap.simulated && snap.history_count == 0);
    assert(!snap.operations[CONTROLLER_TOUCH_INIT].observed);
    puts("Controller diagnostics recovery and retention tests passed");
    return 0;
}
