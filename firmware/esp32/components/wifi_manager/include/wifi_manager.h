#pragma once

/**
 * @file wifi_manager.h
 * @brief Simple WiFi STA connection manager for ESP-IDF.
 *
 * Wraps esp_wifi / esp_netif into a minimal API: call wifi_manager_connect()
 * once and it blocks until an IP is assigned or the initial retries are
 * exhausted. After that the connection is kept up on its own: every later
 * disconnect is retried indefinitely, with exponential backoff (1 s doubling
 * to 60 s), so a router reboot or a long outage never leaves the node offline
 * until its next reset.
 *
 * Credentials are handed in by the caller (main.c reads them from
 * node_config) and are kept in RAM only - the driver's own NVS copy is
 * disabled, so node_config stays the single place they are stored.
 *
 * Designed for single-interface, single-connection use; not thread-safe across
 * concurrent calls to connect/disconnect.
 */

#include "esp_err.h"
#include "esp_wifi_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Configuration passed to wifi_manager_connect().
 */
typedef struct
{
    char ssid[33];       /**< AP SSID — null-terminated, max 32 chars. */
    char password[65];   /**< WPA2 passphrase (max 63 chars) or 64-char hex PSK, null-terminated.
                              Empty string = open network. */
    uint8_t max_retries; /**< Reconnect attempts before wifi_manager_connect() gives up on the
                              *initial* connection. 0 = keep trying forever. Once connected, any
                              later disconnect is retried forever regardless. */
} wifi_manager_config_t;

/**
 * @brief Build the station configuration the node joins its network with.
 *
 * The single place the node's WiFi policy lives. wifi_manager_connect() uses it
 * for normal operation and the provisioning component uses it to test the
 * credentials a phone hands over, so a network that passes the pairing test is
 * one the node can also join after the restart. (Two hand-built configurations
 * drifted apart before: the test accepted WPA1-only networks that the running
 * node then never joined.)
 *
 * Policy:
 *   - with a password, the weakest accepted security is WPA2-PSK (WPA3 and
 *     WPA2/WPA3 transition networks qualify). This is also what the driver
 *     defaults to for a passphrase of 8+ characters; spelling it out keeps a
 *     64-character hex PSK under the same rule and refuses an open or WEP
 *     network that happens to carry the same name;
 *   - without a password, any network of that name is accepted (open).
 *
 * The SSID and password are copied without a terminating NUL when they fill
 * the driver's fields exactly (32-byte SSID, 64-character PSK), which is what
 * the driver expects.
 *
 * @param[out] out       Overwritten completely. Must not be NULL.
 * @param[in]  ssid      1..32 bytes, NUL-terminated. Must not be NULL.
 * @param[in]  password  0..64 bytes, NUL-terminated; empty = open network.
 *                       Must not be NULL.
 *
 * @return ESP_OK               @p out is filled in.
 * @return ESP_ERR_INVALID_ARG  A NULL argument or a length out of range; @p out
 *                              is left untouched.
 */
esp_err_t wifi_manager_fill_sta_config(wifi_config_t *out, const char *ssid, const char *password);

/**
 * @brief Connect to a WiFi AP and block until an IP is assigned.
 *
 * Preconditions (owned by the caller, as for mqtt_manager):
 *   - nvs_flash_init() has been called (the WiFi driver needs NVS for its
 *     calibration data even with credential storage off);
 *   - esp_netif_init() and esp_event_loop_create_default() have been called.
 *
 * Creates the default STA netif (on the first call only), initialises the WiFi
 * driver, registers the event handlers and starts the STA, then blocks until
 * either an IP is assigned or max_retries consecutive attempts have failed.
 *
 * @param[in] config  SSID, password, and retry limit. Must not be NULL.
 *
 * @return ESP_OK           IP acquired successfully.
 * @return ESP_FAIL         max_retries exceeded without a successful connection.
 * @return ESP_ERR_INVALID_ARG  @p config is NULL.
 * @return ESP_ERR_NO_MEM   Event group or reconnect timer could not be created.
 * @return other            Propagated from esp_wifi / esp_netif / esp_event calls.
 *
 * @note Call wifi_manager_disconnect() before calling this again; re-entrant
 *       use without a disconnect will leak event-handler registrations.
 */
esp_err_t wifi_manager_connect(const wifi_manager_config_t *config);

/**
 * @brief Disconnect from the current AP and release WiFi resources.
 *
 * Stops the reconnect logic first, so the deliberate disconnect is not
 * mistaken for a dropped link, then unregisters the event handlers, calls
 * esp_wifi_disconnect / stop / deinit, and deletes the event group. Safe to
 * call even if not currently connected; always returns ESP_OK.
 *
 * @return ESP_OK  Always.
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * @brief Return whether the station currently holds an IP address.
 *
 * Cleared on every disconnect and set again once a new IP is assigned, so it
 * reflects the link as it is now, not whether it was ever up.
 *
 * @return true   Connected and IP assigned.
 * @return false  Not connected, reconnecting, or manager not initialised.
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
