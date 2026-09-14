#pragma once

/**
 * @file node_protocol.h
 * @brief The blue-node MQTT application contract: topic layout and message policy.
 *
 * Sits on top of mqtt_manager the way mqtt_manager sits on top of esp-mqtt:
 * mqtt_manager owns the connection, this component owns what is said over it.
 * It derives the device identity, builds the topic for each category, applies
 * the QoS/retain policy below, and dispatches inbound messages to the
 * application.
 *
 * Topics are "<root>/<device id>/<category>", with four categories split by
 * direction and lifetime:
 *
 *   Category   | Direction        | Contents                      | QoS | Retain
 *   -----------+------------------+-------------------------------+-----+-------
 *   telemetry  | device -> backend| periodic measurement data     |  0  |  no
 *   state      | device -> backend| current device state          |  1  |  yes
 *   config     | backend -> device| persistent settings           |  1  |  yes*
 *   command    | backend -> device| one-shot actions              |  1  |  no*
 *
 * (*) Retain on the inbound topics is the publisher's choice, i.e. the
 * backend's. Config should be retained so a device that reboots or reconnects
 * is handed its settings immediately. Commands must NOT be retained: a
 * retained command is replayed to the device on every reconnect, so a single
 * "restart" would turn into a boot loop.
 *
 * Telemetry is deliberately QoS 0 and unretained: samples are frequent and
 * each one supersedes the last, so a redelivery costs more than the lost
 * sample is worth. State is QoS 1 and retained so a subscriber that connects
 * later still learns whether the device is alive.
 *
 * Liveness is reported through the state topic from both ends: this component
 * publishes "online" once the broker session is up, and the broker publishes
 * "offline" on its behalf via the Last Will and Testament configured by
 * node_protocol_apply_lwt() if the device disappears without disconnecting
 * cleanly.
 *
 * @section state_payload State payload contract
 *
 * The backend registers devices from these two messages, so their shape is an
 * interface, not an implementation detail:
 *
 *     online:  {"status":"online","mac":"AA:BB:CC:DD:EE:FF","fw":"1.0.0"}
 *     offline: {"status":"offline"}
 *
 * The online message carries the full MAC because the topic only contains its
 * low three bytes; it is what identifies the physical board for provisioning.
 * "fw" is the application version from the app descriptor (see
 * esp_app_get_description()), i.e. whatever the project version is set to.
 *
 * The offline message is published by the *broker*, not the device, so it can
 * only be a fixed string agreed on in advance - no MAC, no version. The
 * subscriber already knows which device it refers to from the topic.
 *
 * Both are retained, so a late subscriber always sees the last one.
 * node_protocol_publish_state() may add fields on top of this; keep "status"
 * in anything it publishes.
 *
 * @section threading Threading model
 *
 * Two tasks reach into this component: the application task that publishes
 * telemetry, and the esp-mqtt event task that delivers inbound messages and
 * connection transitions.
 *
 *   - node_protocol_init() / node_protocol_deinit() run on the caller's task
 *     and only while the client is stopped. init() is required to run before
 *     mqtt_manager_init(), i.e. before esp_mqtt_client_start() creates the
 *     event task, and deinit() refuses to run until mqtt_manager_stop() has
 *     retired it.
 *   - The publish helpers may be called from any task at any time.
 *   - on_config, on_command and on_connected run on the esp-mqtt event task.
 *
 * There is deliberately no mutex here. Everything init() writes — the device
 * id, the topics, the callback pointers — is written before the event task
 * exists and is only read afterwards, so there is no shared *mutable* state to
 * guard. The state that genuinely is mutable belongs to esp-mqtt, which
 * serializes it with its own recursive API lock. Adding a lock of our own
 * would also mean taking it inside a callback while that API lock is held,
 * putting every MQTT operation (keepalive included) behind whatever the
 * application task does with it.
 *
 * That recursive API lock is also why the callbacks may publish and subscribe
 * freely: they already run on the task holding it, and neither operation waits
 * for a broker acknowledgement. What they must not do is take long. The lock is
 * held for the duration of the callback, so a slow handler stalls all MQTT
 * traffic — hand anything expensive to a task of your own, and never block on a
 * lock the application task can hold for an extended period.
 */

#include "mqtt_manager.h"

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/** Topic root used when node_protocol_config_t::root is NULL. */
#define NODE_PROTOCOL_ROOT_DEFAULT "blue-node"

/** Size of the device id string, including the terminating NUL. */
#define NODE_PROTOCOL_DEVICE_ID_SIZE 7

