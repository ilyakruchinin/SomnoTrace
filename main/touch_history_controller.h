/* Async controller joining the native History service to its retained UI. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "touch_history_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct touch_history_controller touch_history_controller_t;

/* Invoked from either the History worker or the LVGL caller. It must only
 * wake/schedule the display task; it must not call LVGL from the worker. */
typedef void (*touch_history_controller_changed_fn)(void *context);
typedef void (*touch_history_controller_route_card_fn)(void *context);

typedef struct {
    touch_history_controller_changed_fn changed;
    touch_history_controller_route_card_fn route_card;
    void *context;
    /* Zero means the usage target is unknown. */
    uint16_t usage_target_minutes;
    /* QEMU-only acceptance model. Production callers must leave this false;
     * it keeps touch/zoom/channel behavior testable without emulating SDMMC. */
    bool deterministic_preview;
} touch_history_controller_config_t;

/* The controller and its coherent published model are allocated from PSRAM.
 * Its mutex and one-slot queue use FreeRTOS's internal allocator, while the
 * worker stack and inactive work result remain PSRAM-backed. */
esp_err_t touch_history_controller_create(
    const touch_history_controller_config_t *config,
    touch_history_controller_t **out_controller);
void touch_history_controller_destroy(touch_history_controller_t *controller);

/* Entering History triggers a newest-first seven-row page load and selects
 * the newest night exactly once. Leaving cancels cancellable work and rejects
 * every in-flight generation without destroying the cached surface. */
esp_err_t touch_history_controller_set_active(
    touch_history_controller_t *controller, bool active);

/* Invalidates the cached index/night. If History is visible, immediately
 * queues a newest-first reload; otherwise the next activation reloads. */
esp_err_t touch_history_controller_refresh(
    touch_history_controller_t *controller);

/* Signature intentionally matches touch_history_ui_intent_fn. */
void touch_history_controller_handle_intent(
    void *context, const touch_history_ui_intent_t *intent);

/* Called only by the LVGL task. Applies one coherent deep-copy snapshot. */
esp_err_t touch_history_controller_apply(
    touch_history_controller_t *controller, touch_history_ui_t *ui);

/* Monotonic model revision, useful to avoid redundant LVGL applies. */
uint32_t touch_history_controller_revision(
    touch_history_controller_t *controller);

#ifdef __cplusplus
}
#endif
