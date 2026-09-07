/*
 * SomnoTrace board support for Waveshare ESP32-S3-Touch-LCD-7B.
 *
 * The RGB timings, GPIO map, and CH32V003 controller assignments are adapted
 * from the Waveshare ESP32-S3-Touch-LCD-7B examples (Apache-2.0). This
 * adaptation is part of SomnoTrace and distributed under GPL-3.0-or-later.
 */

#include "board_waveshare_7b.h"

#include <inttypes.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "controller_diagnostics.h"
#include "psram_task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define I2C_SDA GPIO_NUM_8
#define I2C_SCL GPIO_NUM_9
#define I2C_HZ  400000

#define IOX_ADDR       0x24
#define IOX_REG_MODE   0x02
#define IOX_REG_OUTPUT 0x03
#define IOX_REG_PWM    0x05
#define IOX_TOUCH_RST  1
#define IOX_BACKLIGHT  2
#define IOX_SD_CS      4
#define IOX_LCD_POWER  6
#define IOX_USB_SELECT 5
#define BOARD_I2C_TIMEOUT_MS 20
#define BOARD_LOCK_TIMEOUT_MS 25
#define BOARD_VISIBILITY_RETRY_US 250000LL
#define BOARD_TOUCH_OFF_RESET_DELAY_US 50000LL
#define GT911_ADDR 0x5d
#define GT911_STATUS_REG 0x814e
#define GT911_ID_REG 0x8140

static const char *TAG = "board_7b";
static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_iox;
static SemaphoreHandle_t s_lock;
static uint8_t s_output = 0xff;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static i2c_master_dev_handle_t s_touch_device;
static TaskHandle_t s_touch_task;
static portMUX_TYPE s_touch_lock = portMUX_INITIALIZER_UNLOCKED;
static touch_observation_t s_touch_observation;
static uint8_t s_attenuation;
/* The output byte is desired state. Only an acknowledged ON operation may
 * suppress a fresh touch's visibility check. This is not physical readback. */
static bool s_backlight_on_acked;
static bool s_backlight_off_acked;
/* OFF acceptance and a pending retry's generation check share s_lock. */
static uint32_t s_backlight_off_generation;
typedef enum {
    TOUCH_VISIBILITY_IDLE,
    TOUCH_VISIBILITY_CHECK,
    TOUCH_VISIBILITY_ASSERT,
} touch_visibility_work_t;
/* Only the touch worker owns this obligation and its retry deadline. */
static touch_visibility_work_t s_touch_visibility_work;
static uint32_t s_touch_visibility_generation;
static int64_t s_touch_visibility_retry_at;
static int64_t s_touch_visibility_requested_us;
/* A successful physical ON->OFF transition queues one dark maintenance reset.
 * ON cancels pending work; the touch task alone performs the reset. */
static uint32_t s_touch_off_reset_generation;
static int64_t s_touch_off_reset_due_us;
static uint32_t s_touch_off_reset_requests;
static bool s_touch_off_reset_pending;
static bool s_touch_off_reset_active;
static bool s_touch_off_reset_cancelled;

static esp_err_t iox_write(uint8_t reg, uint8_t value)
{
    if (!s_iox) return ESP_ERR_INVALID_STATE;
    uint8_t bytes[2] = { reg, value };
    esp_err_t result = i2c_master_transmit(s_iox, bytes, sizeof(bytes), BOARD_I2C_TIMEOUT_MS);
    if (result != ESP_OK) {
        s_backlight_on_acked = false;
        s_backlight_off_acked = false;
    }
    return result;
}