/** Size of the MAC address string "AA:BB:CC:DD:EE:FF", including the terminating NUL. */
#define NODE_PROTOCOL_MAC_STR_SIZE 18

/** Size of the buffer each topic is built into, including the terminating NUL. */
#define NODE_PROTOCOL_TOPIC_SIZE 64

/**
 * @brief Size of the buffer the online state payload is built into.
 *
 * Fits @ref state_payload plus a firmware version of up to
 * CONFIG_APP_PROJECT_VER_LEN (32) characters, with room to spare.
 */
#define NODE_PROTOCOL_STATE_PAYLOAD_SIZE 128

/**
 * @brief The four topic categories of the blue-node contract.
 *
 * Doubles as the index into this component's topic table, so NODE_TOPIC_COUNT
 * must stay last.
 */
typedef enum {
    NODE_TOPIC_TELEMETRY,  /**< device -> backend: periodic measurement data. */
    NODE_TOPIC_STATE,      /**< device -> backend: current device state, retained. */
    NODE_TOPIC_CONFIG,     /**< backend -> device: persistent settings. */
    NODE_TOPIC_COMMAND,    /**< backend -> device: one-shot actions. */
    NODE_TOPIC_COUNT,      /**< Number of categories; not a valid category. */
} node_topic_t;

/**
 * @brief Callback invoked for an inbound config or command message.
 *
 * Runs synchronously on the esp-mqtt event task, so keep it fast: MQTT event
 * delivery is blocked while it executes, and long work (or anything that
 * publishes and waits) belongs on a task of your own.
 *
 * @p json points into the client's receive buffer, is only valid for the
 * duration of this call, and is NOT NUL-terminated — always use @p len (e.g.
 * "%.*s" when logging, or copy it out before parsing).
 *
 * @note Handing a setting to the application task is the one place this
 *       component's "no shared mutable state" property ends, so publish it
 *       deliberately: a single scalar (a measurement interval, a threshold)
 *       wants an atomic of its own, written here and read by the application
 *       task. Anything larger wants a copy-out under a short-lived lock, or a
 *       queue to that task — never a lock this callback can be made to wait on.
 *
 * @param[in] json      Message payload (not NUL-terminated).
 * @param[in] len       Payload length in bytes.
 * @param[in] user_ctx  node_protocol_config_t::user_ctx as passed to node_protocol_init().
 */
typedef void (*node_protocol_msg_cb_t)(const char *json, int len, void *user_ctx);

/**
 * @brief Callback invoked once the device is announced and subscribed.
 *
 * Runs after the online state has been published and the config and command
 * subscriptions have been requested, on every (re)connect rather than only the
 * first one. Use it to publish anything that should accompany a fresh session
 * (a richer state message, a firmware version, a config request). Same task
 * and same latency constraints as node_protocol_msg_cb_t.
 *
 * @param[in] user_ctx  node_protocol_config_t::user_ctx as passed to node_protocol_init().
 */
typedef void (*node_protocol_connected_cb_t)(void *user_ctx);

/**
 * @brief Configuration passed to node_protocol_init().
 *
 * Unlike mqtt_manager_config_t, @p root is copied into this component's own
 * topic buffers, so it does not have to outlive the call.
 */
typedef struct {
    const char *root;                            /**< Topic root, e.g. "blue-node". NULL
                                                       selects NODE_PROTOCOL_ROOT_DEFAULT. */
    node_protocol_msg_cb_t on_config;            /**< Handler for the config topic. NULL
                                                       logs incoming config and drops it. */
    node_protocol_msg_cb_t on_command;           /**< Handler for the command topic. NULL
                                                       logs incoming commands and drops them. */
    node_protocol_connected_cb_t on_connected;   /**< Called after each successful announce.
                                                       NULL to skip. */
    void *user_ctx;                              /**< Opaque pointer passed to every callback. */
} node_protocol_config_t;

/**
 * @brief Derive the device identity, build the topics, and hook into mqtt_manager.
 *
 * Reads the WiFi station MAC to form the device id, fills the topic table, and
 * registers a data handler and a connection handler with mqtt_manager. From
 * then on, connecting to the broker automatically publishes the online state
 * and subscribes to the config and command topics — including after a
 * reconnect, where the subscriptions would otherwise be lost.
 *
 * Must be called BEFORE mqtt_manager_init(): the state topic it builds is what
 * node_protocol_apply_lwt() hands to the client as the Last Will topic, and
 * esp-mqtt reads that only at init time.
 *
 * Does not require a network connection and does not talk to the broker.
 *
 * @param[in] config  Topic root and callbacks. Must not be NULL.
 *
 * @return ESP_OK                 Topics built and handlers registered.
 * @return ESP_ERR_INVALID_ARG    @p config is NULL, or @p config->root does not
 *                                 fit in NODE_PROTOCOL_TOPIC_SIZE.
 * @return ESP_ERR_INVALID_STATE  node_protocol_init() was already called.
 * @return other                  Propagated from esp_read_mac() or from
 *                                 mqtt_manager handler registration.
 */
