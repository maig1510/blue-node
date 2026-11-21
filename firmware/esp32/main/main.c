#include <stdatomic.h>
#include <stdio.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "a01nyub.h"
#include "mqtt_manager.h"
#include "node_config.h"
#include "node_protocol.h"
#include "provisioning.h"
#include "status_led.h"
#include "user_button.h"
#include "wifi_manager.h"

#define TAG "MAIN"

#define MEASUREMENT_INTERVAL_MS 1000

/* How long a fatal error stays visible on the LED before the node restarts. */
#define ERROR_RESTART_DELAY_MS 5000

/**
 * What the node is doing this boot. Decides what a button click means; only
 * written by app_main before the mode starts, read by the button callback.
 */
typedef enum
{
    MODE_STARTING,
    MODE_PAIRING, /* provisioning_run() is active: a click confirms the phone */
    MODE_IDLE,    /* unprovisioned and the pairing window closed */
    MODE_RUNNING, /* provisioned: WiFi, MQTT, measurements */
} node_mode_t;

static volatile node_mode_t s_mode = MODE_STARTING;
static a01nyub_t s_sensor;

/* Set on the mqtt event task when the device was removed in the app, acted on
 * by the measurement loop: the client cannot be stopped from its own task. */
static atomic_bool s_unregistered = false;

/* Runs on the mqtt event task: only flag it, see decommission(). */
static void on_settings(const node_protocol_settings_t *settings, void *user_ctx)
{
    if (!settings->registered)
    {
        ESP_LOGW(TAG, "Removed in the app");
        atomic_store(&s_unregistered, true);
    }
}

/* Placeholder: one-shot actions arrive here (e.g. {"cmd":"restart"}). */
static void on_command(const char *json, int len, void *user_ctx)
{
    ESP_LOGI(TAG, "Command received: %.*s", len, json);
}

/* Runs on the mqtt event task: just the LED, nothing slow. */
static void on_mqtt_connection(bool connected, void *user_ctx)
{
    status_led_set(connected ? STATUS_LED_ONLINE : STATUS_LED_WIFI_CONNECTING);
}

/* Runs with the provisioning lock held: LED only (see provisioning.h). */
static void on_provisioning_state(provisioning_state_t state, void *user_ctx)
{
    switch (state)
    {
    case PROVISIONING_STATE_ADVERTISING:
    case PROVISIONING_STATE_CONNECTED:
        status_led_set(STATUS_LED_PAIRING);
        break;
    case PROVISIONING_STATE_AWAIT_CONFIRM:
        status_led_set(STATUS_LED_AWAIT_CONFIRM);
        break;
    case PROVISIONING_STATE_CONFIRMED:
    case PROVISIONING_STATE_WIFI_OK:
        status_led_set(STATUS_LED_PAIRED);
        break;
    case PROVISIONING_STATE_WIFI_TESTING:
        status_led_set(STATUS_LED_WIFI_CONNECTING);
        break;
    case PROVISIONING_STATE_WIFI_FAILED:
        status_led_set(STATUS_LED_ERROR);
        break;
    }
}

/* Runs on the esp_timer task. Pairing and factory reset both go through a
 * restart, so neither has to unwind whatever the node was doing. */
static void on_button(user_button_event_t event, void *user_ctx)
{
    switch (event)
    {
    case USER_BUTTON_CLICK:
        if (s_mode == MODE_PAIRING)
        {
            provisioning_confirm();
        }
        else if (s_mode == MODE_IDLE)
        {
            /* Unprovisioned nodes always boot into pairing. */
            ESP_LOGI(TAG, "Click while idle: reopening pairing");
            esp_restart();
        }
        break;

    case USER_BUTTON_PAIRING_ARMED:
        status_led_set(STATUS_LED_HOLD_PAIRING);
        break;

    case USER_BUTTON_RESET_ARMED:
        status_led_set(STATUS_LED_HOLD_RESET);
        break;

    case USER_BUTTON_PAIRING_HOLD:
        ESP_LOGI(TAG, "Pairing requested, restarting");
        if (node_config_request_pairing() != ESP_OK)
        {
            ESP_LOGE(TAG, "Could not store the pairing request");
        }
        esp_restart();
        break;

    case USER_BUTTON_RESET_HOLD:
        ESP_LOGW(TAG, "Factory reset requested");
        if (node_config_factory_reset() != ESP_OK)
        {
            ESP_LOGE(TAG, "Factory reset failed");
        }
        esp_restart();
        break;
    }
}

static void fail_and_restart(const char *what, esp_err_t err)
{
    ESP_LOGE(TAG, "%s: %s - restarting in %d ms", what, esp_err_to_name(err), ERROR_RESTART_DELAY_MS);
    status_led_set(STATUS_LED_ERROR);
    vTaskDelay(pdMS_TO_TICKS(ERROR_RESTART_DELAY_MS));
    esp_restart();
}

/**
 * The device was removed in the app: leave nothing behind and start over.
 *
 * Clears the retained state on the broker and disconnects cleanly (so the Last
 * Will is not published for an id the backend no longer knows), then erases
 * the configuration. After the restart the node is unprovisioned and boots
 * into pairing, ready to be added again.
 */