static esp_err_t iox_output(unsigned pin, bool high)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(BOARD_LOCK_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (pin == IOX_BACKLIGHT && high &&
        __atomic_load_n(&s_touch_off_reset_active, __ATOMIC_ACQUIRE)) {
        /* Keep the panel dark until an already-asserted touch reset is safe to
         * release. The BSP retains ON as desired state and retries shortly. */
        __atomic_store_n(&s_touch_off_reset_cancelled, true, __ATOMIC_RELEASE);
        xSemaphoreGive(s_lock);
        return ESP_ERR_TIMEOUT;
    }
    if (pin == IOX_BACKLIGHT && !high &&
        __atomic_load_n(&s_touch_off_reset_active, __ATOMIC_ACQUIRE)) {
        /* OFF is now the newest intent. Withdraw an earlier ON cancellation
         * and let the already-claimed dark reset satisfy this transition. */
        __atomic_store_n(&s_touch_off_reset_cancelled, false, __ATOMIC_RELEASE);
    }
    bool was_off_acked = s_backlight_off_acked;
    uint32_t off_generation = s_backlight_off_generation;
    if (pin == IOX_BACKLIGHT && !high)
        off_generation = __atomic_add_fetch(&s_backlight_off_generation, 1U,
                                            __ATOMIC_RELAXED);
    if (high) s_output |= (uint8_t)(1U << pin);
    else s_output &= (uint8_t)~(1U << pin);
    esp_err_t ret = iox_write(IOX_REG_OUTPUT, s_output);
    if (pin == IOX_BACKLIGHT) {
        s_backlight_on_acked = ret == ESP_OK && high;
        s_backlight_off_acked = ret == ESP_OK && !high;
        if (ret == ESP_OK && !high && !was_off_acked) {
            s_touch_off_reset_generation = off_generation;
            s_touch_off_reset_due_us = esp_timer_get_time() +
                                       BOARD_TOUCH_OFF_RESET_DELAY_US;
            __atomic_store_n(&s_touch_off_reset_pending, true, __ATOMIC_RELEASE);
            __atomic_add_fetch(&s_touch_off_reset_requests, 1U,
                               __ATOMIC_RELAXED);
        } else if (ret == ESP_OK && !high &&
                   __atomic_load_n(&s_touch_off_reset_pending, __ATOMIC_ACQUIRE)) {
            /* Preserve one already-queued reset across a duplicate OFF write,
             * while advancing its ordering fence to the newest generation. */
            s_touch_off_reset_generation = off_generation;
        } else if (ret == ESP_OK && high) {
            s_touch_off_reset_generation = 0;
            __atomic_store_n(&s_touch_off_reset_pending, false, __ATOMIC_RELEASE);
        }
    }
    xSemaphoreGive(s_lock);
    return ret;
}

static esp_err_t init_i2c_and_expander(void)
{
    if (s_i2c) return ESP_OK;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c), TAG,
                        "create board I2C bus");

    i2c_device_config_t dev_cfg = {
        .device_address = IOX_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev_cfg, &s_iox), TAG,
                        "attach CH32V003 I/O controller");

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "create expander mutex");
    ESP_RETURN_ON_ERROR(iox_write(IOX_REG_MODE, 0xff), TAG,
                        "configure CH32V003 outputs");
    ESP_RETURN_ON_ERROR(iox_write(IOX_REG_OUTPUT, s_output), TAG,
                        "initialize CH32V003 outputs");
    return ESP_OK;
}

static esp_err_t init_rgb_panel(void)
{
    if (s_panel) return ESP_OK;

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            /* Physical testing accepted Waveshare's 30.85 MHz scan clock as
             * the no-shimmer/no-tearing default when paired with the
             * cache-sized ten-line bounce buffer below.  Keep this boot value
             * aligned with the accepted runtime A/B result so a later flash
             * cannot silently restore the visibly shimmering 18 MHz mode. */
            .pclk_hz = 30850000,
            .h_res = WAVESHARE_7B_H_RES,
            .v_res = WAVESHARE_7B_V_RES,
            .hsync_pulse_width = 162,
            .hsync_back_porch = 152,
            .hsync_front_porch = 48,
            .vsync_pulse_width = 45,
            .vsync_back_porch = 13,
            .vsync_front_porch = 3,
            .flags.pclk_active_neg = true,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 2,
        /* Ten lines matches Waveshare's older 30.85 MHz 7B configuration.
         * Each RGB565 refill is 20 KiB, comfortably inside the configured
         * 64 KiB data cache, so scanout does not evict LVGL's entire working
         * set on every DMA EOF. The two buffers also use 40 KiB less internal
         * RAM than the previous twenty-line configuration. */
        .bounce_buffer_size_px = WAVESHARE_7B_H_RES * 10,
        .sram_trans_align = 4,
        .psram_trans_align = 64,
        .hsync_gpio_num = GPIO_NUM_46,
        .vsync_gpio_num = GPIO_NUM_3,
        .de_gpio_num = GPIO_NUM_5,
        .pclk_gpio_num = GPIO_NUM_7,
        .disp_gpio_num = GPIO_NUM_NC,
        .data_gpio_nums = {
            GPIO_NUM_14, GPIO_NUM_38, GPIO_NUM_18, GPIO_NUM_17, GPIO_NUM_10,
            GPIO_NUM_39, GPIO_NUM_0, GPIO_NUM_45, GPIO_NUM_48, GPIO_NUM_47,
            GPIO_NUM_21, GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_42, GPIO_NUM_41,
            GPIO_NUM_40,
        },
        .flags.fb_in_psram = true,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, &s_panel), TAG,
                        "create RGB panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "initialize RGB panel");
    return ESP_OK;
}

