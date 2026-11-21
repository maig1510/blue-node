/**
 * @file provisioning.c
 * @brief BLE pairing window on top of espressif/network_provisioning.
 */

#include "provisioning.h"

#include "node_config.h"
#include "wifi_manager.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "nimble/ble.h"
#include "protocomm_ble.h"
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "PROVISIONING";

/* Endpoint names: the app looks them up by these strings (GATT user
 * description), so they are part of the protocol. */
#define EP_CONFIRM "bn-confirm"
#define EP_INFO "bn-info"
#define EP_WIFI "bn-wifi"
#define EP_STATUS "bn-status"
#define EP_CLOUD "bn-cloud"

/** Version of the JSON protocol on the bn-* endpoints, reported by bn-info. */
#define PROTOCOL_VERSION 1

/** Capability advertised in proto-ver, so the app can refuse an unknown node. */
#define APP_CAPABILITY "bn-v1"

/** Advertised name prefix; the app scans for it. */
#define NAME_PREFIX "BN-"

/** How long one WiFi credential test may take before it counts as failed. */
#define WIFI_TEST_TIMEOUT_US (20 * 1000 * 1000)

/** Reconnect attempts within one test, counted together for every reason that
 *  is not an authentication failure (which is final at once). */
#define WIFI_TEST_RETRIES 3

/** Of those, how many a "no AP found" may use: a scan that shares the radio
 *  with BLE can miss an AP once, but a network that is missing twice in a row
 *  is reported as such rather than as a timeout. */
#define WIFI_TEST_NO_AP_RETRIES 1

/** esp_wifi_set_config() refuses with ESP_ERR_WIFI_STATE while the driver is
 *  still busy with the attempt that was just disconnected; that settles within
 *  a few milliseconds. Runs on the NimBLE host task, so kept short. */
#define WIFI_SET_CONFIG_RETRIES 2
#define WIFI_SET_CONFIG_RETRY_MS 100

/** Longest wait between storing the result and ending the window, so the app
 *  can read the bn-cloud response before the GATT server goes away. The window
 *  ends earlier when the phone disconnects first, as the app does once it has
 *  the response. */
#define FINISH_DELAY_US (5 * 1000 * 1000)

#define WINDOW_OVERRUN_US (PROVISIONING_WINDOW_OVERRUN_S * 1000 * 1000ULL)

/* Failure reasons reported by bn-status. Compared by pointer, so always use
 * these, never a string literal. */
static const char REASON_AUTH[] = "auth";
static const char REASON_NO_AP[] = "no_ap";
static const char REASON_TIMEOUT[] = "timeout";
static const char REASON_OTHER[] = "other";

#define DONE_BIT BIT0
#define WINDOW_CLOSED_BIT BIT1

/* 128-bit service UUID of the blue-node provisioning service, LSB first.
 * The endpoints are characteristics derived from it by network_provisioning.
 * (b1e0de00-6b1d-4c8a-9f3e-5a7c2d4e8f10) */
static uint8_t s_service_uuid[16] = {
    0x10,
    0x8f,
    0x4e,
    0x2d,
    0x7c,
    0x5a,
    0x3e,
    0x9f,
    0x8a,
    0x4c,
    0x1d,
    0x6b,
    0x00,
    0xde,
    0xe0,
    0xb1,
};

/* Manufacturer data in the scan response, so the app can show "already added"
 * before connecting: Espressif's company id (0x02E5, little endian), the
 * magic "BN", a format version, a flags byte and the full MAC. 12 bytes, which
 * fits next to the 9-byte name (31 - (9 + 2) - 2 = 18 available). */
#define MFG_COMPANY_ID_LO 0xE5
#define MFG_COMPANY_ID_HI 0x02
#define MFG_FORMAT_VERSION 1
#define MFG_FLAG_PROVISIONED 0x01
#define MFG_DATA_LEN 12

typedef enum
{
    WIFI_TEST_IDLE,
    WIFI_TEST_CONNECTING,
    WIFI_TEST_CONNECTED,
    WIFI_TEST_FAILED,
} wifi_test_t;

static struct
{
    SemaphoreHandle_t lock;
    EventGroupHandle_t events;
    esp_timer_handle_t window_timer;
    esp_timer_handle_t confirm_timer;
    esp_timer_handle_t wifi_timer;
    esp_timer_handle_t finish_timer;
    esp_timer_handle_t overrun_timer;

