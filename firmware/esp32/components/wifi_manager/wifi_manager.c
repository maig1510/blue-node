/**
 * @file wifi_manager.c
 * @brief WiFi STA manager implementation.
 *
 * State machine is driven entirely by the ESP event system.  The public
 * connect/disconnect calls are the only entry points; all reconnect logic
 * lives in wifi_event_handler().
 */

#include "wifi_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>

static const char *TAG = "WIFI_MANAGER";

/* Event bits used to signal the blocking wait in wifi_manager_connect(). */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAILED_BIT     BIT1

/**
 * @brief Module-level state.  Single instance; not guarded by a mutex, so
 *        connect/disconnect must not be called from multiple tasks.
 */
static struct {
    EventGroupHandle_t events;       /**< Created on connect, deleted on disconnect. */
    uint8_t            retries;      /**< Attempts made in the current connect call. */
    uint8_t            max_retries;  /**< Copied from config; 0 = retry forever. */
    bool               initialized;  /**< Guards the one-time netif/event-loop init. */
} s_ctx = {0};


/**
 * @brief Unified event handler for both WIFI_EVENT and IP_EVENT.
 *
 * Registered for ESP_EVENT_ANY_ID on WIFI_EVENT so a single handler covers
 * the STA_START and STA_DISCONNECTED cases without separate registrations.
 */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* WiFi driver is up — initiate the first association attempt. */
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* max_retries == 0 is the "retry forever" sentinel. */
        if (s_ctx.max_retries == 0 || s_ctx.retries < s_ctx.max_retries) {
            esp_wifi_connect();
            s_ctx.retries++;
            ESP_LOGW(TAG, "Retry %d/%d...", s_ctx.retries,
                     s_ctx.max_retries == 0 ? -1 : s_ctx.max_retries);
        } else {
            /* All retries consumed — unblock wifi_manager_connect() with failure. */
            xEventGroupSetBits(s_ctx.events, WIFI_FAILED_BIT);
            ESP_LOGE(TAG, "Connection failed after %d retries", s_ctx.retries);
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        /* DHCP lease received — unblock wifi_manager_connect() with success. */
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected — IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_ctx.retries = 0;
        xEventGroupSetBits(s_ctx.events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_connect(const wifi_manager_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    s_ctx.events      = xEventGroupCreate();
    s_ctx.max_retries = config->max_retries;
    s_ctx.retries     = 0;

    /* esp_netif_init() and esp_event_loop_create_default() must each be called
     * exactly once per application lifetime; skip them on subsequent calls. */
    if (!s_ctx.initialized) {
        ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
        ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");
        esp_netif_create_default_wifi_sta();
        s_ctx.initialized = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");

    /* Register for all WIFI_EVENTs (covers STA_START + STA_DISCONNECTED) and
     * separately for the IP_EVENT that signals a successful DHCP lease. */
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG, "event handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL), TAG, "event handler failed");

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid,     config->ssid,     sizeof(wifi_cfg.sta.ssid));
    strncpy((char *)wifi_cfg.sta.password, config->password, sizeof(wifi_cfg.sta.password));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set config failed");
    /* esp_wifi_start() fires WIFI_EVENT_STA_START, which triggers esp_wifi_connect() in the handler. */
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    /* Block indefinitely until one of the two terminal bits is set by the
     * event handler.  pdFALSE, pdFALSE = don't clear bits, don't require all. */
    EventBits_t bits = xEventGroupWaitBits(s_ctx.events,
                                            WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                            pdFALSE, pdFALSE,
                                            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) return ESP_OK;

    return ESP_FAIL;
}

esp_err_t wifi_manager_disconnect(void)
{
    /* Unregister before stopping to prevent the STA_DISCONNECTED event from
     * triggering another reconnect attempt during deliberate teardown. */
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler);
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_ctx.events) {
        vEventGroupDelete(s_ctx.events);
        s_ctx.events = NULL;
    }

    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    if (!s_ctx.events) return false;
    return (xEventGroupGetBits(s_ctx.events) & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_manager_get_ip(char *buf, size_t len)
{
    if (!buf) return ESP_ERR_INVALID_ARG;

    /* "WIFI_STA_DEF" is the fixed interface key assigned by
     * esp_netif_create_default_wifi_sta(); returns NULL if not created yet. */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif)  return ESP_FAIL;

    esp_netif_ip_info_t ip;
    esp_err_t ret = esp_netif_get_ip_info(netif, &ip);
    if (ret != ESP_OK) return ret;

    snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    return ESP_OK;
}