static esp_err_t init_touch(void)
{
    if (s_touch) return ESP_OK;

    /* Select the GT911's 0x5d address while releasing reset through EXIO1. */
    ESP_RETURN_ON_ERROR(iox_output(IOX_TOUCH_RST, false), TAG, "hold touch reset");
    gpio_config_t int_out = {
        .pin_bit_mask = 1ULL << GPIO_NUM_4,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_out), TAG, "configure touch interrupt");
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(GPIO_NUM_4, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(iox_output(IOX_TOUCH_RST, true), TAG, "release touch reset");
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = I2C_HZ;
    esp_lcd_panel_io_handle_t touch_io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &touch_io), TAG,
                        "create GT911 I2C IO");

    static esp_lcd_touch_io_gt911_config_t gt_cfg;
    gt_cfg.dev_addr = io_cfg.dev_addr;
    esp_lcd_touch_config_t touch_cfg = {
        .x_max = WAVESHARE_7B_H_RES,
        .y_max = WAVESHARE_7B_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_4,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
        .driver_data = &gt_cfg,
    };
    esp_err_t result = esp_lcd_touch_new_i2c_gt911(touch_io, &touch_cfg, &s_touch);
    if (result != ESP_OK) {
        /* The vendor deletes its touch object on failure, but leaves the
         * caller-owned panel IO allocated. Runtime recovery uses its own IO. */
        esp_lcd_panel_io_del(touch_io);
        s_touch = NULL;
    }
    return result;
}

esp_err_t waveshare_7b_init(esp_lcd_panel_handle_t *panel,
                            esp_lcd_touch_handle_t *touch)
{
    ESP_RETURN_ON_ERROR(init_i2c_and_expander(), TAG, "board control init");
    ESP_RETURN_ON_ERROR(iox_output(IOX_LCD_POWER, true), TAG, "LCD power enable");
    /* EXIO5 low connects the native USB port instead of the optional CAN
     * transceiver, preserving USB-Serial-JTAG logs after board init. */
    ESP_RETURN_ON_ERROR(iox_output(IOX_USB_SELECT, false), TAG, "USB select");
    ESP_RETURN_ON_ERROR(iox_output(IOX_SD_CS, true), TAG, "TF card select release");
    ESP_RETURN_ON_ERROR(init_rgb_panel(), TAG, "RGB display init");

    esp_err_t touch_ret = init_touch();
    controller_diagnostics_record(CONTROLLER_TOUCH_INIT, touch_ret);
    if (touch_ret != ESP_OK) {
        /* The display and the rest of SomnoTrace remain useful without touch. */
        ESP_LOGW(TAG, "GT911 unavailable: %s", esp_err_to_name(touch_ret));
    }
    ESP_RETURN_ON_ERROR(iox_output(IOX_BACKLIGHT, true), TAG, "backlight enable");

    if (panel) *panel = s_panel;
    if (touch) *touch = s_touch;
    ESP_LOGI(TAG, "Waveshare 7B ready: RGB=%dx%d touch=%s",
             WAVESHARE_7B_H_RES, WAVESHARE_7B_V_RES, s_touch ? "yes" : "no");
    return ESP_OK;
}

esp_err_t waveshare_7b_set_backlight(bool on)
{
    esp_err_t result = init_i2c_and_expander();
    /* EXIO2 is the panel's hardware enable. An off request removes the
     * backlight electrically; it is not a black framebuffer or 0% PWM. */
    if (result == ESP_OK) result = iox_output(IOX_BACKLIGHT, on);
    controller_diagnostics_record(CONTROLLER_BACKLIGHT_POWER, result);
    return result;
}

