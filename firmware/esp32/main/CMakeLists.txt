idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES "a01nyub" "nvs_flash" "esp_netif" "esp_event" "wifi_manager" "mqtt_manager"
             "node_protocol" "node_config" "provisioning" "status_led" "user_button"
)
