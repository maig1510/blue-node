/**
 * @file a01nyub.c
 * @brief A01NYUB ultrasonic distance sensor driver implementation.
 */

#include "a01nyub.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** Byte index constants within a 4-byte sensor packet. */
#define PACKET_HEADER 0xFF /**< Fixed start-of-frame marker value. */
#define PACKET_DATA_H 1    /**< Index of the high byte of the distance word. */
#define PACKET_DATA_L 2    /**< Index of the low byte of the distance word. */
#define PACKET_SUM    3    /**< Index of the checksum byte. */
#define PACKET_SIZE   4    /**< Total packet length in bytes. */

static const char *TAG = "A01NYUB";

/**
 * @brief Read and validate one 4-byte sensor packet from UART.
 *
 * Scans the incoming byte stream for the 0xFF header, then collects the
 * remaining three bytes. Verifies the checksum (sum of all bytes except the
 * checksum byte, masked to 8 bits). On success the raw distance word is
 * written to @p value.
 *
 * @param dev    Driver instance (UART port must be installed).
 * @param value  Output: raw distance in mm on success.
 *
 * @return @c ESP_OK, @c ESP_ERR_TIMEOUT, or @c ESP_FAIL (bad checksum).
 */
static esp_err_t read_packet(a01nyub_t *dev, uint16_t *value)
{
    uint8_t packet[PACKET_SIZE];
    uint8_t byte;
    int idx = 0;

    while (idx < PACKET_SIZE)
    {
        int n = uart_read_bytes(dev->uart_port, &byte, 1, pdMS_TO_TICKS(1000));

        if (n <= 0) return ESP_ERR_TIMEOUT;

        /* Skip bytes until the start-of-frame marker is found. */
        if (idx == 0 && byte != PACKET_HEADER) continue;

        packet[idx++] = byte;
    }

    uint8_t checksum = (packet[0] + packet[PACKET_DATA_H] + packet[PACKET_DATA_L]) & 0xFF;
    if (checksum != packet[PACKET_SUM]) return ESP_FAIL;

    *value = ((uint16_t)packet[PACKET_DATA_H] << 8) | packet[PACKET_DATA_L];

    return ESP_OK;
}

/**
 * @brief FreeRTOS task that continuously reads packets in the background.
 *
 * Runs forever, calling @ref read_packet in a loop. On each successful
 * read it acquires the mutex, updates @c last_value, sets
 * @c A01NYUB_FLAG_VALID, and clears @c A01NYUB_FLAG_ERROR. On a failed
 * read it acquires the mutex and sets @c A01NYUB_FLAG_ERROR, leaving any
 * previously cached @c last_value in place. Used only when @c read_mode
 * is @c A01NYUB_READ_CONTINUOUS.
 *
 * @param arg  Pointer to the owning @ref a01nyub_t instance.
 */
static void reader_task(void *arg)
{
    a01nyub_t *dev = (a01nyub_t *)arg;
    uint16_t value;

    while (true) {
        esp_err_t ret = read_packet(dev, &value);
        xSemaphoreTake(dev->mutex, portMAX_DELAY);
        if (ret == ESP_OK) {
            dev->last_value = value;
            dev->flags |= A01NYUB_FLAG_VALID;
            dev->flags &= ~A01NYUB_FLAG_ERROR;
        } else {
            dev->flags |= A01NYUB_FLAG_ERROR;
        }
        xSemaphoreGive(dev->mutex);
    }
}

esp_err_t a01nyub_init(a01nyub_t *dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (dev->flags & A01NYUB_FLAG_INITIALIZED) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = uart_driver_install(dev->uart_port, 256, 0, 0, NULL, 0);
    if (ret != ESP_OK) return ret;

    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ret = uart_param_config(dev->uart_port, &uart_config);
    if (ret != ESP_OK) goto err_uart;

    ret = uart_set_pin(dev->uart_port, UART_PIN_NO_CHANGE, dev->rx_pin,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) goto err_uart;

    /* The TX pin doubles as a mode-select line for the sensor:
     * HIGH = filtered output, LOW = real-time output. */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << dev->tx_pin),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    ret = gpio_config(&io);
    if (ret != ESP_OK) goto err_uart;

    gpio_set_level(dev->tx_pin, dev->mode == A01NYUB_MODE_FILTERED ? 1 : 0);

    dev->mutex = xSemaphoreCreateMutex();
    if (!dev->mutex) { ret = ESP_ERR_NO_MEM; goto err_gpio; }

    if (dev->read_mode == A01NYUB_READ_CONTINUOUS) {
        BaseType_t t = xTaskCreate(reader_task, "a01nyub", 2048, dev, 5, &dev->task);
        if (t != pdPASS) {
            ret = ESP_ERR_NO_MEM;
            goto err_mutex;
        }
    }

    dev->flags |= A01NYUB_FLAG_INITIALIZED;
    ESP_LOGI(TAG, "init OK [UART%d | RX=%d | TX=%d | %s | %s]",
             dev->uart_port, dev->rx_pin, dev->tx_pin,
             dev->mode      == A01NYUB_MODE_FILTERED   ? "filtered"   : "realtime",
             dev->read_mode == A01NYUB_READ_CONTINUOUS ? "continuous" : "oneshot");
    return ESP_OK;

err_mutex:
    vSemaphoreDelete(dev->mutex);
    dev->mutex = NULL;
err_gpio:
    gpio_reset_pin(dev->tx_pin);
err_uart:
    uart_driver_delete(dev->uart_port);
    return ret;
}

esp_err_t a01nyub_get_data(a01nyub_t *dev, uint16_t *value)
{
    if (!dev || !value) return ESP_ERR_INVALID_ARG;
    if (!(dev->flags & A01NYUB_FLAG_INITIALIZED)) return ESP_ERR_INVALID_STATE;

    if (dev->read_mode == A01NYUB_READ_ONESHOT) {
        /* Hold the mutex across the UART transaction itself, not just the
         * state update: read_packet() issues multiple uart_read_bytes()
         * calls, and concurrent callers sharing one UART port would
         * otherwise interleave their byte reads and corrupt each other's
         * packets. */
        xSemaphoreTake(dev->mutex, portMAX_DELAY);
        esp_err_t ret = read_packet(dev, value);
        if (ret == ESP_OK) {
            dev->last_value = *value;
            dev->flags |= A01NYUB_FLAG_VALID;
            dev->flags &= ~A01NYUB_FLAG_ERROR;
        } else {
            dev->flags |= A01NYUB_FLAG_ERROR;
        }
        xSemaphoreGive(dev->mutex);
        return ret;
    }

    /* Continuous mode: return the last cached value under the mutex. */
    xSemaphoreTake(dev->mutex, portMAX_DELAY);
    bool valid = dev->flags & A01NYUB_FLAG_VALID;
    if (valid) *value = dev->last_value;
    xSemaphoreGive(dev->mutex);

    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void a01nyub_deinit(a01nyub_t *dev)
{
    if (!dev) return;
    if (!(dev->flags & A01NYUB_FLAG_INITIALIZED)) return;

    if (dev->task)  vTaskDelete(dev->task);
    if (dev->mutex) vSemaphoreDelete(dev->mutex);

    uart_driver_delete(dev->uart_port);
    dev->flags = 0;
}