esp_err_t waveshare_7b_set_brightness(uint8_t percent)
{
    esp_err_t result = init_i2c_and_expander();
    waveshare_7b_recovery_brightness(percent);
    if (percent > 100) percent = 100;
    /* The Waveshare I/O controller drives the backlight PWM active-low:
     * command 0 is steady/full-on and increasing values add off-time. Keep
     * the vendor's 97% attenuation limit so minimum brightness never becomes
     * indistinguishable from the separate hard-off control. */
    uint8_t attenuation = (uint8_t)(100U - percent);
    if (attenuation > 97) attenuation = 97;
    uint8_t pwm = (uint8_t)(attenuation * 255U / 100U);
    if (result == ESP_OK) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(BOARD_LOCK_TIMEOUT_MS)) != pdTRUE) {
            result = ESP_ERR_TIMEOUT;
        } else {
            /* This is desired PWM, retained for recovery even after failure. */
            pwm = __atomic_load_n(&s_attenuation, __ATOMIC_RELAXED);
            result = iox_write(IOX_REG_PWM, pwm);
            xSemaphoreGive(s_lock);
        }
    }
    controller_diagnostics_record(CONTROLLER_BACKLIGHT_BRIGHTNESS, result);
    return result;
}

void waveshare_7b_recovery_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint8_t attenuation = (uint8_t)(100U - percent);
    if (attenuation > 97) attenuation = 97;
    __atomic_store_n(&s_attenuation, (uint8_t)(attenuation * 255U / 100U),
                     __ATOMIC_RELAXED);
}

/* Caller holds s_lock, including any pending-work generation check. */
static esp_err_t reassert_visible_locked(void)
{
    if (__atomic_load_n(&s_touch_off_reset_active, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s_touch_off_reset_cancelled, true, __ATOMIC_RELEASE);
        return ESP_ERR_TIMEOUT;
    }
    /* Preserve SD/USB and an ongoing touch reset. Attempt each output stage
     * even when an earlier transaction fails. */
    esp_err_t mode = iox_write(IOX_REG_MODE, 0xff);
    esp_err_t pwm = iox_write(IOX_REG_PWM,
                               __atomic_load_n(&s_attenuation, __ATOMIC_RELAXED));
    s_output |= (uint8_t)((1U << IOX_LCD_POWER) | (1U << IOX_BACKLIGHT));
    esp_err_t power = iox_write(IOX_REG_OUTPUT, s_output);
    esp_err_t result = mode != ESP_OK ? mode : pwm != ESP_OK ? pwm : power;
    s_backlight_on_acked = result == ESP_OK;
    s_backlight_off_acked = false;
    if (result == ESP_OK) {
        s_touch_off_reset_generation = 0;
        __atomic_store_n(&s_touch_off_reset_pending, false, __ATOMIC_RELEASE);
    }
    controller_diagnostics_record(CONTROLLER_OUTPUT_MODE, mode);
    controller_diagnostics_record(CONTROLLER_BACKLIGHT_BRIGHTNESS, pwm);
    controller_diagnostics_record(CONTROLLER_LCD_POWER, power);
    controller_diagnostics_record(CONTROLLER_BACKLIGHT_POWER, power);
    return result;
}

esp_err_t waveshare_7b_reassert_visible(void)
{
    if (!s_lock || !s_iox) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(BOARD_LOCK_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    esp_err_t result = reassert_visible_locked();
    xSemaphoreGive(s_lock);
    return result;
}

void waveshare_7b_touch_snapshot(touch_observation_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_touch_lock);
    *out = s_touch_observation;
    portEXIT_CRITICAL(&s_touch_lock);
}

static void publish_touch(const touch_observation_t *state)
{
    portENTER_CRITICAL(&s_touch_lock);
    s_touch_observation = *state;
    portEXIT_CRITICAL(&s_touch_lock);
}

static esp_err_t touch_read_register(uint16_t reg, void *data, size_t size)
{
    uint8_t address[] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return i2c_master_transmit_receive(s_touch_device, address, sizeof(address),
                                      data, size, BOARD_I2C_TIMEOUT_MS);
}

