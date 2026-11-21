#pragma once

/**
 * @file node_config.h
 * @brief Persistent node configuration: what onboarding hands the node.
 *
 * Owns the one NVS namespace the application writes to ("bn_cfg") and keeps a
 * RAM copy of it, loaded once by node_config_init(). Three things live here:
 *
 *   - the WiFi credentials the node joins its network with,
 *   - the broker URI it publishes to (handed over by the backend during
 *     onboarding, so the firmware carries no broker address of its own),
 *   - a one-shot "enter pairing on next boot" request.
 *
 * A node is *provisioned* once it holds both WiFi credentials and a broker
 * URI. Until then it has nothing to publish to, so main.c boots it straight
 * into pairing mode. node_config_store() writes the three values as a single
 * NVS blob, because onboarding only ever learns them together: a node with new
 * WiFi but the old broker (or the reverse) would be half-onboarded, and a
 * single NVS entry is written atomically, so a power cut mid-store leaves
 * either the old configuration or the new one - never a mix.
 *
 * Credentials are stored in plain NVS. NVS encryption is off in this project;
 * turning it on (CONFIG_NVS_ENCRYPTION + flash encryption) protects them
 * without any change here.
 *
 * @section node_config_threading Threading
 *
 * There is no lock. node_config_init() runs once at boot before any other task
 * touches this component. node_config_store() is only called from the
 * provisioning flow, and the node restarts right after it, so no other task is
 * reading the RAM copy at the same time. node_config_get() hands out a pointer
 * to that copy: read it at boot, not concurrently with a store.
 */

#include "esp_err.h"
#include <stdbool.h>

/** Longest SSID 802.11 allows, in bytes, excluding the terminating NUL. */
#define NODE_CONFIG_SSID_MAX_LEN 32

/** Longest WPA passphrase (63) or raw PSK in hex (64), excluding the NUL. */
#define NODE_CONFIG_PASSWORD_MAX_LEN 64

/** Longest broker URI accepted, excluding the terminating NUL. */
#define NODE_CONFIG_BROKER_URI_MAX_LEN 127

/**
 * @brief The persisted configuration, as loaded into RAM.
 *
 * Every string is NUL-terminated; an empty string means "not set".
 */
typedef struct
{
    char wifi_ssid[NODE_CONFIG_SSID_MAX_LEN + 1];         /**< SSID to join. */
    char wifi_password[NODE_CONFIG_PASSWORD_MAX_LEN + 1]; /**< Its passphrase; empty = open network. */
    char broker_uri[NODE_CONFIG_BROKER_URI_MAX_LEN + 1];  /**< e.g. "mqtt://192.168.1.10:1883". */
} node_config_t;

/**
 * @brief Load the stored configuration into RAM.
 *
 * Precondition: nvs_flash_init() has already been called (main.c owns the NVS
 * partition's lifecycle, including erase-on-version-change).
 *
 * A namespace that does not exist yet - a fresh or factory-reset node - is not
 * an error: the node simply comes up unprovisioned.
 *
 * @return ESP_OK                 Loaded (possibly empty).
 * @return ESP_ERR_INVALID_STATE  Already initialised.
 * @return other                  Propagated from NVS.
 */
esp_err_t node_config_init(void);

/**
 * @brief Return the RAM copy loaded by node_config_init().
 *
 * @return Pointer to this component's copy; never NULL. All strings are empty
 *         if node_config_init() has not run or found nothing.
 */
const node_config_t *node_config_get(void);

/**
 * @brief Return whether the node holds everything it needs to run.
 *
 * @return true when an SSID and a broker URI are both stored.
 */
bool node_config_is_provisioned(void);

/**
 * @brief Persist a complete onboarding result and update the RAM copy.
 *
 * All three values are validated before anything is written, so a rejected
 * call leaves both NVS and RAM untouched. They are written as one blob, which
 * NVS commits atomically (see the file comment).
 *
 * @param[in] ssid        SSID, 1..NODE_CONFIG_SSID_MAX_LEN bytes. Must not be NULL.
 * @param[in] password    Passphrase, 0..NODE_CONFIG_PASSWORD_MAX_LEN bytes
 *                        (empty = open network). Must not be NULL.
 * @param[in] broker_uri  "mqtt://" or "mqtts://" URI, at most
 *                        NODE_CONFIG_BROKER_URI_MAX_LEN bytes. Must not be NULL.
 *
 * @return ESP_OK                 Stored and committed.
 * @return ESP_ERR_INVALID_ARG    A pointer is NULL, a length is out of range, or
 *                                 the URI scheme is not mqtt:// / mqtts://.
 * @return ESP_ERR_INVALID_STATE  node_config_init() has not been called.
 * @return other                  Propagated from NVS.
 */
esp_err_t node_config_store(const char *ssid, const char *password, const char *broker_uri);

/**
 * @brief Ask for pairing mode on the next boot.
 *
 * Pairing is always entered through a restart (see main.c), so the request
 * has to survive one: it is a flag in NVS, consumed by
 * node_config_take_pairing_request(). Stored configuration is left alone, so a
 * provisioned node that times out of pairing comes back up as it was.
 *
 * @return ESP_OK  Flag stored.
 * @return other   Propagated from NVS.
 */
esp_err_t node_config_request_pairing(void);

/**
 * @brief Read and clear the pairing request.
 *
 * Clearing on read is what keeps a pairing window that ends (timeout, power
 * loss) from turning into a pairing loop on every later boot.
 *
 * @return true if pairing was requested since the last call.
 */
bool node_config_take_pairing_request(void);

/**
 * @brief Factory reset: erase the whole default NVS partition.
 *
 * Erases more than this component's namespace on purpose. The WiFi driver
 * keeps its own copy of past credentials in NVS (from before this component
 * existed, or from any code path that left WIFI_STORAGE_FLASH on), and a
 * factory reset that left those behind would not be one. The caller is
 * expected to restart right after; NVS is unusable until nvs_flash_init()
 * runs again.
 *
 * @return ESP_OK  Partition erased.
 * @return other   Propagated from nvs_flash_deinit() / nvs_flash_erase().
 */
esp_err_t node_config_factory_reset(void);
