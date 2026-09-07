#pragma once
#include "esp_err.h"
#include "lvgl.h"
typedef enum { MAINT_UI_STORAGE, MAINT_UI_SYSTEM, MAINT_UI_ADVANCED } maintenance_ui_destination_t;
typedef enum {
    MAINT_NAV_DEVICES,
    MAINT_NAV_CONNECTIVITY,
    MAINT_NAV_UPLOADS,
    MAINT_NAV_LOGS
} maintenance_ui_navigation_t;
typedef struct {
    void (*navigate)(maintenance_ui_navigation_t destination);
    void (*screen_off)(void);
    bool (*wake_available)(void);
    void (*restart)(void); /* shared shell's confirmed, therapy-fenced restart */
} maintenance_ui_hooks_t;
esp_err_t touch_maintenance_ui_show(lv_obj_t *parent, maintenance_ui_destination_t destination,
                                    const maintenance_ui_hooks_t *hooks);
void touch_maintenance_ui_destroy(void);
/* LVGL task only; polling copies bounded snapshots and never performs I/O. */
void touch_maintenance_ui_refresh(bool services_ready);

/* LVGL-thread hook after successful framebuffer publication; QEMU trace only. */
void touch_maintenance_ui_frame_published(void);
