#pragma once

/**
 * @file a01nyub.h
 * @brief Driver for the A01NYUB waterproof ultrasonic distance sensor.
 *
 * The sensor transmits distance readings over UART at 9600 baud as 4-byte
 * packets: [0xFF header | DATA_H | DATA_L | checksum]. Distance is reported
 * in millimetres (range: 28–4500 mm).
 *
 * The TX pin controls the output mode: pull HIGH for filtered output (sensor
 * averages readings internally) or LOW for real-time output.
 *
 * Two read modes are supported:
 *  - @ref A01NYUB_READ_ONESHOT    – caller blocks in @ref a01nyub_get_data.
 *  - @ref A01NYUB_READ_CONTINUOUS – a background FreeRTOS task keeps
 *    @ref a01nyub_t::last_value up-to-date; @ref a01nyub_get_data returns
 *    the most recent measurement without blocking the UART.
 */

#include <stdint.h>
#include "esp_err.h"
#include "driver/uart.h"
#include "sdkconfig.h"

/**
 * @brief Runtime state flags stored in @ref a01nyub_t::flags.
 */
typedef enum {
    A01NYUB_FLAG_INITIALIZED = (1 << 0), /**< Driver has been initialised successfully. */
    A01NYUB_FLAG_VALID       = (1 << 1), /**< At least one valid reading has been received. */
    A01NYUB_FLAG_ERROR       = (1 << 2), /**< Last read attempt failed (timeout or bad checksum). */
} a01nyub_flags_t;

/**
 * @brief Selects how readings are collected.
 */
typedef enum {
    A01NYUB_READ_ONESHOT,     /**< Read one packet synchronously on each @ref a01nyub_get_data call. */
    A01NYUB_READ_CONTINUOUS,  /**< Background task reads continuously; @ref a01nyub_get_data returns cached value. */
} a01nyub_read_mode_t;

/**
 * @brief Selects the sensor output mode via the TX pin level.
 */
typedef enum {
    A01NYUB_MODE_REALTIME, /**< TX pin LOW  – sensor streams raw readings. */
    A01NYUB_MODE_FILTERED, /**< TX pin HIGH – sensor streams filtered (averaged) readings. */
} a01nyub_mode_t;

/**
 * @brief Driver instance handle.
 *
 * Caller must populate @c uart_port, @c mode, @c read_mode, @c rx_pin, and
 * @c tx_pin before passing the struct to @ref a01nyub_init. All other fields
 * are managed by the driver.
 */
typedef struct {
    uart_port_t         uart_port;  /**< UART peripheral to use (e.g. UART_NUM_1). */
    a01nyub_mode_t      mode;       /**< Sensor output mode (realtime / filtered). */
    a01nyub_read_mode_t read_mode;  /**< Collection strategy (oneshot / continuous). */
    int                 rx_pin;     /**< GPIO number wired to the sensor TX line. */
    int                 tx_pin;     /**< GPIO number wired to the sensor RX / mode line. */
    uint16_t            last_value; /**< Most recent valid distance reading in mm. */
    uint8_t             flags;      /**< Bitmask of @ref a01nyub_flags_t values. */
    TaskHandle_t        task;       /**< Background reader task (continuous mode only). */
    SemaphoreHandle_t   mutex;      /**< Guards access to @c last_value and @c flags. */
} a01nyub_t;

/**
 * @brief Populate an @ref a01nyub_t with Kconfig defaults.
 *
 * Expands to a designated-initialiser list for @ref a01nyub_t. Use it when
 * declaring an instance:
 * @code
 *   a01nyub_t sensor = A01NYUB_DEFAULT();
 * @endcode
 *
 * Sets the following fields from sdkconfig:
 *  - @c uart_port  ← @c CONFIG_A01NYUB_DEFAULT_UART_PORT
 *  - @c rx_pin     ← @c CONFIG_A01NYUB_DEFAULT_RX_PIN
 *  - @c tx_pin     ← @c CONFIG_A01NYUB_DEFAULT_TX_PIN
 *  - @c mode       ← @c CONFIG_A01NYUB_DEFAULT_MODE_FILTERED
 *                     (filtered or realtime)
 *  - @c read_mode  ← @c CONFIG_A01NYUB_DEFAULT_READ_CONTINUOUS
 *                     (continuous or oneshot)
 *
 * Runtime fields (@c last_value, @c flags, @c task, @c mutex) are left
 * zero-initialised and are set up by @ref a01nyub_init.
 */
#define A01NYUB_DEFAULT() {                                             \
    .uart_port = CONFIG_A01NYUB_DEFAULT_UART_PORT,                      \
    .rx_pin    = CONFIG_A01NYUB_DEFAULT_RX_PIN,                         \
    .tx_pin    = CONFIG_A01NYUB_DEFAULT_TX_PIN,                         \
    .mode      = CONFIG_A01NYUB_DEFAULT_MODE_FILTERED                   \
                     ? A01NYUB_MODE_FILTERED : A01NYUB_MODE_REALTIME,   \
    .read_mode = CONFIG_A01NYUB_DEFAULT_READ_CONTINUOUS                  \
                     ? A01NYUB_READ_CONTINUOUS : A01NYUB_READ_ONESHOT,   \
}

/**
 * @brief Initialise the driver and start the sensor.
 *
 * Installs the UART driver, configures the UART peripheral (9600 8N1), sets
 * the TX pin to the level that corresponds to @p dev->mode, and — in
 * continuous mode — creates the background reader task and its mutex.
 *
 * @param dev  Pointer to a caller-allocated and pre-configured instance.
 *             Must not be NULL.
 *
 * @return
 *  - @c ESP_OK               on success.
 *  - @c ESP_ERR_INVALID_ARG  if @p dev is NULL.
 *  - @c ESP_ERR_INVALID_STATE if the driver is already initialised.
 *  - @c ESP_ERR_NO_MEM       if mutex or task creation fails.
 *  - Other @c esp_err_t codes propagated from the UART or GPIO drivers.
 */
esp_err_t a01nyub_init(a01nyub_t *dev);

/**
 * @brief Read the current distance measurement in millimetres.
 *
 * **Oneshot mode** – blocks until a complete, valid UART packet is received
 * (up to 1 s timeout) and writes the distance into @p value.
 *
 * **Continuous mode** – returns the latest value cached by the background
 * task without blocking the UART. Returns @c ESP_ERR_INVALID_STATE if no
 * valid reading has arrived yet.
 *
 * @param dev    Pointer to an initialised driver instance.
 * @param value  Output pointer for the distance in mm.
 *
 * @return
 *  - @c ESP_OK               on success; @p value is valid.
 *  - @c ESP_ERR_INVALID_ARG  if @p dev or @p value is NULL.
 *  - @c ESP_ERR_INVALID_STATE if the driver is not initialised, or
 *    (continuous mode) no reading has been received yet.
 *  - @c ESP_ERR_TIMEOUT      (oneshot only) if no byte arrived within 1 s.
 *  - @c ESP_FAIL             (oneshot only) if the packet checksum is wrong.
 */
esp_err_t a01nyub_get_data(a01nyub_t *dev, uint16_t *value);

/**
 * @brief Stop the sensor and release all driver resources.
 *
 * Deletes the background task and mutex (if present), uninstalls the UART
 * driver, and clears the flags so the instance can be re-initialised.
 * Safe to call on a NULL or uninitialised instance (no-op).
 *
 * @param dev  Pointer to the driver instance to tear down.
 */
void a01nyub_deinit(a01nyub_t *dev);
