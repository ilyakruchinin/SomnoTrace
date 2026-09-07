#include "controller_diagnostics.h"
#include <string.h>

#ifdef CONTROLLER_DIAGNOSTICS_HOST_TEST
/* The host test supplies a deterministic clock. Firmware always uses the
 * real clock and a cross-core critical section. */
extern int64_t esp_timer_get_time(void);
#define DIAG_LOCK() ((void)0)
#define DIAG_UNLOCK() ((void)0)
#else
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define DIAG_LOCK() portENTER_CRITICAL(&s_lock)
#define DIAG_UNLOCK() portEXIT_CRITICAL(&s_lock)
#endif

static controller_diagnostics_snapshot_t s_snapshot;

static uint32_t increment(uint32_t value)
{
    return value < UINT32_MAX ? value + 1U : value;
}

void controller_diagnostics_init(bool simulated)
{
    DIAG_LOCK();
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.initialized = true;
    s_snapshot.simulated = simulated;
    DIAG_UNLOCK();
}

void controller_diagnostics_record(controller_operation_t operation,
                                   esp_err_t result)
{
    if ((unsigned)operation >= CONTROLLER_OPERATION_COUNT) return;
    DIAG_LOCK();
    if (!s_snapshot.initialized) {
        DIAG_UNLOCK();
        return;
    }
    int64_t now = esp_timer_get_time();
    controller_operation_status_t *status = &s_snapshot.operations[operation];
    bool continued = status->observed && status->last_result == result;
    status->observed = true;
    status->last_result = result;
    status->last_observed_us = now;
    if (result != ESP_OK) {
        status->error_count = increment(status->error_count);
        status->last_error_us = now;
        size_t found = s_snapshot.history_count;
        if (continued) {
            for (size_t i = 0; i < s_snapshot.history_count; ++i) {
                if (s_snapshot.history[i].operation == operation) {
                    /* The newest episode for this operation is the only one
                     * that can still be active. Never join a pre-recovery one. */
                    if (s_snapshot.history[i].result == result) found = i;
                    break;
                }
            }
        }
        controller_error_episode_t episode = {
            .operation = operation, .result = result, .occurrences = 1,
            .first_us = now, .last_us = now,
        };
        size_t move;
        if (found < s_snapshot.history_count) {
            episode = s_snapshot.history[found];
            episode.occurrences = increment(episode.occurrences);
            episode.last_us = now;
            move = found;
        } else {
            if (s_snapshot.history_count < CONTROLLER_DIAGNOSTICS_HISTORY_MAX)
                ++s_snapshot.history_count;
            move = s_snapshot.history_count - 1U;
        }
        memmove(&s_snapshot.history[1], &s_snapshot.history[0],
                move * sizeof(s_snapshot.history[0]));
        s_snapshot.history[0] = episode;
    }
    DIAG_UNLOCK();
}

void controller_diagnostics_get_snapshot(controller_diagnostics_snapshot_t *out)
{
    if (!out) return;
    DIAG_LOCK();
    *out = s_snapshot;
    DIAG_UNLOCK();
}

const char *controller_diagnostics_operation_name(controller_operation_t operation)
{
    static const char *const names[] = {
        "Display initialization", "RGB frame submission", "RGB frame handoff",
        "Touch initialization", "Touch read", "Backlight power",
        "Backlight brightness",
        "Touch recovery", "Controller output mode", "LCD power",
    };
    return (unsigned)operation < CONTROLLER_OPERATION_COUNT
               ? names[operation] : "Unknown controller operation";
}
