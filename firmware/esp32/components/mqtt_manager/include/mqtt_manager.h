#pragma once

/**
 * @file mqtt_manager.h
 * @brief Thin wrapper around ESP-IDF's built-in esp-mqtt client.
 *
 * Owns exactly one esp_mqtt_client_handle_t for the lifetime of the
 * application: mqtt_manager_init() creates and configures it,
 * mqtt_manager_start()/mqtt_manager_stop() start and stop the underlying
 * network session, and publish/subscribe/unsubscribe operate on it. Does not
 * own NVS, netif, the default event loop, or WiFi — see the preconditions on
 * mqtt_manager_init() and mqtt_manager_start() below.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/** Maximum number of independent components that can register a data callback. */
#define MQTT_MANAGER_MAX_DATA_HANDLERS 4

/** Maximum number of independent components that can register a connection callback. */
#define MQTT_MANAGER_MAX_CONN_HANDLERS 4

/**
 * @brief Callback invoked for every incoming MQTT_EVENT_DATA message.
 *
 * Called synchronously, from the esp-mqtt client's internal event task, once
 * per registered handler. @p topic and @p data point into the client's
 * internal receive buffer and are only valid for the duration of this call —
 * copy anything you need to keep past it. Neither buffer is null-terminated;
 * always use the accompanying length. Keep the handler fast: it runs on the
 * mqtt task and blocks delivery of subsequent events while it executes.
 *
 * @note Large or fragmented payloads (bigger than the client's internal
 *       buffer) arrive as multiple MQTT_EVENT_DATA events for the same
 *       message; this simple wrapper forwards each fragment as-is rather
 *       than reassembling them. Size the client buffer (menuconfig or the
 *       esp-mqtt default) to comfortably fit your largest expected payload
 *       if you need single-callback delivery.
 *
 * @param[in] topic      Pointer to the topic string (not null-terminated).
 * @param[in] topic_len  Length of @p topic in bytes.
 * @param[in] data       Pointer to the payload (not null-terminated).
 * @param[in] data_len   Length of @p data in bytes.
 * @param[in] user_ctx   Opaque context passed to mqtt_manager_register_data_handler().
 */
typedef void (*mqtt_manager_data_cb_t)(const char *topic, int topic_len,
                                       const char *data, int data_len,
                                       void *user_ctx);

/**
 * @brief Callback invoked whenever the broker session comes up or goes down.
 *
 * Called synchronously, from the esp-mqtt client's internal event task, on
 * MQTT_EVENT_CONNECTED (@p connected true) and MQTT_EVENT_DISCONNECTED
 * (@p connected false), once per registered handler. Fires on every reconnect,
 * not just the first connection — subscriptions do not survive a clean-session
 * reconnect, so re-subscribing from here is the intended pattern.
 *
 * mqtt_manager_is_connected() already reflects the new state by the time this
 * runs, so a handler may call mqtt_manager_publish() / _subscribe() directly.
 * Keep the handler fast: it runs on the mqtt task and blocks delivery of
 * subsequent events while it executes.
 *
 * @param[in] connected  true on connect, false on disconnect.
 * @param[in] user_ctx   Opaque context passed to mqtt_manager_register_connection_handler().
 */
typedef void (*mqtt_manager_conn_cb_t)(bool connected, void *user_ctx);

/**
 * @brief Configuration passed to mqtt_manager_init().
 *
 * All pointer fields are handed directly to esp-mqtt and are not copied, so
 * they must remain valid for as long as the client exists (string literals
 * or static/global buffers are fine; stack buffers are not).
 */