static esp_err_t touch_read_frame(bool *frame, bool *pressed,
                                  uint16_t *x, uint16_t *y)
{
    *frame = false;
    *pressed = false;
    uint8_t status = 0;
    esp_err_t result = touch_read_register(GT911_STATUS_REG, &status, 1);
    if (result != ESP_OK || !(status & 0x80)) return result;
    uint8_t count = status & 0x0f;
    uint8_t point[8] = {0};
    if (count > 0 && count <= 5)
        result = touch_read_register(GT911_STATUS_REG + 1, point, sizeof(point));
    uint8_t clear[] = { GT911_STATUS_REG >> 8, GT911_STATUS_REG & 0xff, 0 };
    esp_err_t cleared = i2c_master_transmit(s_touch_device, clear, sizeof(clear),
                                           BOARD_I2C_TIMEOUT_MS);
    if (result != ESP_OK) return result;
    if (cleared != ESP_OK) return cleared;
    if (count > 5) return ESP_ERR_INVALID_SIZE;
    *frame = true;
    *pressed = count != 0;
    *x = (uint16_t)point[1] | (uint16_t)point[2] << 8;
    *y = (uint16_t)point[3] | (uint16_t)point[4] << 8;
    return ESP_OK;
}

static esp_err_t assert_touch_reset(bool cancellable, bool *cancelled)
{
    if (cancelled) *cancelled = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(BOARD_LOCK_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (cancellable &&
        __atomic_load_n(&s_touch_off_reset_active, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&s_touch_off_reset_cancelled, __ATOMIC_ACQUIRE)) {
        if (cancelled) *cancelled = true;
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    s_output &= (uint8_t)~(1U << IOX_TOUCH_RST);
    esp_err_t result = iox_write(IOX_REG_OUTPUT, s_output);
    xSemaphoreGive(s_lock);
    return result;
}

static esp_err_t recover_touch(bool cancellable, bool *cancelled)
{
    /* Only the touch worker uses GPIO4 or the GT911 runtime handle. LVGL never
     * waits for this reset sequence and observes released/invalid input until
     * a new complete controller frame arrives. */
    esp_err_t result = assert_touch_reset(cancellable, cancelled);
    if (cancelled && *cancelled) return ESP_OK;
    gpio_config_t output = {
        .pin_bit_mask = 1ULL << GPIO_NUM_4,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (result == ESP_OK) result = gpio_config(&output);
    if (result == ESP_OK) result = gpio_set_level(GPIO_NUM_4, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    /* Always try to release reset, even after a failed preceding operation. */
    esp_err_t release = iox_output(IOX_TOUCH_RST, true);
    if (result == ESP_OK) result = release;
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_config_t input = output;
    input.mode = GPIO_MODE_INPUT;
    /* Goodix specifies a floating INT line after address selection. */
    input.pull_up_en = GPIO_PULLUP_DISABLE;
    esp_err_t restored = gpio_config(&input);
    if (result == ESP_OK) result = restored;
    uint8_t id[4] = {0};
    if (result == ESP_OK) result = touch_read_register(GT911_ID_REG, id, sizeof(id));
    if (result == ESP_OK && (id[0] != '9' || id[1] != '1' || id[2] != '1'))
        result = ESP_ERR_INVALID_RESPONSE;
    controller_diagnostics_record(CONTROLLER_TOUCH_RECOVERY, result);
    return result;
}

static void service_touch_visibility(touch_observation_t *state)
{
    if (s_touch_visibility_work == TOUCH_VISIBILITY_IDLE ||
        esp_timer_get_time() < s_touch_visibility_retry_at) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(BOARD_LOCK_TIMEOUT_MS)) != pdTRUE) {
        s_touch_visibility_retry_at = esp_timer_get_time() + BOARD_VISIBILITY_RETRY_US;
        return;
    }
    /* A newer explicit OFF cancels old work before any retry can write ON.
     * Keeping this check and the write under one mutex closes the race. */
    if (s_touch_visibility_generation !=
        __atomic_load_n(&s_backlight_off_generation, __ATOMIC_RELAXED)) {
        s_touch_visibility_work = TOUCH_VISIBILITY_IDLE;
        xSemaphoreGive(s_lock);
        return;
    }
    bool notify = false;
    if (s_touch_visibility_work == TOUCH_VISIBILITY_CHECK) {
        if (s_backlight_on_acked) {
            s_touch_visibility_work = TOUCH_VISIBILITY_IDLE;
            xSemaphoreGive(s_lock);
            return;
        }
        s_touch_visibility_work = TOUCH_VISIBILITY_ASSERT;
        notify = true;
    }
    esp_err_t result = reassert_visible_locked();
    if (result == ESP_OK) s_touch_visibility_work = TOUCH_VISIBILITY_IDLE;
    else s_touch_visibility_retry_at = esp_timer_get_time() + BOARD_VISIBILITY_RETRY_US;
    xSemaphoreGive(s_lock);
    if (notify) {
        state->visibility_requested_us = s_touch_visibility_requested_us;
        ++state->visibility_requests;
        publish_touch(state);
    }
}

static void request_visible(touch_observation_t *state)
{
    s_touch_visibility_work = TOUCH_VISIBILITY_ASSERT;
    s_touch_visibility_generation =
        __atomic_load_n(&s_backlight_off_generation, __ATOMIC_RELAXED);
    s_touch_visibility_retry_at = 0;
    s_touch_visibility_requested_us = esp_timer_get_time();
    state->visibility_requested_us = s_touch_visibility_requested_us;
    ++state->visibility_requests;
    publish_touch(state);
    /* The independent operation persists until acknowledged or superseded. */
    service_touch_visibility(state);
}

static void queue_touch_wake_check(uint32_t generation, int64_t requested_us)
{
    if (s_touch_visibility_work != TOUCH_VISIBILITY_IDLE &&
        s_touch_visibility_generation == generation) return;
    s_touch_visibility_work = TOUCH_VISIBILITY_CHECK;
    s_touch_visibility_generation = generation;
    s_touch_visibility_requested_us = requested_us;
    s_touch_visibility_retry_at = 0;
}

static bool service_touch_off_reset(touch_observation_t *state)
{
    state->preventive_recovery_requests =
        __atomic_load_n(&s_touch_off_reset_requests, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&s_touch_off_reset_pending, __ATOMIC_ACQUIRE))
        return false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(BOARD_LOCK_TIMEOUT_MS)) != pdTRUE)
        return false;
    if (!__atomic_load_n(&s_touch_off_reset_pending, __ATOMIC_ACQUIRE) ||
        esp_timer_get_time() < s_touch_off_reset_due_us) {
        xSemaphoreGive(s_lock);
        return false;
    }
    uint32_t generation = s_touch_off_reset_generation;
    bool current = generation != 0 &&
                   generation == __atomic_load_n(&s_backlight_off_generation,
                                                  __ATOMIC_RELAXED) &&
                   s_backlight_off_acked &&
                   !(s_output & (1U << IOX_BACKLIGHT));
    s_touch_off_reset_generation = 0;
    __atomic_store_n(&s_touch_off_reset_pending, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_touch_off_reset_active, current, __ATOMIC_RELEASE);
    __atomic_store_n(&s_touch_off_reset_cancelled, false, __ATOMIC_RELEASE);
    xSemaphoreGive(s_lock);
    if (!current) return false;

    touch_observation_preventive_recovering(state);
    publish_touch(state);
    bool cancelled = false;
    esp_err_t result = recover_touch(true, &cancelled);
    __atomic_store_n(&s_touch_off_reset_active, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_touch_off_reset_cancelled, false, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "dark touch reset %lu: %s",
             (unsigned long)state->preventive_recovery_attempts,
             esp_err_to_name(result));
    if (result == ESP_OK) {
        /* Keep the maintenance marker until the first status read, but do not
         * route a completed reset through the failure-recovery branch below. */
        state->recovering = false;
        if (cancelled) state->preventive_recovery = false;
        return true;
    }

    /* A failed maintenance reset becomes ordinary sticky recovery so the UI
     * fails visibly and the existing bounded retry path takes ownership. */
    state->preventive_recovery = false;
    touch_observation_update(state, esp_timer_get_time(), result,
                             false, false, 0, 0);
    publish_touch(state);
    return true;
}

