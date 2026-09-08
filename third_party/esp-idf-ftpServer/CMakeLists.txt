idf_component_register(
    SRCS "ftp.c"
    INCLUDE_DIRS "."
    REQUIRES freertos esp_wifi lwip fatfs spi_flash nvs_flash
)