static void decommission(void)
{
    status_led_set(STATUS_LED_HOLD_RESET);

    esp_err_t ret = node_protocol_clear_state();
    if (ret != ESP_OK)
    {
        /* Not fatal: the broker then keeps a stale retained state that
         * ingest drops anyway (the node_id is no longer registered). */
        ESP_LOGW(TAG, "Could not clear the retained state: %s", esp_err_to_name(ret));
    }
    mqtt_manager_stop();

    ret = node_config_factory_reset();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Factory reset failed: %s", esp_err_to_name(ret));
    }
    ESP_LOGW(TAG, "Decommissioned, restarting into pairing");
    esp_restart();
}

/**
 * Pairing mode: one BLE window, then a restart into whatever it produced.
 * Only returns if the node is still unprovisioned and nobody paired.
 */
static void run_pairing(void)
{
    s_mode = MODE_PAIRING;

    const provisioning_config_t cfg = {
        .hw_revision = CONFIG_IDF_TARGET,
        .on_state = on_provisioning_state,
    };
    provisioning_result_t result;
    esp_err_t ret = provisioning_run(&cfg, &result);
    if (ret != ESP_OK)
    {
        fail_and_restart("Pairing could not start", ret);
    }

    if (result == PROVISIONING_RESULT_STORED || node_config_is_provisioned())
    {
        /* New configuration, or a re-pairing that was abandoned: either way
         * there is a configuration to run with. */
        ESP_LOGI(TAG, "Pairing ended, restarting into normal operation");
        esp_restart();
    }

    s_mode = MODE_IDLE;
    status_led_set(STATUS_LED_OFF);
    ESP_LOGW(TAG, "Not provisioned and the pairing window closed. Press the button to pair.");
}

/** Normal operation: WiFi and broker from node_config, then measure forever. */
static void run_node(void)
{
    s_mode = MODE_RUNNING;
    const node_config_t *cfg = node_config_get();

    wifi_manager_config_t wifi_cfg = {
        .max_retries = 0, /* never give up: re-pairing is the user's way out */
    };
    strlcpy(wifi_cfg.ssid, cfg->wifi_ssid, sizeof(wifi_cfg.ssid));
    strlcpy(wifi_cfg.password, cfg->wifi_password, sizeof(wifi_cfg.password));

    status_led_set(STATUS_LED_WIFI_CONNECTING);
    esp_err_t ret = wifi_manager_connect(&wifi_cfg);
    if (ret != ESP_OK)
    {
        fail_and_restart("WiFi setup failed", ret);
    }
    char ip[16];
    wifi_manager_get_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "WiFi connected, IP: %s", ip);

    /* Before mqtt_manager_init(): the state topic built here is what the client
     * needs as its Last Will topic. */
    node_protocol_config_t proto_cfg = {
        .on_settings = on_settings,
        .on_command = on_command,
    };
    ESP_ERROR_CHECK(node_protocol_init(&proto_cfg));
    ESP_ERROR_CHECK(mqtt_manager_register_connection_handler(on_mqtt_connection, NULL));

    /* broker_uri points into node_config's RAM copy, which lives for the whole
     * run - as mqtt_manager requires of every pointer it is handed. */
    mqtt_manager_config_t mqtt_cfg = {
        .broker_uri = cfg->broker_uri,
    };
    node_protocol_apply_lwt(&mqtt_cfg);

    ESP_ERROR_CHECK(mqtt_manager_init(&mqtt_cfg));
    ESP_ERROR_CHECK(mqtt_manager_start());

    s_sensor = (a01nyub_t)A01NYUB_DEFAULT();
    s_sensor.rx_pin = GPIO_NUM_3;
    s_sensor.tx_pin = GPIO_NUM_2;
    s_sensor.uart_port = UART_NUM_1;
    s_sensor.mode = A01NYUB_MODE_FILTERED;
    s_sensor.read_mode = A01NYUB_READ_CONTINUOUS;

    ESP_ERROR_CHECK(a01nyub_init(&s_sensor));

    uint16_t distance;

    while (true)
    {
        if (atomic_load(&s_unregistered))
        {
            decommission();
        }

        esp_err_t ret = a01nyub_get_data(&s_sensor, &distance);

        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG, "Distance: %d mm  (%.1f cm)", distance, distance / 10.0f);

            char payload[32];
            int len = snprintf(payload, sizeof(payload), "{\"distance_mm\":%u}", distance);
            node_protocol_publish_telemetry(payload, len);
        }
        else if (ret == ESP_ERR_INVALID_STATE)
        {
            ESP_LOGW(TAG, "Waiting for first reading...");
        }

        vTaskDelay(pdMS_TO_TICKS(MEASUREMENT_INTERVAL_MS));
    }
}

void app_main(void)
{
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);
    ESP_ERROR_CHECK(node_config_init());

    /* Shared by every network component below (wifi_manager, mqtt_manager,
     * provisioning), each of which documents these as its preconditions. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Feedback and input are nice to have, not required: a board without an
     * LED, or a misconfigured pin, must still run. */
    esp_err_t ret = status_led_init();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Status LED unavailable: %s", esp_err_to_name(ret));
    }
    ret = user_button_init(on_button, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Button unavailable, pairing can only start on an unprovisioned node: %s",
                 esp_err_to_name(ret));
    }

    /* Take the request unconditionally, so it never outlives this boot. */
    bool pairing_requested = node_config_take_pairing_request();
    if (pairing_requested || !node_config_is_provisioned())
    {
        ESP_LOGI(TAG, "Entering pairing (%s)", pairing_requested ? "requested" : "not provisioned");
        run_pairing();
        return; /* idle: the button callback takes it from here */
    }

    run_node();
}
