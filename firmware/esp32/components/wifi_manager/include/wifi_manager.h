#pragma once

/**
 * @file wifi_manager.h
 * @brief Simple blocking WiFi STA connection manager for ESP-IDF.
 *
 * Wraps esp_wifi / esp_netif into a minimal API: call wifi_manager_connect()
 * once and it blocks until an IP is assigned or all retries are exhausted.
 * Designed for single-interface, single-connection use; not thread-safe across
 * concurrent calls to connect/disconnect.
 */

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Configuration passed to wifi_manager_connect().
 */
typedef struct {
    char    ssid[32];        /**< AP SSID — null-terminated, max 31 chars. */
    char    password[64];    /**< WPA2 passphrase — null-terminated, max 63 chars. Empty string = open network. */
    uint8_t max_retries;     /**< How many reconnect attempts before giving up. 0 = retry forever. */
} wifi_manager_config_t;

/**
 * @brief Connect to a WiFi AP and block until an IP is assigned.
 *
 * Initialises esp_netif, the default event loop, and the WiFi driver (only on
 * the first call — subsequent calls skip the one-time setup). Registers event
 * handlers, starts the WiFi STA, then blocks on an FreeRTOS event group until
 * either WIFI_CONNECTED_BIT or WIFI_FAILED_BIT is set.
 *
 * @param[in] config  SSID, password, and retry limit. Must not be NULL.
 *
 * @return ESP_OK           IP acquired successfully.
 * @return ESP_FAIL         Max retries exceeded without a successful connection.
 * @return ESP_ERR_INVALID_ARG  @p config is NULL.
 * @return other            Propagated from esp_wifi / esp_netif init calls.
 *
 * @note Call wifi_manager_disconnect() before calling this again; re-entrant
 *       use without a disconnect will leak event-handler registrations.
 */
esp_err_t wifi_manager_connect(const wifi_manager_config_t *config);

/**
 * @brief Disconnect from the current AP and release WiFi resources.
 *
 * Unregisters event handlers, calls esp_wifi_disconnect / stop / deinit, and
 * deletes the FreeRTOS event group. Safe to call even if not currently
 * connected; always returns ESP_OK.
 *
 * @return ESP_OK  Always.
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * @brief Return whether the station currently holds an IP address.
 *
 * Reads WIFI_CONNECTED_BIT from the internal event group without clearing it.
 * Returns false if wifi_manager_connect() has never been called.
 *
 * @return true   Connected and IP assigned.
 * @return false  Not connected or manager not initialised.
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Write the current IP address as a dotted-decimal string into @p buf.
 *
 * Retrieves ip_info from the default STA netif and formats it with snprintf.
 * Useful for logging or display without pulling in esp_netif headers at the
 * call site.
 *
 * @param[out] buf  Destination buffer; must be at least 16 bytes for a full
 *                  IPv4 string ("255.255.255.255\0").
 * @param[in]  len  Size of @p buf in bytes.
 *
 * @return ESP_OK              IP written to @p buf.
 * @return ESP_ERR_INVALID_ARG @p buf is NULL.
 * @return ESP_FAIL            Default STA netif not found (manager not started).
 * @return other               Propagated from esp_netif_get_ip_info().
 */
esp_err_t wifi_manager_get_ip(char *buf, size_t len);
