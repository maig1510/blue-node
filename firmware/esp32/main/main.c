#include <stdio.h>
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "driver/gpio.h"


#include "a01nyub.h"

#define TAG "MAIN"

static a01nyub_t s_sensor;

void app_main(void)
{
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
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Waiting for first reading...");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}