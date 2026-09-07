idf_component_register(
    SRCS "therapy_alert.c" "alert_history.c" "alert_config_model.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash json esp_http_client esp_timer mbedtls
)
