/**
 * @file mqtt_manager.c
 * @brief MQTT client manager implementation.
 *
 * State is driven by esp-mqtt's own event callback (mqtt_event_handler());
 * this file just tracks connection state and fans MQTT_EVENT_DATA out to
 * registered handlers. See mqtt_manager.h for the ownership rules this
 * component follows (no NVS/netif/event-loop/WiFi init here).
 */

#include "mqtt_manager.h"

#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>

static const char *TAG = "MQTT_MANAGER";

/**
 * @brief Module-level state. Single instance; not guarded by a mutex, so
 *        init/start/stop and handler registration must not race each other.
 */
static struct {
    esp_mqtt_client_handle_t client;      /**< Created once in mqtt_manager_init(). */
    bool                     connected;   /**< Mirrors the last CONNECTED/DISCONNECTED event. */
    bool                     initialized; /**< Guards the one-time client creation. */
    bool                     started;     /**< True between a successful start() and stop(). */
    struct {
        mqtt_manager_data_cb_t cb;
        void                   *user_ctx;
    } handlers[MQTT_MANAGER_MAX_DATA_HANDLERS];
} s_ctx = {0};

/**
 * @brief esp-mqtt event callback, registered for every event ID.
 *
 * Updates s_ctx.connected on CONNECTED/DISCONNECTED and fans DATA events out
 * to every registered handler in registration order.
 */
static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to broker");
        s_ctx.connected = true;
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker");
        s_ctx.connected = false;
        break;

    case MQTT_EVENT_DATA:
        for (int i = 0; i < MQTT_MANAGER_MAX_DATA_HANDLERS; i++) {
            if (s_ctx.handlers[i].cb) {
                s_ctx.handlers[i].cb(event->topic, event->topic_len,
                                      event->data, event->data_len,
                                      s_ctx.handlers[i].user_ctx);
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        if (!event->error_handle) {
            ESP_LOGE(TAG, "MQTT error, no error_handle provided");
            break;
        }
        ESP_LOGE(TAG, "MQTT error, type=%d", event->error_handle->error_type);
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "  transport error: esp_tls last error 0x%x, esp_tls stack 0x%x, sock errno %d",
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_tls_stack_err,
                     event->error_handle->esp_transport_sock_errno);
        }
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGD(TAG, "Subscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGD(TAG, "Unsubscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "Published, msg_id=%d", event->msg_id);
        break;

    default:
        break;
    }
}

esp_err_t mqtt_manager_init(const mqtt_manager_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(config->broker_uri, ESP_ERR_INVALID_ARG, TAG, "config->broker_uri is NULL");
    ESP_RETURN_ON_FALSE(!s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "already initialized");

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = config->broker_uri,
        .credentials.client_id = config->client_id,
        .credentials.username = config->username,
        .credentials.authentication.password = config->password,
    };

    if (config->ca_cert_pem) {
        mqtt_cfg.broker.verification.certificate = config->ca_cert_pem;
        mqtt_cfg.broker.verification.certificate_len = config->ca_cert_len;
    }

    s_ctx.client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_RETURN_ON_FALSE(s_ctx.client, ESP_FAIL, TAG, "esp_mqtt_client_init failed");

    /* ESP_EVENT_ANY_ID: one registration dispatches every event through
     * mqtt_event_handler()'s switch, instead of one registration per event ID. */
    ESP_RETURN_ON_ERROR(
        esp_mqtt_client_register_event(s_ctx.client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL),
        TAG, "event handler registration failed");

    s_ctx.initialized = true;
    return ESP_OK;
}

esp_err_t mqtt_manager_start(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "mqtt_manager_init() not called");
    ESP_RETURN_ON_FALSE(!s_ctx.started, ESP_ERR_INVALID_STATE, TAG, "already started");

    esp_err_t ret = esp_mqtt_client_start(s_ctx.client);
    if (ret == ESP_OK) {
        s_ctx.started = true;
    }
    return ret;
}

esp_err_t mqtt_manager_stop(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "mqtt_manager_init() not called");

    esp_err_t ret = esp_mqtt_client_stop(s_ctx.client);
    if (ret == ESP_OK) {
        s_ctx.started = false;
        /* An explicit stop() doesn't reliably raise MQTT_EVENT_DISCONNECTED,
         * so clear connected here rather than waiting for that event. */
        s_ctx.connected = false;
    }
    return ret;
}

esp_err_t mqtt_manager_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "mqtt_manager_init() not called");
    ESP_RETURN_ON_FALSE(!s_ctx.started, ESP_ERR_INVALID_STATE, TAG, "mqtt_manager_stop() not called");

    esp_err_t ret = esp_mqtt_client_destroy(s_ctx.client);
    if (ret == ESP_OK) {
        memset(&s_ctx, 0, sizeof(s_ctx));
    }
    return ret;
}

esp_err_t mqtt_manager_publish(const char *topic, const char *data, int len, int qos, int retain)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "mqtt_manager_init() not called");
    ESP_RETURN_ON_FALSE(s_ctx.connected, ESP_ERR_INVALID_STATE, TAG, "not connected");
    ESP_RETURN_ON_FALSE(topic, ESP_ERR_INVALID_ARG, TAG, "topic is NULL");

    /* esp_mqtt_client_publish/subscribe/unsubscribe return a msg_id (or -1 on
     * failure), not esp_err_t, hence the translation below and at each call site. */
    int msg_id = esp_mqtt_client_publish(s_ctx.client, topic, data, len, qos, retain);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t mqtt_manager_subscribe(const char *topic, int qos)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "mqtt_manager_init() not called");
    ESP_RETURN_ON_FALSE(topic, ESP_ERR_INVALID_ARG, TAG, "topic is NULL");

    int msg_id = esp_mqtt_client_subscribe(s_ctx.client, topic, qos);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t mqtt_manager_unsubscribe(const char *topic)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "mqtt_manager_init() not called");
    ESP_RETURN_ON_FALSE(topic, ESP_ERR_INVALID_ARG, TAG, "topic is NULL");

    int msg_id = esp_mqtt_client_unsubscribe(s_ctx.client, topic);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

bool mqtt_manager_is_connected(void)
{
    return s_ctx.connected;
}

esp_err_t mqtt_manager_register_data_handler(mqtt_manager_data_cb_t cb, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(cb, ESP_ERR_INVALID_ARG, TAG, "cb is NULL");

    for (int i = 0; i < MQTT_MANAGER_MAX_DATA_HANDLERS; i++) {
        if (!s_ctx.handlers[i].cb) {
            s_ctx.handlers[i].cb = cb;
            s_ctx.handlers[i].user_ctx = user_ctx;
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "No free data handler slots (max %d)", MQTT_MANAGER_MAX_DATA_HANDLERS);
    return ESP_ERR_NO_MEM;
}

esp_err_t mqtt_manager_unregister_data_handler(mqtt_manager_data_cb_t cb)
{
    ESP_RETURN_ON_FALSE(cb, ESP_ERR_INVALID_ARG, TAG, "cb is NULL");

    for (int i = 0; i < MQTT_MANAGER_MAX_DATA_HANDLERS; i++) {
        if (s_ctx.handlers[i].cb == cb) {
            s_ctx.handlers[i].cb = NULL;
            s_ctx.handlers[i].user_ctx = NULL;
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}
