#include <stdio.h>
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_mac.h"

#include "a01nyub.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"

#define TAG "MAIN"

// Set URL of development MQTT broker
#define MQTT_BROKER_URI "mqtt://192.168.100.129:1883"

static a01nyub_t s_sensor;

void app_main(void)
{
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    wifi_manager_config_t wifi_cfg = {
        .ssid        = "2.4G-2b7i",
        .password    = "d6e91b8k",
        .max_retries = 5,
    };

    if (wifi_manager_connect(&wifi_cfg) == ESP_OK) {
        char ip[16];
        wifi_manager_get_ip(ip, sizeof(ip));
        ESP_LOGI(TAG, "WiFi connected, IP: %s", ip);
    } else {
        ESP_LOGE(TAG, "WiFi connection failed");
    }

    char topic[48];
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(topic, sizeof(topic), "blue-node/%02X%02X%02X/distance",
             mac[3], mac[4], mac[5]);

    mqtt_manager_config_t mqtt_cfg = {
        .broker_uri = MQTT_BROKER_URI,
    };
    ESP_ERROR_CHECK(mqtt_manager_init(&mqtt_cfg));
    ESP_ERROR_CHECK(mqtt_manager_start());

    s_sensor          = (a01nyub_t)A01NYUB_DEFAULT();
    s_sensor.rx_pin   = GPIO_NUM_3;
    s_sensor.tx_pin   = GPIO_NUM_2;
    s_sensor.uart_port = UART_NUM_1;
    s_sensor.mode     = A01NYUB_MODE_FILTERED;
    s_sensor.read_mode = A01NYUB_READ_CONTINUOUS;

    ESP_ERROR_CHECK(a01nyub_init(&s_sensor));

    uint16_t distance;

    while (true) {
        esp_err_t ret = a01nyub_get_data(&s_sensor, &distance);

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Distance: %d mm  (%.1f cm)", distance, distance / 10.0f);

            if (mqtt_manager_is_connected()) {
                char payload[8];
                int len = snprintf(payload, sizeof(payload), "%u", distance);
                mqtt_manager_publish(topic, payload, len, 0, 0);
            }
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Waiting for first reading...");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}