esp_err_t node_protocol_init(const node_protocol_config_t *config);

/**
 * @brief Fill in the Last Will fields of an mqtt_manager configuration.
 *
 * Points the client's Last Will at this device's state topic with the
 * component's offline payload, retained, so the broker announces the device as
 * offline if it drops without a clean disconnect. Both pointers it writes are
 * to storage owned by this component, which satisfies mqtt_manager's
 * requirement that they outlive the client.
 *
 * Call between node_protocol_init() and mqtt_manager_init(); it leaves every
 * other field of @p mqtt_cfg untouched. A no-op (with a warning) if
 * node_protocol_init() has not run, which would otherwise arm the will with an
 * empty topic.
 *
 * @param[in,out] mqtt_cfg  Configuration to amend. Must not be NULL.
 */
void node_protocol_apply_lwt(mqtt_manager_config_t *mqtt_cfg);

/**
 * @brief Return the device id: the low three bytes of the WiFi station MAC as
 *        uppercase hex, e.g. "A4C1F8".
 *
 * @return Pointer to this component's copy, valid until node_protocol_deinit().
 *         An empty string if node_protocol_init() has not been called.
 */
const char *node_protocol_device_id(void);

/**
 * @brief Return the full WiFi station MAC as "AA:BB:CC:DD:EE:FF".
 *
 * The device id is only the low three bytes of this; the full address is what
 * identifies the board itself, and is what the online state message carries to
 * the backend (see @ref state_payload).
 *
 * @return Pointer to this component's copy, valid until node_protocol_deinit().
 *         An empty string if node_protocol_init() has not been called.
 */
const char *node_protocol_mac(void);

/**
 * @brief Return the fully-qualified topic for one category.
 *
 * @param[in] category  Category to look up.
 *
 * @return Pointer to this component's copy, valid until node_protocol_deinit().
 *         An empty string if @p category is out of range or node_protocol_init()
 *         has not been called.
 */
const char *node_protocol_topic(node_topic_t category);

/**
 * @brief Publish a measurement to the telemetry topic (QoS 0, not retained).
 *
 * @param[in] json  Complete JSON object, e.g. "{\"distance_mm\":842}". Must not be NULL.
 * @param[in] len   Payload length in bytes. 0 lets esp-mqtt use strlen(json).
 *
 * @return ESP_OK                 Message handed to the client.
 * @return ESP_ERR_INVALID_STATE  Not initialized, or no broker session — samples
 *                                 taken while offline are dropped rather than queued.
 * @return other                  Propagated from mqtt_manager_publish().
 */
esp_err_t node_protocol_publish_telemetry(const char *json, int len);

/**
 * @brief Publish device state to the state topic (QoS 1, retained).
 *
 * The online/offline status is already handled for you — this is for
 * additional state (uptime, RSSI, firmware version, applied config). Retained,
 * so each call replaces what a late subscriber will receive.
 *
 * @param[in] json  Complete JSON object, e.g. "{\"status\":\"online\",\"rssi\":-61}".
 *                   Must not be NULL.
 * @param[in] len   Payload length in bytes. 0 lets esp-mqtt use strlen(json).
 *
 * @return ESP_OK                 Message handed to the client.
 * @return ESP_ERR_INVALID_STATE  Not initialized, or no broker session.
 * @return other                  Propagated from mqtt_manager_publish().
 */
esp_err_t node_protocol_publish_state(const char *json, int len);

/**
 * @brief Unhook from mqtt_manager and clear all module state.
 *
 * Requires the client to be stopped: while it is started, the esp-mqtt event
 * task may be inside a callback reading the very state this clears, and no
 * lock stands between the two (see @ref threading). Call mqtt_manager_stop()
 * first.
 *
 * Does not publish a farewell state message: call node_protocol_publish_state()
 * before mqtt_manager_stop() if the backend should see a clean shutdown, since
 * a clean disconnect suppresses the Last Will.
 *
 * @return ESP_OK                 Handlers removed and state cleared.
 * @return ESP_ERR_INVALID_STATE  node_protocol_init() has not been called, or
 *                                 the client is still started — call
 *                                 mqtt_manager_stop() first.
 */
esp_err_t node_protocol_deinit(void);
