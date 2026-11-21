idf_component_register(
    SRCS         "provisioning.c"
    INCLUDE_DIRS "include"
    REQUIRES     "esp_event"
    PRIV_REQUIRES "node_config" "wifi_manager" "esp_wifi" "esp_netif" "esp_timer" "esp_app_format"
                  "esp_hw_support" "protocomm" "bt"
)
