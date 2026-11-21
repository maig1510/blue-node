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

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "NODE_PROTOCOL";

/* The offline payload is what the broker publishes on the device's behalf, so
 * it has to be a fixed string: esp-mqtt keeps this pointer for the lifetime of
 * the client, and the device is by definition not around to fill anything in.
 * The online payload carries the MAC and firmware version and is built once in
 * init() - see the state payload contract in node_protocol.h. */
static const char PAYLOAD_OFFLINE[] = "{\"status\":\"offline\"}";

/** Suffix per node_topic_t, indexed by category. */
static const char *const TOPIC_SUFFIX[NODE_TOPIC_COUNT] = {
    [NODE_TOPIC_TELEMETRY] = "telemetry",
    [NODE_TOPIC_STATE] = "state",
    [NODE_TOPIC_CONFIG] = "config",
    [NODE_TOPIC_COMMAND] = "command",
};

/**
 * @brief Module-level state. Single instance, mirroring mqtt_manager: not
 *        guarded by a mutex, so init/deinit must not race the mqtt event task.
 */
static struct
{
    char device_id[NODE_PROTOCOL_DEVICE_ID_SIZE];
    char mac_str[NODE_PROTOCOL_MAC_STR_SIZE];
    char topics[NODE_TOPIC_COUNT][NODE_PROTOCOL_TOPIC_SIZE];
    /* Built once in init() and only read afterwards, like the topics: the
     * component's lock-free threading model (see node_protocol.h) rests on
     * nothing here changing once the mqtt event task exists. */
    char online_payload[NODE_PROTOCOL_STATE_PAYLOAD_SIZE];
    int online_payload_len;
    bool initialized;
    node_protocol_settings_cb_t on_settings;
    node_protocol_msg_cb_t on_command;
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
 * @brief Parse a config payload into settings, see @ref config_payload.
 *
 * @return false when there is nothing to hand on: a zero-length payload (the
 *         retained config was cleared) or one that is not a JSON object.
 */
static bool parse_settings(const char *data, int len, node_protocol_settings_t *out)
{
    if (len <= 0)
    {
        ESP_LOGD(TAG, "config cleared");
        return false;
    }

    cJSON *json = cJSON_ParseWithLength(data, (size_t)len);
    if (!cJSON_IsObject(json))
    {
        ESP_LOGW(TAG, "Ignoring config that is not a JSON object: %.*s", len, data);
        cJSON_Delete(json);
        return false;
    }

    /* Defaults first: a field the payload does not carry keeps them. */
    *out = (node_protocol_settings_t){
        .registered = true,
    };
    const cJSON *registered = cJSON_GetObjectItemCaseSensitive(json, "registered");
    if (cJSON_IsBool(registered))
    {
        out->registered = cJSON_IsTrue(registered);
    }

    cJSON_Delete(json);
    return true;
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
    if (topic_matches(topic, topic_len, NODE_TOPIC_CONFIG))
    {
        ESP_LOGI(TAG, "config: %.*s", data_len, data);
        node_protocol_settings_t settings;
        if (s_ctx.on_settings && parse_settings(data, data_len, &settings))
        {
            s_ctx.on_settings(&settings, s_ctx.user_ctx);
        }
    }
    else if (topic_matches(topic, topic_len, NODE_TOPIC_COMMAND))
    {
        ESP_LOGI(TAG, "command: %.*s", data_len, data);
        if (s_ctx.on_command)
        {
            s_ctx.on_command(data, data_len, s_ctx.user_ctx);
        }
    }
    else
    {
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
    if (!connected)
    {
        return;
    }

    esp_err_t ret = node_protocol_publish_state(s_ctx.online_payload, s_ctx.online_payload_len);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to publish online state: %s", esp_err_to_name(ret));
    }

    /* QoS 1: a lost SUBSCRIBE would leave the device deaf to config and
     * commands until the next reconnect. */
    if (mqtt_manager_subscribe(s_ctx.topics[NODE_TOPIC_CONFIG], 1) != ESP_OK ||
        mqtt_manager_subscribe(s_ctx.topics[NODE_TOPIC_COMMAND], 1) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to subscribe to config/command topics");
    }

    if (s_ctx.on_connected)
    {
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

    if (!mqtt_manager_is_connected())
    {
        return ESP_ERR_INVALID_STATE;
    }

    return mqtt_manager_publish(s_ctx.topics[category], json, len, qos, retain);
}

esp_err_t node_protocol_init(const node_protocol_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(!s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "already initialized");

    const char *root = config->root ? config->root : NODE_PROTOCOL_ROOT_DEFAULT;

    /* The full MAC, not a shortened form: the id has to be unique across every
     * node ever registered with the backend, which derives its node_id column
     * from the MAC registered during onboarding (see backend init.sql). */
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG, "esp_read_mac failed");
    snprintf(s_ctx.device_id, sizeof(s_ctx.device_id), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_ctx.mac_str, sizeof(s_ctx.mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Built here rather than per connect: it never changes, and on_mqtt_connection()
     * runs on the mqtt event task where the less work the better. */
    const esp_app_desc_t *app_desc = esp_app_get_description();
    int payload_len = snprintf(s_ctx.online_payload, sizeof(s_ctx.online_payload),
                               "{\"status\":\"online\",\"mac\":\"%s\",\"fw\":\"%s\"}",
                               s_ctx.mac_str, app_desc->version);
    /* A truncated payload is invalid JSON the backend would drop on every
     * connect, so refuse to come up instead. */
    ESP_RETURN_ON_FALSE(payload_len > 0 && payload_len < (int)sizeof(s_ctx.online_payload),
                        ESP_ERR_INVALID_SIZE, TAG, "firmware version '%s' is too long",
                        app_desc->version);
    s_ctx.online_payload_len = payload_len;

    for (int i = 0; i < NODE_TOPIC_COUNT; i++)
    {
        int written = snprintf(s_ctx.topics[i], NODE_PROTOCOL_TOPIC_SIZE, "%s/%s/%s",
                               root, s_ctx.device_id, TOPIC_SUFFIX[i]);
        /* snprintf truncates silently; a truncated topic would publish to the
         * wrong place, so refuse to come up instead. */
        ESP_RETURN_ON_FALSE(written > 0 && written < NODE_PROTOCOL_TOPIC_SIZE,
                            ESP_ERR_INVALID_ARG, TAG, "topic root '%s' is too long", root);
    }

    s_ctx.on_settings = config->on_settings;
    s_ctx.on_command = config->on_command;
    s_ctx.on_connected = config->on_connected;
    s_ctx.user_ctx = config->user_ctx;

    ESP_RETURN_ON_ERROR(mqtt_manager_register_data_handler(on_mqtt_data, NULL),
                        TAG, "data handler registration failed");
    esp_err_t ret = mqtt_manager_register_connection_handler(on_mqtt_connection, NULL);
    if (ret != ESP_OK)
    {
        mqtt_manager_unregister_data_handler(on_mqtt_data);
        ESP_RETURN_ON_ERROR(ret, TAG, "connection handler registration failed");
    }

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "Device %s (%s) fw %s, topics %s/%s/{telemetry,state,config,command}",
             s_ctx.device_id, s_ctx.mac_str, app_desc->version, root, s_ctx.device_id);
    return ESP_OK;
}

void node_protocol_apply_lwt(mqtt_manager_config_t *mqtt_cfg)
{
    if (!mqtt_cfg)
    {
        ESP_LOGE(TAG, "mqtt_cfg is NULL");
        return;
    }
    if (!s_ctx.initialized)
    {
        ESP_LOGW(TAG, "node_protocol_init() not called, leaving LWT unset");
        return;
    }

    mqtt_cfg->lwt_topic = s_ctx.topics[NODE_TOPIC_STATE];
    mqtt_cfg->lwt_msg = PAYLOAD_OFFLINE;
    mqtt_cfg->lwt_msg_len = sizeof(PAYLOAD_OFFLINE) - 1;
    mqtt_cfg->lwt_qos = 1;
    mqtt_cfg->lwt_retain = 1;
}

const char *node_protocol_device_id(void)
{
    return s_ctx.initialized ? s_ctx.device_id : EMPTY;
}

const char *node_protocol_mac(void)
{
    return s_ctx.initialized ? s_ctx.mac_str : EMPTY;
}

const char *node_protocol_topic(node_topic_t category)
{
    /* Cast: the enum's underlying type may be unsigned, where a bare
     * "category < 0" is a tautology the compiler warns about. */
    if (!s_ctx.initialized || (int)category < 0 || category >= NODE_TOPIC_COUNT)
    {
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

esp_err_t node_protocol_clear_state(void)
{
    /* A zero-length retained publish deletes the retained message. "" with
     * length 0 is what mqtt_manager_publish() turns into an empty payload. */
    return publish(NODE_TOPIC_STATE, "", 0, 1, 1);
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
