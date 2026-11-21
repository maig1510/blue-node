/**
 * @file node_config.c
 * @brief NVS-backed node configuration.
 */

#include "node_config.h"

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "NODE_CONFIG";

#define NAMESPACE "bn_cfg"
#define KEY_CONFIG "config"
#define KEY_PAIRING_REQ "pairing_req"

/* Bump whenever node_config_t changes layout: a blob of another version (or
 * size) is ignored rather than misread, and the node comes up unprovisioned. */
#define CONFIG_BLOB_VERSION 1

/** What is persisted under KEY_CONFIG: one blob, so one atomic NVS write. */
typedef struct
{
    uint8_t version;
    node_config_t cfg;
} config_blob_t;

static struct
{
    node_config_t cfg;
    bool initialized;
} s_ctx = {0};

/** Force NUL termination, so a blob from a buggy writer cannot overrun a reader. */
static void terminate_strings(node_config_t *cfg)
{
    cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';
    cfg->wifi_password[sizeof(cfg->wifi_password) - 1] = '\0';
    cfg->broker_uri[sizeof(cfg->broker_uri) - 1] = '\0';
}

esp_err_t node_config_init(void)
{
    ESP_RETURN_ON_FALSE(!s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "already initialized");
    memset(&s_ctx.cfg, 0, sizeof(s_ctx.cfg));

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND)
    {
        /* Never written: a fresh or factory-reset node. */
        s_ctx.initialized = true;
        ESP_LOGI(TAG, "No stored configuration");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs_open failed");

    config_blob_t blob;
    size_t len = sizeof(blob);
    ret = nvs_get_blob(nvs, KEY_CONFIG, &blob, &len);
    nvs_close(nvs);

    if (ret == ESP_OK && len == sizeof(blob) && blob.version == CONFIG_BLOB_VERSION)
    {
        terminate_strings(&blob.cfg);
        s_ctx.cfg = blob.cfg;
    }
    else if (ret == ESP_OK || ret == ESP_ERR_NVS_INVALID_LENGTH)
    {
        ESP_LOGW(TAG, "Stored configuration has an unknown layout, ignoring it");
    }
    else if (ret != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_RETURN_ON_ERROR(ret, TAG, "reading configuration failed");
    }

    s_ctx.initialized = true;
    /* The password is deliberately never logged. */
    ESP_LOGI(TAG, "Loaded: ssid '%s', broker '%s'", s_ctx.cfg.wifi_ssid, s_ctx.cfg.broker_uri);
    return ESP_OK;
}

const node_config_t *node_config_get(void)
{
    return &s_ctx.cfg;
}

bool node_config_is_provisioned(void)
{
    return s_ctx.cfg.wifi_ssid[0] != '\0' && s_ctx.cfg.broker_uri[0] != '\0';
}

static bool valid_broker_uri(const char *uri)
{
    return strncmp(uri, "mqtt://", 7) == 0 || strncmp(uri, "mqtts://", 8) == 0;
}

esp_err_t node_config_store(const char *ssid, const char *password, const char *broker_uri)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "node_config_init() not called");
    ESP_RETURN_ON_FALSE(ssid && password && broker_uri, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    size_t ssid_len = strlen(ssid);
    ESP_RETURN_ON_FALSE(ssid_len > 0 && ssid_len <= NODE_CONFIG_SSID_MAX_LEN,
                        ESP_ERR_INVALID_ARG, TAG, "SSID length %u out of range", (unsigned)ssid_len);
    ESP_RETURN_ON_FALSE(strlen(password) <= NODE_CONFIG_PASSWORD_MAX_LEN,
                        ESP_ERR_INVALID_ARG, TAG, "password too long");
    ESP_RETURN_ON_FALSE(strlen(broker_uri) <= NODE_CONFIG_BROKER_URI_MAX_LEN,
                        ESP_ERR_INVALID_ARG, TAG, "broker URI too long");
    ESP_RETURN_ON_FALSE(valid_broker_uri(broker_uri), ESP_ERR_INVALID_ARG, TAG,
                        "broker URI must start with mqtt:// or mqtts://");

    config_blob_t blob = {.version = CONFIG_BLOB_VERSION};
    strlcpy(blob.cfg.wifi_ssid, ssid, sizeof(blob.cfg.wifi_ssid));
    strlcpy(blob.cfg.wifi_password, password, sizeof(blob.cfg.wifi_password));
    strlcpy(blob.cfg.broker_uri, broker_uri, sizeof(blob.cfg.broker_uri));

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs_open failed");
    esp_err_t ret = nvs_set_blob(nvs, KEY_CONFIG, &blob, sizeof(blob));
    if (ret == ESP_OK)
    {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(ret, TAG, "writing configuration failed");

    s_ctx.cfg = blob.cfg;
    ESP_LOGI(TAG, "Stored: ssid '%s', broker '%s'", ssid, broker_uri);
    return ESP_OK;
}

esp_err_t node_config_request_pairing(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs_open failed");
    esp_err_t ret = nvs_set_u8(nvs, KEY_PAIRING_REQ, 1);
    if (ret == ESP_OK)
    {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

bool node_config_take_pairing_request(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
    {
        return false;
    }

    uint8_t requested = 0;
    bool found = nvs_get_u8(nvs, KEY_PAIRING_REQ, &requested) == ESP_OK;
    if (found)
    {
        /* Clear before acting on it, so a crash inside the pairing window
         * cannot turn the request into a boot loop. */
        nvs_erase_key(nvs, KEY_PAIRING_REQ);
        nvs_commit(nvs);
    }
    nvs_close(nvs);
    return found && requested;
}

esp_err_t node_config_factory_reset(void)
{
    /* Deinit first: erasing a partition NVS still has open would leave its
     * in-RAM page cache describing pages that no longer exist. */
    esp_err_t ret = nvs_flash_deinit();
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_INITIALIZED)
    {
        ESP_RETURN_ON_ERROR(ret, TAG, "nvs_flash_deinit failed");
    }
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase failed");

    memset(&s_ctx.cfg, 0, sizeof(s_ctx.cfg));
    ESP_LOGW(TAG, "Factory reset: NVS erased");
    return ESP_OK;
}