typedef struct
{
    const char *broker_uri;  /**< e.g. "mqtts://broker.example.com:8883" or
                                   "mqtt://broker.example.com:1883". Required. */
    const char *client_id;   /**< MQTT client identifier. NULL lets esp-mqtt
                                   auto-generate one ("ESP32_" + random suffix). */
    const char *username;    /**< Optional MQTT username. NULL to omit. */
    const char *password;    /**< Optional MQTT password. NULL to omit; ignored
                                   if username is NULL (MQTT requires a username
                                   to send a password). */
    const char *ca_cert_pem; /**< Optional PEM-encoded CA certificate buffer used
                                   to verify the broker's TLS certificate, e.g. a
                                   buffer embedded via EMBED_TXTFILES in
                                   CMakeLists.txt:
                                       idf_component_register(...
                                           EMBED_TXTFILES "certs/ca_cert.pem")
                                   and referenced as:
                                       extern const uint8_t ca_cert_pem_start[]
                                           asm("_binary_ca_cert_pem_start");
                                       extern const uint8_t ca_cert_pem_end[]
                                           asm("_binary_ca_cert_pem_end");
                                   NULL = no explicit CA cert (plain mqtt://, or
                                   mqtts:// relying on a certificate bundle
                                   enabled elsewhere in menuconfig). */
    size_t ca_cert_len;      /**< Length of ca_cert_pem in bytes, including the
                                   terminating NUL (EMBED_TXTFILES-embedded files
                                   include it, so passing
                                   ca_cert_pem_end - ca_cert_pem_start is
                                   correct). 0 lets esp-mqtt treat ca_cert_pem as
                                   a plain NUL-terminated string instead. Ignored
                                   if ca_cert_pem is NULL. */
    const char *lwt_topic;   /**< Optional Last Will and Testament topic. The broker
                                   publishes lwt_msg here if the client drops without
                                   a clean disconnect (power loss, lost network),
                                   which is what lets subscribers distinguish a dead
                                   device from an idle one. NULL disables LWT, and
                                   then every other lwt_* field is ignored. */
    const char *lwt_msg;     /**< LWT payload. Like the fields above, this is handed
                                   to esp-mqtt uncopied and must outlive the client. */
    int lwt_msg_len;         /**< Length of lwt_msg in bytes. 0 lets esp-mqtt compute
                                   it with strlen(lwt_msg) — only valid if lwt_msg is
                                   NUL-terminated. */
    int lwt_qos;             /**< LWT QoS level: 0, 1, or 2. */
    int lwt_retain;          /**< Non-zero to set the MQTT retain flag on the LWT
                                   message, so late subscribers still see it. */
} mqtt_manager_config_t;

/**
 * @brief Create and configure the underlying esp-mqtt client.
 *
 * Preconditions (owned by the caller, not by mqtt_manager):
 *   - nvs_flash_init() has already been called.
 *   - esp_netif_init() has already been called.
 *   - esp_event_loop_create_default() has already been called.
 *
 * Does not connect to the broker and does not require network connectivity
 * yet — that happens in mqtt_manager_start(). May be called once after
 * startup, or again after a matching mqtt_manager_deinit(). Not thread-safe
 * against concurrent callers.
 *
 * @param[in] config  Broker URI, credentials, and optional CA cert. Must not
 *                     be NULL, and config->broker_uri must not be NULL. See
 *                     mqtt_manager_config_t for pointer-lifetime requirements.
 *
 * @return ESP_OK                 Client created and configured.
 * @return ESP_ERR_INVALID_ARG    @p config or config->broker_uri is NULL.
 * @return ESP_ERR_INVALID_STATE  mqtt_manager_init() was already called.
 * @return ESP_FAIL               esp_mqtt_client_init() returned NULL, or event
 *                                 handler registration failed.
 */
esp_err_t mqtt_manager_init(const mqtt_manager_config_t *config);

/**
 * @brief Start the MQTT client and begin connecting to the broker.
 *
 * Preconditions:
 *   - mqtt_manager_init() has already been called.
 *   - IP network connectivity already exists. mqtt_manager does not manage
 *     WiFi or any other interface — bring up wifi_manager (or Ethernet,
 *     etc.) and wait for an IP address before calling this.
 *
 * Asynchronous: returns once the client task has been started, not once a
 * broker session is established. Poll mqtt_manager_is_connected() or
 * register a data handler / watch logs for connection state.
 *
 * @return ESP_OK                 esp_mqtt_client_start() succeeded.
 * @return ESP_ERR_INVALID_STATE  mqtt_manager_init() has not been called, or
 *                                 the client is already started.
 * @return other                  Propagated from esp_mqtt_client_start().
 */