    provisioning_config_t cfg;
    char hw_revision[32];
    char name[sizeof(NAME_PREFIX) + 6];
    char mac_str[18]; /* AA:BB:CC:DD:EE:FF */
    char node_id[13]; /* AABBCCDDEEFF, as node_protocol puts it in topics */
    uint8_t mfg_data[MFG_DATA_LEN];

    provisioning_state_t state;
    bool window_expired; /* expired while a phone was connected */
    bool finishing;      /* result stored, window about to end */

    bool connected;
    uint16_t conn_handle;
    bool awaiting; /* the connected phone asked for confirmation */
    bool confirmed;
    uint32_t session; /* protocomm session of the connected phone */

    /* One WiFi test, owned by the connection that started it: dropped (and
     * the credentials wiped) when that phone disconnects. */
    wifi_test_t wifi;
    const char *fail_reason;
    uint8_t retries;
    char ssid[NODE_CONFIG_SSID_MAX_LEN + 1];
    char password[NODE_CONFIG_PASSWORD_MAX_LEN + 1];
    char ip[16];
} s_ctx;

static bool s_ran;

#define LOCK() xSemaphoreTake(s_ctx.lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_ctx.lock)

/* ------------------------------------------------------------------------- */
/* Helpers (call with the lock held unless noted)                            */
/* ------------------------------------------------------------------------- */

static void set_state(provisioning_state_t state)
{
    if (s_ctx.state == state)
    {
        return;
    }
    s_ctx.state = state;
    if (s_ctx.cfg.on_state)
    {
        s_ctx.cfg.on_state(state, s_ctx.cfg.user_ctx);
    }
}

/** Is @p session the connection the user confirmed? */
static bool authorized(uint32_t session)
{
    return s_ctx.connected && s_ctx.confirmed && s_ctx.session == session;
}

/** Serialise @p obj into a heap buffer protocomm frees after sending. */
static esp_err_t respond(cJSON *obj, uint8_t **outbuf, ssize_t *outlen)
{
    /* cJSON allocates with malloc by default, and protocomm releases the
     * response with free(), so the printed buffer can be handed over as is.
     * No trailing NUL: the app parses the bytes as JSON text. */
    char *text = obj ? cJSON_PrintUnformatted(obj) : NULL;
    cJSON_Delete(obj);
    if (!text)
    {
        return ESP_ERR_NO_MEM;
    }
    *outbuf = (uint8_t *)text;
    *outlen = (ssize_t)strlen(text);
    return ESP_OK;
}

static esp_err_t respond_error(const char *error, uint8_t **outbuf, ssize_t *outlen)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", false);
    cJSON_AddStringToObject(obj, "error", error);
    return respond(obj, outbuf, outlen);
}

static esp_err_t respond_ok(uint8_t **outbuf, ssize_t *outlen)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    return respond(obj, outbuf, outlen);
}

/** Parse a request body; an empty body is an empty object. Caller frees. */
static cJSON *parse_request(const uint8_t *inbuf, ssize_t inlen)
{
    if (!inbuf || inlen <= 0)
    {
        return cJSON_CreateObject();
    }
    return cJSON_ParseWithLength((const char *)inbuf, (size_t)inlen);
}

static const char *json_string(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static void close_window(void)
{
    xEventGroupSetBits(s_ctx.events, WINDOW_CLOSED_BIT);
}

/** Does @p password fit a WiFi network: empty (open), an 8..63 character WPA
 *  passphrase, or a 64-digit hex PSK? (No lock needed.) */
static bool valid_password(const char *password, size_t len)
{
    if (len == 0)
    {
        return true;
    }
    if (len < 8 || len > NODE_CONFIG_PASSWORD_MAX_LEN)
    {
        return false;
    }
    if (len < 64)
    {
        return true;
    }
    /* 64 characters can only be a raw PSK; the driver rejects anything else
     * there, and the app should hear "invalid" for it, not a generic failure. */
    for (size_t i = 0; i < len; i++)
    {
        if (!isxdigit((unsigned char)password[i]))
        {
            return false;
        }
    }
    return true;
}

/** esp_wifi_set_config(), riding out the moment the driver still spends on the
 *  attempt that was just disconnected. (No lock needed; blocks briefly.) */
static esp_err_t apply_wifi_config(wifi_config_t *cfg)
{
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, cfg);
    for (int i = 0; i < WIFI_SET_CONFIG_RETRIES && ret == ESP_ERR_WIFI_STATE; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(WIFI_SET_CONFIG_RETRY_MS));
        ret = esp_wifi_set_config(WIFI_IF_STA, cfg);
    }
    return ret;
}

