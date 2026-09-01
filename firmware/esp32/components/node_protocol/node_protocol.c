/**
 * @file node_protocol.c
 * @brief blue-node MQTT contract implementation.
 *
 * Everything that depends on the broker session is driven by the connection
 * handler registered with mqtt_manager: announcing state and (re)subscribing
 * happen on connect rather than on a poll from the application, so the
 * subscriptions come back after a reconnect without the app tracking it.
 * See node_protocol.h for the topic layout and the QoS/retain policy.
 */

#include "node_protocol.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "NODE_PROTOCOL";

/* Payloads for the two states this component publishes itself. Both are static
 * so the offline one can be handed to esp-mqtt as a Last Will, which keeps a
 * pointer to it for the lifetime of the client. */
static const char PAYLOAD_ONLINE[]  = "{\"status\":\"online\"}";
static const char PAYLOAD_OFFLINE[] = "{\"status\":\"offline\"}";

/** Suffix per node_topic_t, indexed by category. */
static const char *const TOPIC_SUFFIX[NODE_TOPIC_COUNT] = {
    [NODE_TOPIC_TELEMETRY] = "telemetry",
    [NODE_TOPIC_STATE]     = "state",
    [NODE_TOPIC_CONFIG]    = "config",
    [NODE_TOPIC_COMMAND]   = "command",
};

/**
 * @brief Module-level state. Single instance, mirroring mqtt_manager: not
 *        guarded by a mutex, so init/deinit must not race the mqtt event task.
 */
static struct {
    char  device_id[NODE_PROTOCOL_DEVICE_ID_SIZE];
    char  topics[NODE_TOPIC_COUNT][NODE_PROTOCOL_TOPIC_SIZE];
    bool  initialized;
    node_protocol_msg_cb_t       on_config;
    node_protocol_msg_cb_t       on_command;
    node_protocol_connected_cb_t on_connected;
    void *user_ctx;
} s_ctx = {0};

/** Empty string handed out instead of NULL by the accessors before init. */
static const char EMPTY[] = "";

/**
 * @brief Compare an incoming topic against one of ours.
 *
 * Incoming topics are not NUL-terminated, hence the explicit length check
 * before strncmp() — without it "blue-node/A4C1F8/config" would also match a
 * longer topic sharing that prefix.
 */
static bool topic_matches(const char *topic, int topic_len, node_topic_t category)
{
    const char *expected = s_ctx.topics[category];
    return (size_t)topic_len == strlen(expected) && strncmp(topic, expected, topic_len) == 0;
}

/**
 * @brief mqtt_manager data handler: route config and command to the app.
 *
 * Every mqtt_manager data handler sees every message regardless of topic, so
 * the dispatch happens here.
 */
static void on_mqtt_data(const char *topic, int topic_len,
                         const char *data, int data_len, void *user_ctx)
{
    if (topic_matches(topic, topic_len, NODE_TOPIC_CONFIG)) {
        ESP_LOGI(TAG, "config: %.*s", data_len, data);
        if (s_ctx.on_config) {
            s_ctx.on_config(data, data_len, s_ctx.user_ctx);
        }
    } else if (topic_matches(topic, topic_len, NODE_TOPIC_COMMAND)) {
        ESP_LOGI(TAG, "command: %.*s", data_len, data);
        if (s_ctx.on_command) {
            s_ctx.on_command(data, data_len, s_ctx.user_ctx);
        }
    } else {
        /* Another component's subscription, or a stale one from a previous
         * session that this device no longer expects. */
        ESP_LOGD(TAG, "Ignoring message on %.*s", topic_len, topic);
    }
}

/**
 * @brief mqtt_manager connection handler: announce and (re)subscribe on connect.
 *
 * Runs on the mqtt event task with the session already up, so publishing and
 * subscribing from here is safe. Nothing to do on disconnect: the broker
 * publishes the Last Will on the device's behalf.
 */
static void on_mqtt_connection(bool connected, void *user_ctx)
{
    if (!connected) {
        return;
    }

    esp_err_t ret = node_protocol_publish_state(PAYLOAD_ONLINE, sizeof(PAYLOAD_ONLINE) - 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish online state: %s", esp_err_to_name(ret));
    }

    /* QoS 1: a lost SUBSCRIBE would leave the device deaf to config and
     * commands until the next reconnect. */
    if (mqtt_manager_subscribe(s_ctx.topics[NODE_TOPIC_CONFIG], 1) != ESP_OK ||
        mqtt_manager_subscribe(s_ctx.topics[NODE_TOPIC_COMMAND], 1) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to config/command topics");
    }

    if (s_ctx.on_connected) {
        s_ctx.on_connected(s_ctx.user_ctx);
    }
}

