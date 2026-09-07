set(SOMNOTRACE_MAIN_SRC "main.c")
set(SOMNOTRACE_BSP_SRCS "bsp_power.c" "bsp_display.c" "bsp_audio.c" "bsp_i2c.c" "bsp_touch.c")
set(SOMNOTRACE_DISPLAY_REQUIRES)
if(CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B)
    set(SOMNOTRACE_BSP_SRCS
        "board_waveshare_7b.c"
        "bsp_power_7b.c"
        "bsp_display_7b.c"
        "bsp_audio_7b.c")
    list(APPEND SOMNOTRACE_DISPLAY_REQUIRES lvgl esp_lcd_touch_gt911)
elseif(CONFIG_SOMNOTRACE_BOARD_QEMU)
    if(CONFIG_SOMNOTRACE_QEMU_DISPLAY_154)
        set(SOMNOTRACE_MAIN_SRC "main_qemu_154.c")
        set(SOMNOTRACE_BSP_SRCS
            "board_qemu_154.c"
            "bsp_power_7b.c"
            "bsp_display.c"
            "bsp_audio_7b.c")
    else()
        set(SOMNOTRACE_MAIN_SRC "main_qemu.c")
        set(SOMNOTRACE_BSP_SRCS
            "board_qemu.c"
            "bsp_power_7b.c"
            "bsp_display_7b.c"
            "bsp_audio_7b.c")
        list(APPEND SOMNOTRACE_DISPLAY_REQUIRES lvgl esp_lcd_touch_gt911)
    endif()
    list(APPEND SOMNOTRACE_DISPLAY_REQUIRES esp_lcd_qemu_rgb)
endif()

if(CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B OR
   (CONFIG_SOMNOTRACE_BOARD_QEMU AND NOT CONFIG_SOMNOTRACE_QEMU_DISPLAY_154))
    list(APPEND SOMNOTRACE_BSP_SRCS "touch_observation.c" "controller_diagnostics.c")
endif()

idf_component_register(SRCS ${SOMNOTRACE_MAIN_SRC} "firmware_target.c" ${SOMNOTRACE_BSP_SRCS} "net_provision.c" "as11_ble.c"
                           "sd_storage.c" "session_writer.c" "time_sync.c"
                           "post_therapy.c" "edf_gen.c" "edf_header.c" "edf_waveform.c" "edf_annotations.c" "edf_summary.c" "log_stream.c" "device_settings.c"
                           "psram_task.c" "session_graph.c" "nvs_writer.c"
                           "as11_time.c"
                           "crash_diag.c"
                           "oximeter.c" "oximeter_oxyii.c" "oximeter_legacy.c" "oximeter_store.c" "oximetry_vld3.c" "oximetry_canonical.c" "oximetry_http.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd driver esp_wifi esp_netif esp_event nvs_flash esp_http_server json esp_app_format
                                bt mbedtls esp_rom fatfs sdmmc esp-idf-ftpServer uploader esp_http_client
                                esp_driver_i2c esp_driver_i2s esp_adc app_update espcoredump esp_https_ota therapy_alert esp_coex ${SOMNOTRACE_DISPLAY_REQUIRES})

# Retain board identity even when OTA services are not linked.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u somnotrace_firmware_target")

# Download zones.json (IANA-to-POSIX timezone database) if not present.
# Source: https://github.com/nayarsystems/posix_tz_db (MIT License)
find_package(Python3 REQUIRED)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_LIST_DIR}/zones.json"
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/../scripts/gen_tz_db.py
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/../scripts/gen_tz_db.py
    COMMENT "Fetching timezone database (zones.json)"
    VERBATIM)
add_custom_target(tz_db DEPENDS "${CMAKE_CURRENT_LIST_DIR}/zones.json")
add_dependencies(${COMPONENT_LIB} tz_db)

target_add_binary_data(${COMPONENT_LIB} "portal.html" TEXT)
target_add_binary_data(${COMPONENT_LIB} "zones.json" TEXT)
target_add_binary_data(${COMPONENT_LIB} "../third_party/uplot/dist/uPlot.iife.min.js" TEXT)
target_add_binary_data(${COMPONENT_LIB} "../third_party/uplot/dist/uPlot.min.css" TEXT)
target_add_binary_data(${COMPONENT_LIB} "../assets/svg/logo-full.svg" TEXT)
target_add_binary_data(${COMPONENT_LIB} "../assets/svg/logo-small.svg" TEXT)