/* ------------------------------------------------------------------------- */
/* Endpoint handlers (NimBLE host task)                                      */
/* ------------------------------------------------------------------------- */

static esp_err_t handle_confirm(uint32_t session, const uint8_t *inbuf, ssize_t inlen,
                                uint8_t **outbuf, ssize_t *outlen, void *priv)
{
    LOCK();
    bool confirmed = authorized(session);
    if (!confirmed && s_ctx.connected && s_ctx.session == session)
    {
        /* Only now does a click mean something: see provisioning_confirm(). */
        s_ctx.awaiting = true;
        set_state(PROVISIONING_STATE_AWAIT_CONFIRM);
    }
    UNLOCK();

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "state", confirmed ? "confirmed" : "pending");
    return respond(obj, outbuf, outlen);
}

static esp_err_t handle_info(uint32_t session, const uint8_t *inbuf, ssize_t inlen,
                             uint8_t **outbuf, ssize_t *outlen, void *priv)
{
    LOCK();
    bool ok = authorized(session);
    UNLOCK();
    if (!ok)
    {
        return respond_error("not_confirmed", outbuf, outlen);
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "v", PROTOCOL_VERSION);
    cJSON_AddStringToObject(obj, "mac", s_ctx.mac_str);
    cJSON_AddStringToObject(obj, "node_id", s_ctx.node_id);
    cJSON_AddStringToObject(obj, "fw", esp_app_get_description()->version);
    cJSON_AddStringToObject(obj, "hw", s_ctx.hw_revision);
    cJSON_AddBoolToObject(obj, "provisioned", node_config_is_provisioned());
    return respond(obj, outbuf, outlen);
}

static esp_err_t handle_wifi(uint32_t session, const uint8_t *inbuf, ssize_t inlen,
                             uint8_t **outbuf, ssize_t *outlen, void *priv)
{
    cJSON *req = parse_request(inbuf, inlen);
    const char *ssid = json_string(req, "ssid");
    const char *password = json_string(req, "password");
    size_t ssid_len = ssid ? strlen(ssid) : 0;

    /* Same configuration the node joins with after the restart (see
     * wifi_manager_fill_sta_config() for the security policy), so passing
     * this test means the running node can connect too. */
    wifi_config_t wifi_cfg;
    bool valid = ssid && password && ssid_len > 0 && ssid_len <= NODE_CONFIG_SSID_MAX_LEN &&
                 valid_password(password, strlen(password)) &&
                 wifi_manager_fill_sta_config(&wifi_cfg, ssid, password) == ESP_OK;

    LOCK();
    if (!authorized(session))
    {
        UNLOCK();
        cJSON_Delete(req);
        return respond_error("not_confirmed", outbuf, outlen);
    }
    if (s_ctx.finishing)
    {
        /* bn-cloud already stored the tested network; nothing left to test. */
        UNLOCK();
        cJSON_Delete(req);
        return respond_ok(outbuf, outlen);
    }
    if (!valid)
    {
        UNLOCK();
        cJSON_Delete(req);
        return respond_error("invalid", outbuf, outlen);
    }

    strlcpy(s_ctx.ssid, ssid, sizeof(s_ctx.ssid));
    strlcpy(s_ctx.password, password, sizeof(s_ctx.password));
    cJSON_Delete(req);

    s_ctx.wifi = WIFI_TEST_CONNECTING;
    s_ctx.fail_reason = NULL;
    s_ctx.retries = 0;
    s_ctx.ip[0] = '\0';
    set_state(PROVISIONING_STATE_WIFI_TESTING);
    ESP_LOGI(TAG, "Testing WiFi '%s'", s_ctx.ssid);
    UNLOCK();

    /* A scan from prov-scan may still be running; connecting during it fails. */
    esp_wifi_scan_stop();
    /* Drop a previous test's association. The resulting disconnect carries
     * WIFI_REASON_ASSOC_LEAVE, which on_wifi_event() ignores. */
    esp_wifi_disconnect();

    /* Armed before connecting, so an early result cannot be followed by a
     * timer that was started after it. */
    esp_timer_stop(s_ctx.wifi_timer);
    esp_timer_start_once(s_ctx.wifi_timer, WIFI_TEST_TIMEOUT_US);

    esp_err_t ret = apply_wifi_config(&wifi_cfg);
    if (ret == ESP_OK)
    {
        ret = esp_wifi_connect();
    }
    if (ret != ESP_OK)
    {
        esp_timer_stop(s_ctx.wifi_timer);
        ESP_LOGW(TAG, "Starting the WiFi test failed: %s", esp_err_to_name(ret));
        LOCK();
        s_ctx.wifi = WIFI_TEST_FAILED;
        s_ctx.fail_reason = REASON_OTHER;
        set_state(PROVISIONING_STATE_WIFI_FAILED);
        UNLOCK();
        /* The driver has the last word on what a valid password is. */
        bool rejected = ret == ESP_ERR_WIFI_PASSWORD || ret == ESP_ERR_INVALID_ARG;
        return respond_error(rejected ? "invalid" : "busy", outbuf, outlen);
    }
    return respond_ok(outbuf, outlen);
}

