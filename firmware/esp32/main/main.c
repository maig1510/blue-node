#include <stdio.h>
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "nvs_flash.h"

#include "a01nyub.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "node_protocol.h"

#define TAG "MAIN"

// Set URL of development MQTT broker
#define MQTT_BROKER_URI "mqtt://192.168.100.129:1883"

#define MEASUREMENT_INTERVAL_MS 1000

static a01nyub_t s_sensor;

/* Placeholder: settings arrive here (e.g. {"interval_ms":2000}) once the
 * config handling lands. Runs on the mqtt event task — keep it short. */
static void on_config(const char *json, int len, void *user_ctx)
{
    ESP_LOGI(TAG, "Config received: %.*s", len, json);
}

/* Placeholder: one-shot actions arrive here (e.g. {"cmd":"restart"}). */
static void on_command(const char *json, int len, void *user_ctx)
{
    ESP_LOGI(TAG, "Command received: %.*s", len, json);
}

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

    /* Before mqtt_manager_init(): the state topic built here is what the client
     * needs as its Last Will topic. */
    node_protocol_config_t proto_cfg = {
        .on_config  = on_config,
        .on_command = on_command,
    };
    ESP_ERROR_CHECK(node_protocol_init(&proto_cfg));

    mqtt_manager_config_t mqtt_cfg = {
        .broker_uri = MQTT_BROKER_URI,
    };
    node_protocol_apply_lwt(&mqtt_cfg);

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

            char payload[32];
            int len = snprintf(payload, sizeof(payload), "{\"distance_mm\":%u}", distance);
            node_protocol_publish_telemetry(payload, len);
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Waiting for first reading...");
        }

        vTaskDelay(pdMS_TO_TICKS(MEASUREMENT_INTERVAL_MS));
    }
}
