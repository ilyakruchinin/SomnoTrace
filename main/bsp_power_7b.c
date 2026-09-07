/* SomnoTrace power shim for Waveshare ESP32-S3-Touch-LCD-7B. */
#include "bsp_power.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "bsp_power_7b";

void bsp_power_hold(void)
{
    /* The 7B is powered directly; GPIO2 is an LCD red-data signal. */
}

void bsp_power_off(void)
{
    ESP_LOGW(TAG, "power-off is not available on the 7B; use its supply switch");
}

void bsp_power_start_button_monitor(int hold_ms)
{
    (void)hold_ms;
}

void bsp_power_start_boot_monitor(volatile bool *softap_flag, int hold_ms)
{
    (void)softap_flag;
    (void)hold_ms;
    /* GPIO0 carries LCD G3 after boot and must not be polled as a button. */
}

void bsp_power_start_plus_monitor(void)
{
    /* The native touch UI replaces the 1.54-inch board's PLUS button. */
}

esp_err_t bsp_power_battery_monitor_start(void)
{
    return ESP_OK;
}

void bsp_power_battery_get(bsp_battery_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->percent = -1;
    out->millivolts = -1;
    out->valid = false;
}

int bsp_power_battery_percent(void)
{
    return -1;
}

bool bsp_power_is_charging(void)
{
    return false;
}
