/**
 * @file wifi_manager.c
 * @brief WiFi STA manager implementation.
 *
 * State machine is driven entirely by the ESP event system. The public
 * connect/disconnect calls are the only entry points; all reconnect logic
 * lives in wifi_event_handler() and the backoff timer it arms.
 */

#include "wifi_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>

static const char *TAG = "WIFI_MANAGER";

/* Event bits used to signal the blocking wait in wifi_manager_connect(). */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1

/* Reconnect backoff: 1 s, 2 s, 4 s ... capped at 60 s. Immediate retries in a
 * tight loop (the old behaviour) only hammer an AP that is rebooting. */
#define BACKOFF_BASE_MS 1000
#define BACKOFF_MAX_MS 60000

/**
 * @brief Module-level state.  Single instance; not guarded by a mutex, so
 *        connect/disconnect must not be called from multiple tasks.
 *
 * The event handler and the backoff timer touch retries/ever_connected from
 * the event loop and esp_timer tasks respectively; the timer only ever calls
 * esp_wifi_connect(), so the fields themselves are written by the event
 * handler alone.
 */
static struct
{
    EventGroupHandle_t events;          /**< Created on connect, deleted on disconnect. */
    esp_timer_handle_t reconnect_timer; /**< Arms the next attempt after a backoff delay. */
    uint8_t retries;                    /**< Consecutive failed attempts. */
    uint8_t max_retries;                /**< Copied from config; 0 = retry forever. */
    bool ever_connected;                /**< Once true, max_retries no longer applies. */
    bool netif_created;                 /**< Guards the one-time default STA netif. */
} s_ctx = {0};

static void reconnect_cb(void *arg)
{
    esp_wifi_connect();
}

static uint32_t backoff_ms(uint8_t attempt)
{
    uint32_t delay = BACKOFF_BASE_MS;
    while (attempt-- > 0 && delay < BACKOFF_MAX_MS)
    {
        delay *= 2;
    }
    return delay > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : delay;
}

/**
 * @brief Unified event handler for both WIFI_EVENT and IP_EVENT.
 *
 * Registered for ESP_EVENT_ANY_ID on WIFI_EVENT so a single handler covers
 * the STA_START and STA_DISCONNECTED cases without separate registrations.
 */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        /* WiFi driver is up — initiate the first association attempt. */
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        const wifi_event_sta_disconnected_t *e = data;
        /* The link is down now, whatever happens next. */
        xEventGroupClearBits(s_ctx.events, WIFI_CONNECTED_BIT);

        bool give_up = !s_ctx.ever_connected && s_ctx.max_retries != 0 &&
                       s_ctx.retries >= s_ctx.max_retries;
        if (give_up)
        {
            /* Initial connect failed — unblock wifi_manager_connect() with failure. */
            ESP_LOGE(TAG, "Connection failed after %d retries (reason %d)",
                     s_ctx.retries, e->reason);
            xEventGroupSetBits(s_ctx.events, WIFI_FAILED_BIT);
            return;
        }

        uint32_t delay = backoff_ms(s_ctx.retries);
        if (s_ctx.retries < UINT8_MAX)
        {
            s_ctx.retries++;
        }
        ESP_LOGW(TAG, "Disconnected (reason %d), retry %d in %lu ms",
                 e->reason, s_ctx.retries, (unsigned long)delay);
        esp_timer_stop(s_ctx.reconnect_timer);
        esp_timer_start_once(s_ctx.reconnect_timer, (uint64_t)delay * 1000);
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        /* DHCP lease received — unblock wifi_manager_connect() with success. */
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected — IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_ctx.retries = 0;
        s_ctx.ever_connected = true;
        xEventGroupSetBits(s_ctx.events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_fill_sta_config(wifi_config_t *out, const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(out && ssid && password, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    const size_t ssid_len = strlen(ssid);
    const size_t pass_len = strlen(password);
    ESP_RETURN_ON_FALSE(ssid_len > 0 && ssid_len <= sizeof(out->sta.ssid), ESP_ERR_INVALID_ARG, TAG,
                        "SSID length %u out of range", (unsigned)ssid_len);
    ESP_RETURN_ON_FALSE(pass_len <= sizeof(out->sta.password), ESP_ERR_INVALID_ARG, TAG,
                        "password length %u out of range", (unsigned)pass_len);

    memset(out, 0, sizeof(*out));
    memcpy(out->sta.ssid, ssid, ssid_len);
    memcpy(out->sta.password, password, pass_len);
    out->sta.threshold.authmode = pass_len ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const wifi_manager_config_t *config)
{
    if (!config)
        return ESP_ERR_INVALID_ARG;

    /* Validated before anything is allocated or registered. */
    wifi_config_t wifi_cfg;
    ESP_RETURN_ON_ERROR(wifi_manager_fill_sta_config(&wifi_cfg, config->ssid, config->password), TAG,
                        "invalid credentials");

    s_ctx.events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_ctx.events, ESP_ERR_NO_MEM, TAG, "event group failed");
    s_ctx.max_retries = config->max_retries;
    s_ctx.retries = 0;
    s_ctx.ever_connected = false;

    const esp_timer_create_args_t timer_args = {.callback = reconnect_cb, .name = "wifi_reconnect"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_ctx.reconnect_timer), TAG, "timer failed");

    /* The default STA netif must be created exactly once per application
     * lifetime; esp_netif_init() and the default event loop are the caller's. */
    if (!s_ctx.netif_created)
    {
        esp_netif_create_default_wifi_sta();
        s_ctx.netif_created = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");
    /* node_config owns the credentials; the driver's NVS copy would be a second
     * store that a factory reset of node_config alone would leave behind. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set storage failed");

    /* Register for all WIFI_EVENTs (covers STA_START + STA_DISCONNECTED) and
     * separately for the IP_EVENT that signals a successful DHCP lease. */
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG, "event handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL), TAG, "event handler failed");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set config failed");
    /* esp_wifi_start() fires WIFI_EVENT_STA_START, which triggers esp_wifi_connect() in the handler. */
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    /* Block until one of the two terminal bits is set by the event handler.
     * pdFALSE, pdFALSE = don't clear bits, don't require all. */
    EventBits_t bits = xEventGroupWaitBits(s_ctx.events,
                                           WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                           pdFALSE, pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
        return ESP_OK;

    return ESP_FAIL;
}

esp_err_t wifi_manager_disconnect(void)
{
    /* Unregister before stopping to prevent the STA_DISCONNECTED event from
     * arming another reconnect during deliberate teardown, and retire a
     * reconnect that is already armed. */
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler);
    if (s_ctx.reconnect_timer)
    {
        esp_timer_stop(s_ctx.reconnect_timer);
        esp_timer_delete(s_ctx.reconnect_timer);
        s_ctx.reconnect_timer = NULL;
    }
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_ctx.events)
    {
        vEventGroupDelete(s_ctx.events);
        s_ctx.events = NULL;
    }

    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    if (!s_ctx.events)
        return false;
    return (xEventGroupGetBits(s_ctx.events) & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_manager_get_ip(char *buf, size_t len)
{
    if (!buf)
        return ESP_ERR_INVALID_ARG;

    /* "WIFI_STA_DEF" is the fixed interface key assigned by
     * esp_netif_create_default_wifi_sta(); returns NULL if not created yet. */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif)
        return ESP_FAIL;

    esp_netif_ip_info_t ip;
    esp_err_t ret = esp_netif_get_ip_info(netif, &ip);
    if (ret != ESP_OK)
        return ret;

    snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    return ESP_OK;
}
