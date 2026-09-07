/* SomnoTrace - runtime controller for the bounded native Logs pane. */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* show() lazily creates the controller and its LVGL tree on first entry. */
esp_err_t touch_logs_controller_show(lv_obj_t *parent);
void touch_logs_controller_hide(void);
bool touch_logs_controller_is_visible(void);
bool touch_logs_controller_is_paused(void);

/* Called from LVGL only while Logs is the visible Manage destination. */
void touch_logs_controller_refresh(bool card_available);

/* Refuses destruction while a bounded snapshot/save/retry worker still owns
 * the controller, preventing a use-after-free during display teardown. */
esp_err_t touch_logs_controller_destroy(void);

#ifdef __cplusplus
}
#endif
