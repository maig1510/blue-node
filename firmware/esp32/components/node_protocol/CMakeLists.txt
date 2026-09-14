idf_component_register(
    SRCS        "node_protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES     "mqtt_manager" "esp_hw_support" "esp_app_format"
)