esp_err_t mqtt_manager_start(void);

/**
 * @brief Stop the MQTT client and close the connection to the broker.
 *
 * The underlying client handle is kept (not destroyed), so
 * mqtt_manager_start() may be called again later to reconnect.
 *
 * @return ESP_OK                 esp_mqtt_client_stop() succeeded.
 * @return ESP_ERR_INVALID_STATE  mqtt_manager_init() has not been called.
 * @return other                  Propagated from esp_mqtt_client_stop().
 */
esp_err_t mqtt_manager_stop(void);

/**
 * @brief Destroy the underlying esp-mqtt client and reset all module state.
 *
 * After this returns (successfully), mqtt_manager_init() may be called again
 * to create a fresh client.
 *
 * @return ESP_OK                 Client destroyed and state cleared.
 * @return ESP_ERR_INVALID_STATE  mqtt_manager_init() has not been called, or
 *                                 the client is still started — call
 *                                 mqtt_manager_stop() first.
 * @return other                  Propagated from esp_mqtt_client_destroy().
 */
esp_err_t mqtt_manager_deinit(void);

/**
 * @brief Publish a message.
 *
 * Requires a live broker session: this call fails with
 * ESP_ERR_INVALID_STATE if not currently connected, rather than silently
 * queuing or dropping the message.
 *
 * @param[in] topic   Topic to publish to. Must not be NULL.
 * @param[in] data    Payload buffer. May be NULL only if @p len is 0.
 * @param[in] len     Payload length in bytes. 0 lets esp-mqtt compute it with
 *                     strlen(data) — only valid if @p data is NUL-terminated.
 * @param[in] qos     QoS level: 0, 1, or 2.
 * @param[in] retain  Non-zero to set the MQTT retain flag.
 *
 * @return ESP_OK                 Message queued (or sent, for qos 0).
 * @return ESP_ERR_INVALID_STATE  mqtt_manager_init() has not been called, or
 *                                 the client is not currently connected.
 * @return ESP_ERR_INVALID_ARG    @p topic is NULL.
 * @return ESP_FAIL               esp_mqtt_client_publish() failed.
 */
esp_err_t mqtt_manager_publish(const char *topic, const char *data, int len, int qos, int retain);

/**
 * @brief Subscribe to a topic (may include MQTT wildcards, e.g. "a/+/c" or "a/#").
 *
 * @param[in] topic  Topic filter to subscribe to. Must not be NULL.
 * @param[in] qos    Requested QoS level: 0, 1, or 2.
 *
 * @return ESP_OK                 Subscribe request sent.
 * @return ESP_ERR_INVALID_STATE  mqtt_manager_init() has not been called.
 * @return ESP_ERR_INVALID_ARG    @p topic is NULL.
 * @return ESP_FAIL               esp_mqtt_client_subscribe() failed.
 */
esp_err_t mqtt_manager_subscribe(const char *topic, int qos);

/**
 * @brief Unsubscribe from a topic previously passed to mqtt_manager_subscribe().
 *
 * @param[in] topic  Topic filter to unsubscribe from. Must not be NULL.
 *
 * @return ESP_OK                 Unsubscribe request sent.
 * @return ESP_ERR_INVALID_STATE  mqtt_manager_init() has not been called.
 * @return ESP_ERR_INVALID_ARG    @p topic is NULL.
 * @return ESP_FAIL               esp_mqtt_client_unsubscribe() failed.
 */
esp_err_t mqtt_manager_unsubscribe(const char *topic);

/**
 * @brief Return whether the client currently holds a live broker session.
 *
 * Reflects the most recent MQTT_EVENT_CONNECTED / MQTT_EVENT_DISCONNECTED
 * (or _ERROR) event; not a live round-trip check.
 *
 * @return true   Connected (MQTT_EVENT_CONNECTED seen, no disconnect since).
 * @return false  Not connected, or mqtt_manager_init() has never been called.
 */