static void touch_task(void *argument)
{
    (void)argument;
    touch_observation_t state = {0};
    if (!s_touch) {
        /* A failed boot probe may leave reset/INT partway through address
         * selection. Restore those pins before the first runtime read. */
        state.consecutive_errors = TOUCH_OBSERVATION_FAILURE_LIMIT;
        state.recovering = true;
        publish_touch(&state);
    }
    int64_t retry_at = 0;
    for (;;) {
        service_touch_visibility(&state);
        (void)service_touch_off_reset(&state);
        int64_t now = esp_timer_get_time();
        if (state.recovering ||
            state.consecutive_errors >= TOUCH_OBSERVATION_FAILURE_LIMIT) {
            /* A status read cannot prove that reset/INT restoration succeeded.
             * Keep a failed reset pending until the complete sequence passes. */
            if (now < retry_at) {
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            touch_observation_recovering(&state);
            publish_touch(&state);
            request_visible(&state);
            esp_err_t recovered = recover_touch(false, NULL);
            retry_at = esp_timer_get_time() + 5000000LL;
            if (recovered != ESP_OK) {
                touch_observation_update(&state, esp_timer_get_time(), recovered,
                                          false, false, 0, 0);
                publish_touch(&state);
            }
            ESP_LOGW(TAG, "touch recovery attempt %lu: %s",
                     (unsigned long)state.recovery_attempts, esp_err_to_name(recovered));
            if (recovered != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
        }
        bool frame = false, pressed = false;
        uint16_t x = 0, y = 0;
        bool was_pressed = touch_observation_pressed(&state, esp_timer_get_time());
        uint8_t prior_errors = state.consecutive_errors;
        /* Keep the original sample order across a concurrent OFF or a delayed
         * lock retry. A late notification must not become a new wake request. */
        uint32_t sampled_generation =
            __atomic_load_n(&s_backlight_off_generation, __ATOMIC_RELAXED);
        int64_t sampled_us = esp_timer_get_time();
        esp_err_t result = touch_read_frame(&frame, &pressed, &x, &y);
        controller_diagnostics_record(CONTROLLER_TOUCH_READ, result);
        if (result != ESP_OK && state.preventive_recovery)
            state.preventive_recovery = false;
        touch_observation_update(&state, esp_timer_get_time(), result,
                                  frame, pressed, x, y);
        bool failed = prior_errors < TOUCH_OBSERVATION_FAILURE_LIMIT &&
                      state.consecutive_errors >= TOUCH_OBSERVATION_FAILURE_LIMIT;
        if (!was_pressed && touch_observation_pressed(&state, esp_timer_get_time()))
            queue_touch_wake_check(sampled_generation, sampled_us);
        if (failed) request_visible(&state);
        service_touch_visibility(&state);
        publish_touch(&state);
        /* Bound a failed peripheral's logging and bus traffic; never hammer
         * it at the UI's 10 ms input cadence while it is unavailable. */
        vTaskDelay(pdMS_TO_TICKS(result == ESP_OK ? 10 : 250));
    }
}

esp_err_t waveshare_7b_start_touch(void)
{
    if (s_touch_task) return ESP_OK;
    if (!s_i2c || !s_lock || !s_iox) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t device = {
        .device_address = GT911_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    esp_err_t result = i2c_master_bus_add_device(s_i2c, &device, &s_touch_device);
    if (result != ESP_OK) return result;
    s_touch_task = psram_task_create(touch_task, "touch_7b", 4096, NULL, 5,
                                     tskNO_AFFINITY, NULL, NULL);
    if (!s_touch_task) {
        i2c_master_bus_rm_device(s_touch_device);
        s_touch_device = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t waveshare_7b_set_panel_pclk(uint32_t hz)
{
    /* Keep this deliberately narrow: 30.85 MHz is the accepted boot clock;
     * 18 MHz remains available only as an A/B diagnostic fallback. */
    if (hz != 18000000U && hz != 30850000U) return ESP_ERR_INVALID_ARG;
    if (!s_panel) return ESP_ERR_INVALID_STATE;

    esp_err_t err = esp_lcd_rgb_panel_set_pclk(s_panel, hz);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "diagnostic RGB pixel clock requested: %" PRIu32 " Hz", hz);
    }
    return err;
}

esp_err_t waveshare_7b_prepare_sd(void)
{
    ESP_RETURN_ON_ERROR(init_i2c_and_expander(), TAG, "board control init");
    /* In one-bit SD mode DAT3/CS must remain high. */
    return iox_output(IOX_SD_CS, true);
}