static esp_err_t handle_status(uint32_t session, const uint8_t *inbuf, ssize_t inlen,
                               uint8_t **outbuf, ssize_t *outlen, void *priv)
{
    static const char *const NAMES[] = {
        [WIFI_TEST_IDLE] = "idle",
        [WIFI_TEST_CONNECTING] = "connecting",
        [WIFI_TEST_CONNECTED] = "connected",
        [WIFI_TEST_FAILED] = "failed",
    };

    LOCK();
    if (!authorized(session))
    {
        UNLOCK();
        return respond_error("not_confirmed", outbuf, outlen);
    }
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "wifi", NAMES[s_ctx.wifi]);
    if (s_ctx.wifi == WIFI_TEST_FAILED && s_ctx.fail_reason)
    {
        cJSON_AddStringToObject(obj, "reason", s_ctx.fail_reason);
    }
    if (s_ctx.wifi == WIFI_TEST_CONNECTED)
    {
        cJSON_AddStringToObject(obj, "ip", s_ctx.ip);
    }
    UNLOCK();
    return respond(obj, outbuf, outlen);
}

static esp_err_t handle_cloud(uint32_t session, const uint8_t *inbuf, ssize_t inlen,
                              uint8_t **outbuf, ssize_t *outlen, void *priv)
{
    cJSON *req = parse_request(inbuf, inlen);
    const char *broker_uri = json_string(req, "broker_uri");

    LOCK();
    if (!authorized(session))
    {
        UNLOCK();
        cJSON_Delete(req);
        return respond_error("not_confirmed", outbuf, outlen);
    }
    if (s_ctx.wifi != WIFI_TEST_CONNECTED)
    {
        /* Storing credentials that were never seen to work would brick the
         * node until the next pairing. */
        UNLOCK();
        cJSON_Delete(req);
        return respond_error("wifi_not_connected", outbuf, outlen);
    }
    if (s_ctx.finishing)
    {
        UNLOCK();
        cJSON_Delete(req);
        return respond_ok(outbuf, outlen); /* a retried write of the same step */
    }

    esp_err_t ret = broker_uri ? node_config_store(s_ctx.ssid, s_ctx.password, broker_uri)
                               : ESP_ERR_INVALID_ARG;
    if (ret == ESP_OK)
    {
        s_ctx.finishing = true;
    }
    UNLOCK();
    cJSON_Delete(req);

    if (ret == ESP_ERR_INVALID_ARG)
    {
        return respond_error("invalid_uri", outbuf, outlen);
    }
    if (ret != ESP_OK)
    {
        return respond_error("storage", outbuf, outlen);
    }

    ESP_LOGI(TAG, "Onboarding complete, ending the pairing window");
    esp_timer_start_once(s_ctx.finish_timer, FINISH_DELAY_US);
    return respond_ok(outbuf, outlen);
}

