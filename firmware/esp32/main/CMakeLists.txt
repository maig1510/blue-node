idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES "a01nyub" "spi_flash" "wifi_manager" "nvs_flash" "mqtt_manager"
)