bool mqtt_manager_is_connected(void);

/**
 * @brief Return whether the client is started, regardless of session state.
 *
 * True between a successful mqtt_manager_start() and mqtt_manager_stop(). This
 * is a different question from mqtt_manager_is_connected(): a started client
 * that has lost its session is still running its event task and may invoke
 * registered handlers at any moment, which is what makes this the right check
 * before tearing down anything those handlers touch.
 *
 * @return true   Started (start() succeeded, no stop() since).
 * @return false  Not started, or mqtt_manager_init() has never been called.
 */
bool mqtt_manager_is_started(void);

/**
 * @brief Register a callback to receive incoming MQTT_EVENT_DATA messages.
 *
 * Up to MQTT_MANAGER_MAX_DATA_HANDLERS callbacks may be registered
 * simultaneously (e.g. one per subscribing component); every registered
 * callback is invoked for every incoming message regardless of topic, so
 * each handler is responsible for filtering by topic if needed.
 *
 * The handler table is read by the mqtt event task without a lock, so mutate
 * it only while that task cannot be dispatching: register before
 * mqtt_manager_start() and unregister only after mqtt_manager_stop().
 *
 * @param[in] cb        Callback to invoke. Must not be NULL.
 * @param[in] user_ctx  Opaque pointer passed back to @p cb on every call.
 *
 * @return ESP_OK               Callback registered.
 * @return ESP_ERR_INVALID_ARG  @p cb is NULL.
 * @return ESP_ERR_NO_MEM       All MQTT_MANAGER_MAX_DATA_HANDLERS slots are in use.
 */
esp_err_t mqtt_manager_register_data_handler(mqtt_manager_data_cb_t cb, void *user_ctx);

/**
 * @brief Unregister a previously registered data callback.
 *
 * @param[in] cb  Callback previously passed to mqtt_manager_register_data_handler().
 *
 * @return ESP_OK                 Callback removed.
 * @return ESP_ERR_INVALID_ARG    @p cb is NULL.
 * @return ESP_ERR_NOT_FOUND      @p cb was not registered.
 */
esp_err_t mqtt_manager_unregister_data_handler(mqtt_manager_data_cb_t cb);

/**
 * @brief Register a callback to be notified of connect/disconnect transitions.
 *
 * Up to MQTT_MANAGER_MAX_CONN_HANDLERS callbacks may be registered
 * simultaneously; every registered callback is invoked on every transition, in
 * registration order. May be called before mqtt_manager_init(), which is the
 * usual ordering when the handler's own state (e.g. topic strings) has to exist
 * before the client is configured.
 *
 * The handler table is read by the mqtt event task without a lock, so mutate
 * it only while that task cannot be dispatching: register before
 * mqtt_manager_start() and unregister only after mqtt_manager_stop().
 *
 * @param[in] cb        Callback to invoke. Must not be NULL.
 * @param[in] user_ctx  Opaque pointer passed back to @p cb on every call.
 *
 * @return ESP_OK               Callback registered.
 * @return ESP_ERR_INVALID_ARG  @p cb is NULL.
 * @return ESP_ERR_NO_MEM       All MQTT_MANAGER_MAX_CONN_HANDLERS slots are in use.
 */
esp_err_t mqtt_manager_register_connection_handler(mqtt_manager_conn_cb_t cb, void *user_ctx);

/**
 * @brief Unregister a previously registered connection callback.
 *
 * @param[in] cb  Callback previously passed to mqtt_manager_register_connection_handler().
 *
 * @return ESP_OK                 Callback removed.
 * @return ESP_ERR_INVALID_ARG    @p cb is NULL.
 * @return ESP_ERR_NOT_FOUND      @p cb was not registered.
 */
esp_err_t mqtt_manager_unregister_connection_handler(mqtt_manager_conn_cb_t cb);