/* ------------------------------------------------------------------------- */
/* Events (default event loop task)                                          */
/* ------------------------------------------------------------------------- */

static void on_ble_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == PROTOCOMM_TRANSPORT_BLE_CONNECTED)
    {
        const protocomm_ble_event_t *event = data;
        if (!event)
        {
            return;
        }
        LOCK();
        s_ctx.connected = true;
        s_ctx.conn_handle = event->conn_handle;
        /* protocomm's BLE transport uses the connection handle as the
         * session id of everything that arrives over that connection. */
        s_ctx.session = event->conn_handle;
        s_ctx.awaiting = false;
        s_ctx.confirmed = false;
        set_state(PROVISIONING_STATE_CONNECTED);
        UNLOCK();

        esp_timer_stop(s_ctx.confirm_timer);
        esp_timer_start_once(s_ctx.confirm_timer, (uint64_t)s_ctx.cfg.confirm_timeout_s * 1000000ULL);
        ESP_LOGI(TAG, "Phone connected (conn %u)", event->conn_handle);
    }
    else if (id == PROTOCOMM_TRANSPORT_BLE_DISCONNECTED)
    {
        /* Posted twice per disconnect by the NimBLE transport, once without
         * data; handle it idempotently. */
        LOCK();
        bool was_connected = s_ctx.connected;
        bool finishing = s_ctx.finishing;
        s_ctx.connected = false;
        s_ctx.awaiting = false;
        s_ctx.confirmed = false;

        /* The test belonged to that connection. Whoever connects next has to
         * be confirmed and send credentials of its own; nothing a previous
         * phone left behind can be committed by bn-cloud. */
        bool drop_test = was_connected && !finishing && s_ctx.wifi != WIFI_TEST_IDLE;
        if (was_connected && !finishing)
        {
            s_ctx.wifi = WIFI_TEST_IDLE;
            s_ctx.fail_reason = NULL;
            s_ctx.ip[0] = '\0';
            memset(s_ctx.ssid, 0, sizeof(s_ctx.ssid));
            memset(s_ctx.password, 0, sizeof(s_ctx.password));
            set_state(PROVISIONING_STATE_ADVERTISING);
        }
        bool close = was_connected && s_ctx.window_expired && !finishing;
        UNLOCK();

        if (was_connected)
        {
            esp_timer_stop(s_ctx.confirm_timer);
            esp_timer_stop(s_ctx.overrun_timer);
            ESP_LOGI(TAG, "Phone disconnected");
        }
        if (drop_test)
        {
            esp_timer_stop(s_ctx.wifi_timer);
            esp_wifi_disconnect(); /* reason ASSOC_LEAVE: ignored by on_wifi_event() */
        }
        if (was_connected && finishing)
        {
            /* The app leaves once it has read the bn-cloud response, so there
             * is nothing left to wait for (finish_timer is only the cap). */
            ESP_LOGI(TAG, "Phone left after onboarding, ending the window");
            xEventGroupSetBits(s_ctx.events, DONE_BIT);
        }
        if (close)
        {
            ESP_LOGI(TAG, "Pairing window had expired, closing it now");
            close_window();
        }
    }
}

/** Map a disconnect reason to a bn-status reason; NULL = transient. */
static const char *classify_disconnect(uint8_t reason)
{
    switch (reason)
    {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return REASON_AUTH;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return REASON_NO_AP;
    default:
        return NULL;
    }
}

