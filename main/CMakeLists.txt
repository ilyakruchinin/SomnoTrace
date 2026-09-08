idf_component_register(SRCS "main.c" "bsp_power.c" "bsp_display.c" "net_provision.c" "as11_ble.c"
                           "sd_storage.c" "session_writer.c" "time_sync.c"
                           "post_therapy.c" "edf_gen.c" "edf_header.c" "edf_waveform.c" "edf_annotations.c" "edf_summary.c" "log_stream.c" "device_settings.c"
                           "psram_task.c" "bsp_audio.c" "bsp_i2c.c" "bsp_touch.c" "session_graph.c" "nvs_writer.c"
                           "as11_time.c"
                           "crash_diag.c"
                           "oximeter.c" "oximeter_oxyii.c" "oximeter_legacy.c" "oximeter_store.c" "oximetry_vld3.c" "oximetry_canonical.c" "oximetry_http.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd driver esp_wifi esp_netif esp_event nvs_flash esp_http_server json esp_app_format
                                bt mbedtls esp_rom fatfs sdmmc esp-idf-ftpServer uploader esp_http_client
                                esp_driver_i2c esp_driver_i2s esp_adc app_update espcoredump esp_https_ota therapy_alert esp_coex)

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