/**
 * @brief Publish to one category, applying that category's QoS and retain policy.
 *
 * Skips quietly when there is no session: telemetry is published once a second,
 * and mqtt_manager_publish() logs an error on every attempt while disconnected.
 *
 * The connected check can go stale before the publish lands — the session may
 * drop in between. That costs at most one sample: esp-mqtt notices it is no
 * longer connected and drops the QoS 0 message with a warning, so there is
 * nothing here worth locking against.
 */
static esp_err_t publish(node_topic_t category, const char *json, int len, int qos, int retain)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "node_protocol_init() not called");
    ESP_RETURN_ON_FALSE(json, ESP_ERR_INVALID_ARG, TAG, "json is NULL");

    if (!mqtt_manager_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    return mqtt_manager_publish(s_ctx.topics[category], json, len, qos, retain);
}

esp_err_t node_protocol_init(const node_protocol_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(!s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "already initialized");

    const char *root = config->root ? config->root : NODE_PROTOCOL_ROOT_DEFAULT;

    /* The low three bytes of the MAC are unique enough within one deployment
     * and keep the topic short; the full MAC stays available for provisioning. */
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG, "esp_read_mac failed");
    snprintf(s_ctx.device_id, sizeof(s_ctx.device_id), "%02X%02X%02X", mac[3], mac[4], mac[5]);

    for (int i = 0; i < NODE_TOPIC_COUNT; i++) {
        int written = snprintf(s_ctx.topics[i], NODE_PROTOCOL_TOPIC_SIZE, "%s/%s/%s",
                               root, s_ctx.device_id, TOPIC_SUFFIX[i]);
        /* snprintf truncates silently; a truncated topic would publish to the
         * wrong place, so refuse to come up instead. */
        ESP_RETURN_ON_FALSE(written > 0 && written < NODE_PROTOCOL_TOPIC_SIZE,
                            ESP_ERR_INVALID_ARG, TAG, "topic root '%s' is too long", root);
    }

    s_ctx.on_config    = config->on_config;
    s_ctx.on_command   = config->on_command;
    s_ctx.on_connected = config->on_connected;
    s_ctx.user_ctx     = config->user_ctx;

    ESP_RETURN_ON_ERROR(mqtt_manager_register_data_handler(on_mqtt_data, NULL),
                        TAG, "data handler registration failed");
    esp_err_t ret = mqtt_manager_register_connection_handler(on_mqtt_connection, NULL);
    if (ret != ESP_OK) {
        mqtt_manager_unregister_data_handler(on_mqtt_data);
        ESP_RETURN_ON_ERROR(ret, TAG, "connection handler registration failed");
    }

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "Device %s, topics %s/%s/{telemetry,state,config,command}",
             s_ctx.device_id, root, s_ctx.device_id);
    return ESP_OK;
}

void node_protocol_apply_lwt(mqtt_manager_config_t *mqtt_cfg)
{
    if (!mqtt_cfg) {
        ESP_LOGE(TAG, "mqtt_cfg is NULL");
        return;
    }
    if (!s_ctx.initialized) {
        ESP_LOGW(TAG, "node_protocol_init() not called, leaving LWT unset");
        return;
    }

    mqtt_cfg->lwt_topic   = s_ctx.topics[NODE_TOPIC_STATE];
    mqtt_cfg->lwt_msg     = PAYLOAD_OFFLINE;
    mqtt_cfg->lwt_msg_len = sizeof(PAYLOAD_OFFLINE) - 1;
    mqtt_cfg->lwt_qos     = 1;
    mqtt_cfg->lwt_retain  = 1;
}

const char *node_protocol_device_id(void)
{
    return s_ctx.initialized ? s_ctx.device_id : EMPTY;
}

const char *node_protocol_topic(node_topic_t category)
{
    /* Cast: the enum's underlying type may be unsigned, where a bare
     * "category < 0" is a tautology the compiler warns about. */
    if (!s_ctx.initialized || (int)category < 0 || category >= NODE_TOPIC_COUNT) {
        return EMPTY;
    }
    return s_ctx.topics[category];
}

esp_err_t node_protocol_publish_telemetry(const char *json, int len)
{
    return publish(NODE_TOPIC_TELEMETRY, json, len, 0, 0);
}

esp_err_t node_protocol_publish_state(const char *json, int len)
{
    return publish(NODE_TOPIC_STATE, json, len, 1, 1);
}

esp_err_t node_protocol_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "node_protocol_init() not called");
    /* A started client means a live event task that can be inside on_mqtt_data()
     * right now, reading the topics and callback pointers cleared below. */
    ESP_RETURN_ON_FALSE(!mqtt_manager_is_started(), ESP_ERR_INVALID_STATE, TAG,
                        "mqtt_manager_stop() not called");

    /* Unregister before clearing, so no callback can start against state that
     * is about to go away. */
    mqtt_manager_unregister_data_handler(on_mqtt_data);
    mqtt_manager_unregister_connection_handler(on_mqtt_connection);
    memset(&s_ctx, 0, sizeof(s_ctx));
    return ESP_OK;
}