/** May a test that is still connecting try again after @p reason? (Lock held.) */
static bool worth_retrying(const char *reason)
{
    if (reason == REASON_AUTH)
    {
        return false; /* a wrong password stays wrong */
    }
    uint8_t limit = reason == REASON_NO_AP ? WIFI_TEST_NO_AP_RETRIES : WIFI_TEST_RETRIES;
    return s_ctx.retries < limit;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        const wifi_event_sta_disconnected_t *event = data;
        if (event->reason == WIFI_REASON_ASSOC_LEAVE)
        {
            return; /* our own esp_wifi_disconnect() */
        }

        bool retry = false;
        bool failed = false;
        LOCK();
        if (s_ctx.wifi == WIFI_TEST_CONNECTING || s_ctx.wifi == WIFI_TEST_CONNECTED)
        {
            const char *reason = classify_disconnect(event->reason);
            if (s_ctx.wifi == WIFI_TEST_CONNECTING && worth_retrying(reason))
            {
                s_ctx.retries++;
                retry = true;
            }
            else
            {
                s_ctx.wifi = WIFI_TEST_FAILED;
                s_ctx.fail_reason = reason ? reason : REASON_OTHER;
                set_state(PROVISIONING_STATE_WIFI_FAILED);
                failed = true;
                ESP_LOGW(TAG, "WiFi test failed: %s (reason %d)", s_ctx.fail_reason, event->reason);
            }
        }
        UNLOCK();

        if (retry)
        {
            esp_wifi_connect();
        }
        else if (failed)
        {
            esp_timer_stop(s_ctx.wifi_timer);
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t *event = data;

        /* Events are queued, so this may be the late lease of an association
         * a newer bn-wifi already replaced: only count it if the station is
         * still associated, and with the network under test. A hidden network
         * can report an empty SSID here, which is taken as a match. */
        wifi_ap_record_t ap;
        bool associated = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;

        bool passed = false;
        LOCK();
        if (s_ctx.wifi == WIFI_TEST_CONNECTING)
        {
            bool same = associated && (ap.ssid[0] == '\0' || strcmp((const char *)ap.ssid, s_ctx.ssid) == 0);
            if (same)
            {
                s_ctx.wifi = WIFI_TEST_CONNECTED;
                snprintf(s_ctx.ip, sizeof(s_ctx.ip), IPSTR, IP2STR(&event->ip_info.ip));
                set_state(PROVISIONING_STATE_WIFI_OK);
                passed = true;
                ESP_LOGI(TAG, "WiFi test passed, IP %s", s_ctx.ip);
            }
            else
            {
                ESP_LOGD(TAG, "Ignoring an IP from an association that is no longer under test");
            }
        }
        UNLOCK();

        if (passed)
        {
            esp_timer_stop(s_ctx.wifi_timer);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Timers (esp_timer task)                                                   */
/* ------------------------------------------------------------------------- */

static void on_window_timeout(void *arg)
{
    LOCK();
    bool close = !s_ctx.connected && !s_ctx.finishing;
    bool overrun = s_ctx.connected && !s_ctx.finishing;
    if (s_ctx.connected)
    {
        /* Don't cut off a user in the middle of onboarding: close once the
         * phone leaves (see on_ble_event), but no later than the overrun. */
        s_ctx.window_expired = true;
    }
    UNLOCK();

    if (overrun)
    {
        ESP_LOGI(TAG, "Pairing window expired with a phone connected, allowing it %d s more",
                 PROVISIONING_WINDOW_OVERRUN_S);
        esp_timer_start_once(s_ctx.overrun_timer, WINDOW_OVERRUN_US);
    }
    if (close)
    {
        ESP_LOGI(TAG, "Pairing window expired");
        close_window();
    }
}

static void on_overrun_timeout(void *arg)
{
    LOCK();
    bool drop = s_ctx.connected && !s_ctx.finishing;
    uint16_t conn_handle = s_ctx.conn_handle;
    UNLOCK();

    if (drop)
    {
        /* The disconnect closes the window (window_expired is set). */
        ESP_LOGW(TAG, "Phone still connected %d s after the window expired, dropping it",
                 PROVISIONING_WINDOW_OVERRUN_S);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void on_confirm_timeout(void *arg)
{
    LOCK();
    bool drop = s_ctx.connected && !s_ctx.confirmed;
    uint16_t conn_handle = s_ctx.conn_handle;
    UNLOCK();

    if (drop)
    {
        ESP_LOGW(TAG, "Connection not confirmed in time, dropping it");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void on_wifi_timeout(void *arg)
{
    LOCK();
    bool timed_out = s_ctx.wifi == WIFI_TEST_CONNECTING;
    if (timed_out)
    {
        s_ctx.wifi = WIFI_TEST_FAILED;
        s_ctx.fail_reason = REASON_TIMEOUT;
        set_state(PROVISIONING_STATE_WIFI_FAILED);
    }
    UNLOCK();

    if (timed_out)
    {
        ESP_LOGW(TAG, "WiFi test timed out");
        esp_wifi_disconnect();
    }
}

static void on_finish(void *arg)
{
    xEventGroupSetBits(s_ctx.events, DONE_BIT);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

void provisioning_confirm(void)
{
    if (!s_ctx.lock)
    {
        return;
    }

    LOCK();
    bool confirmed_now = s_ctx.connected && s_ctx.awaiting && !s_ctx.confirmed;
    if (confirmed_now)
    {
        s_ctx.confirmed = true;
        set_state(PROVISIONING_STATE_CONFIRMED);
    }
    UNLOCK();

    if (confirmed_now)
    {
        esp_timer_stop(s_ctx.confirm_timer);
        ESP_LOGI(TAG, "Connection confirmed by the user");
    }
}

static esp_err_t create_timer(esp_timer_cb_t cb, const char *name, esp_timer_handle_t *out)
{
    const esp_timer_create_args_t args = {.callback = cb, .name = name};
    return esp_timer_create(&args, out);
}

static void identity_init(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ctx.name, sizeof(s_ctx.name), NAME_PREFIX "%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(s_ctx.mac_str, sizeof(s_ctx.mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_ctx.node_id, sizeof(s_ctx.node_id), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    s_ctx.mfg_data[0] = MFG_COMPANY_ID_LO;
    s_ctx.mfg_data[1] = MFG_COMPANY_ID_HI;
    s_ctx.mfg_data[2] = 'B';
    s_ctx.mfg_data[3] = 'N';
    s_ctx.mfg_data[4] = MFG_FORMAT_VERSION;
    s_ctx.mfg_data[5] = node_config_is_provisioned() ? MFG_FLAG_PROVISIONED : 0;
    memcpy(&s_ctx.mfg_data[6], mac, 6);
}

static esp_err_t wifi_init(void)
{
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "esp_wifi_init failed");
    /* Credential storage is set in service_start(): the manager changes it
     * while it starts, so setting it here would not last. */

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                   on_wifi_event, NULL),
                        TAG, "handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   on_wifi_event, NULL),
                        TAG, "handler failed");
    return ESP_OK;
}

static esp_err_t service_start(void)
{
    network_prov_mgr_config_t mgr_cfg = {
        .scheme = network_prov_scheme_ble,
        /* No BT memory release on the way out: the node restarts instead. */
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
    };
    ESP_RETURN_ON_ERROR(network_prov_mgr_init(mgr_cfg), TAG, "manager init failed");

    static const char *capabilities[] = {APP_CAPABILITY};
    ESP_RETURN_ON_ERROR(network_prov_mgr_set_app_info("bn", esp_app_get_description()->version,
                                                      capabilities, 1),
                        TAG, "app info failed");
    ESP_RETURN_ON_ERROR(network_prov_scheme_ble_set_service_uuid(s_service_uuid), TAG, "uuid failed");
    ESP_RETURN_ON_ERROR(network_prov_scheme_ble_set_mfg_data(s_ctx.mfg_data, sizeof(s_ctx.mfg_data)),
                        TAG, "mfg data failed");

    static const char *const endpoints[] = {EP_CONFIRM, EP_INFO, EP_WIFI, EP_STATUS, EP_CLOUD};
    for (size_t i = 0; i < sizeof(endpoints) / sizeof(endpoints[0]); i++)
    {
        ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_create(endpoints[i]), TAG,
                            "endpoint %s failed", endpoints[i]);
    }

    /* Security 1 without a PoP - see "Security model" in the header. */
    ESP_RETURN_ON_ERROR(network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, NULL, s_ctx.name, NULL),
                        TAG, "start failed");

    /* The manager always serves prov-config and prov-ctrl, and answers them on
     * any session, confirmed or not. prov-config would apply WiFi credentials
     * on the manager's own terms - past the button, into the driver's NVS, and
     * with its auto-stop tearing BLE down under a provisioning_run() that then
     * never returns. The app uses neither, so both go. Their characteristics
     * stay in the GATT table; a write to one now fails with an ATT error. They
     * were reachable only for the moment since start_provisioning() returned,
     * far too short for a phone to connect and set up a session. */
    network_prov_mgr_endpoint_unregister("prov-config");
    network_prov_mgr_endpoint_unregister("prov-ctrl");

    /* start_provisioning() switches credential storage back to FLASH on its
     * way out (its restore_wifi_flag path, also on success), so without this
     * every network bn-wifi tests would land in the driver's NVS - wrong
     * passwords included, and still there after an abandoned re-pairing.
     * node_config is the only place credentials are stored, and only once
     * they have been seen to work. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set_storage failed");

    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_register(EP_CONFIRM, handle_confirm, NULL), TAG, "register failed");
    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_register(EP_INFO, handle_info, NULL), TAG, "register failed");
    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_register(EP_WIFI, handle_wifi, NULL), TAG, "register failed");
    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_register(EP_STATUS, handle_status, NULL), TAG, "register failed");
    ESP_RETURN_ON_ERROR(network_prov_mgr_endpoint_register(EP_CLOUD, handle_cloud, NULL), TAG, "register failed");
    return ESP_OK;
}

esp_err_t provisioning_run(const provisioning_config_t *config, provisioning_result_t *result)
{
    ESP_RETURN_ON_FALSE(config && result, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(!s_ran, ESP_ERR_INVALID_STATE, TAG, "already run in this boot");
    s_ran = true;

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.cfg = *config;
    if (s_ctx.cfg.window_s == 0)
    {
        s_ctx.cfg.window_s = PROVISIONING_DEFAULT_WINDOW_S;
    }
    if (s_ctx.cfg.confirm_timeout_s == 0)
    {
        s_ctx.cfg.confirm_timeout_s = PROVISIONING_DEFAULT_CONFIRM_TIMEOUT_S;
    }
    strlcpy(s_ctx.hw_revision, config->hw_revision ? config->hw_revision : "", sizeof(s_ctx.hw_revision));
    s_ctx.cfg.hw_revision = s_ctx.hw_revision;

    s_ctx.lock = xSemaphoreCreateMutex();
    s_ctx.events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_ctx.lock && s_ctx.events, ESP_ERR_NO_MEM, TAG, "out of memory");
    ESP_RETURN_ON_ERROR(create_timer(on_window_timeout, "prov_window", &s_ctx.window_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(create_timer(on_confirm_timeout, "prov_confirm", &s_ctx.confirm_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(create_timer(on_wifi_timeout, "prov_wifi", &s_ctx.wifi_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(create_timer(on_finish, "prov_finish", &s_ctx.finish_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(create_timer(on_overrun_timeout, "prov_overrun", &s_ctx.overrun_timer), TAG, "timer");

    identity_init();
    ESP_RETURN_ON_ERROR(wifi_init(), TAG, "WiFi setup failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID,
                                                   on_ble_event, NULL),
                        TAG, "handler failed");

    /* The first callback goes out explicitly: set_state() skips "no change",
     * and the zeroed state already reads as ADVERTISING. */
    s_ctx.state = PROVISIONING_STATE_ADVERTISING;
    if (s_ctx.cfg.on_state)
    {
        s_ctx.cfg.on_state(s_ctx.state, s_ctx.cfg.user_ctx);
    }

    ESP_RETURN_ON_ERROR(service_start(), TAG, "provisioning service failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_once(s_ctx.window_timer, (uint64_t)s_ctx.cfg.window_s * 1000000ULL),
                        TAG, "window timer failed");
    ESP_LOGI(TAG, "Pairing as '%s' for %lu s", s_ctx.name, (unsigned long)s_ctx.cfg.window_s);

    EventBits_t bits = xEventGroupWaitBits(s_ctx.events, DONE_BIT | WINDOW_CLOSED_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    esp_timer_stop(s_ctx.window_timer);
    esp_timer_stop(s_ctx.confirm_timer);
    esp_timer_stop(s_ctx.wifi_timer);
    esp_timer_stop(s_ctx.overrun_timer);
    esp_timer_stop(s_ctx.finish_timer);
    /* Stops advertising and the GATT server, asynchronously. */
    network_prov_mgr_stop_provisioning();

    *result = (bits & DONE_BIT) ? PROVISIONING_RESULT_STORED : PROVISIONING_RESULT_TIMEOUT;
    if (*result == PROVISIONING_RESULT_TIMEOUT)
    {
        /* Nothing was stored, so the caller may stay up without a restart (an
         * unprovisioned node goes idle): don't leave the station associated
         * with a network that was only under test, or the radio on for nothing. */
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    return ESP_OK;
}
