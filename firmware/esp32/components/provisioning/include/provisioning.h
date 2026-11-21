#pragma once

/**
 * @file provisioning.h
 * @brief BLE pairing: the node's half of onboarding.
 *
 * Runs one pairing window: the node advertises over BLE as "BN-XXXXXX" (the
 * low three MAC bytes), a phone with the blue-node app connects, the user
 * confirms with a click on the node's button, and the app hands over WiFi
 * credentials and the broker address. The result is stored through
 * node_config; the caller restarts the node to run with it.
 *
 * Transport and session security come from espressif/network_provisioning:
 * the BLE GATT service, protocomm security 1 (X25519 key exchange + AES-CTR,
 * so nothing crosses the air in clear text), and its standard "prov-scan"
 * endpoint, which the app uses to list WiFi networks. Everything else is done
 * through this component's own endpoints, which carry small JSON documents:
 *
 *   Endpoint    | Request                      | Response
 *   ------------+------------------------------+-------------------------------------------
 *   bn-confirm  | {}                           | {"state":"pending"|"confirmed"}
 *   bn-info     | {}                           | {"v":1,"mac":"A4:C1:..","node_id":"A4C1..",
 *               |                              |  "fw":"..","hw":"..","provisioned":bool}
 *   bn-wifi     | {"ssid":"..","password":".."}| {"ok":true} | {"ok":false,"error":".."}
 *   bn-status   | {}                           | {"wifi":"idle|connecting|connected|failed",
 *               |                              |  "reason":"auth|no_ap|timeout|other","ip":".."}
 *   bn-cloud    | {"broker_uri":"mqtt://.."}   | {"ok":true} | {"ok":false,"error":".."}
 *
 * Every endpoint except bn-confirm answers {"ok":false,"error":"not_confirmed"}
 * until the user has confirmed *this* connection with a click. Other errors:
 * bn-wifi "invalid" (SSID or password cannot be a WiFi network - the password
 * is empty, 8..63 characters, or 64 hex digits) and "busy" (the radio refused
 * to start the test); bn-cloud "wifi_not_connected", "invalid_uri", "storage".
 *
 * A WiFi test belongs to the connection that started it: when the phone
 * disconnects, the test is dropped and bn-status reads "idle" again, so a new
 * connection always starts over with bn-wifi. Once bn-cloud has stored the
 * result, bn-wifi and bn-cloud only answer {"ok":true}, and the window ends
 * when the phone disconnects (or a few seconds later at most).
 *
 * @section prov_why_custom Why WiFi goes through bn-wifi, not the standard endpoint
 *
 * The standard "prov-config" endpoint makes the manager apply credentials and
 * connect on its own, with no hook to (a) refuse them until the button
 * confirmation, (b) keep the result pending while the app registers the node
 * with the backend, or (c) report the failure reason in this protocol's terms.
 * It is therefore unregistered right after the service starts, together with
 * "prov-ctrl" (the manager's reset / re-provision commands for prov-config);
 * left in place, any phone could use prov-config without a click. The manager
 * only reacts to WiFi/IP events once credentials arrived through prov-config
 * (prov_state >= CRED_RECV in its manager.c), which now cannot happen, so
 * testing the credentials ourselves does not confuse it.
 *
 * @section prov_security Security model
 *
 * Security 1 is used without a proof-of-possession: the node has no display
 * and no printed code, so the phone has nothing to prove. What authorises a
 * phone instead is physical presence:
 *
 *   - the window only opens on a factory-fresh node or after a 3 s button hold,
 *     and closes after window_s (300 s by default). A phone that is connected
 *     when it expires may finish, but is dropped PROVISIONING_WINDOW_OVERRUN_S
 *     later at the latest, so
 *     an app left open cannot keep a node out of normal operation;
 *   - NimBLE accepts one connection at a time and stops advertising while it
 *     lasts (CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1);
 *   - nothing is accepted until a button click confirms that connection, and a
 *     connection that is not confirmed within confirm_timeout_s is dropped, so
 *     a stranger's phone cannot sit on the only slot;
 *   - the manager's own prov-config / prov-ctrl endpoints are removed (see
 *     above), so every path to the node's configuration goes through the click.
 *
 * The session is still encrypted, so the WiFi password cannot be sniffed.
 * Known gaps, accepted for now:
 *   - an active man-in-the-middle during the window is not stopped; a PoP
 *     printed on the node (security 2) is the upgrade path for that;
 *   - "prov-scan" answers before the click as well: the manager does not
 *     expose its handler's context, so it cannot be wrapped in the check. An
 *     unconfirmed phone can therefore list the WiFi networks around the node
 *     for up to confirm_timeout_s;
 *   - a stranger who keeps reconnecting can keep taking the only slot, a
 *     confirm_timeout_s at a time, until the window ends. That denies pairing,
 *     nothing more.
 *
 * @section prov_threading Threading
 *
 * provisioning_run() blocks the calling task for the whole window. Endpoint
 * handlers run on the NimBLE host task; BLE, WiFi and IP events on the default
 * event loop task; timeouts on the esp_timer task. Shared state is guarded by
 * one mutex inside this component. provisioning_confirm() may be called from
 * any task or esp_timer callback (the button's callback runs on esp_timer).
 */

#include "esp_err.h"
#include <stdint.h>

/** Default pairing window, used when provisioning_config_t::window_s is 0. */
#define PROVISIONING_DEFAULT_WINDOW_S 300

/** Default confirmation timeout, used when provisioning_config_t::confirm_timeout_s is 0. */
#define PROVISIONING_DEFAULT_CONFIRM_TIMEOUT_S 60

/**
 * How long a phone still connected when the window expires may keep the node
 * in pairing before it is dropped. Long enough to finish an onboarding that was
 * nearly done; short enough that an app left open does not keep a node offline.
 */
#define PROVISIONING_WINDOW_OVERRUN_S 180

/**
 * @brief Where the pairing flow is, reported through provisioning_config_t::on_state.
 *
 * Meant for user feedback (the status LED), not for control flow.
 */
typedef enum
{
    PROVISIONING_STATE_ADVERTISING,   /**< Discoverable, no phone connected. */
    PROVISIONING_STATE_CONNECTED,     /**< A phone connected; it has not asked for confirmation yet. */
    PROVISIONING_STATE_AWAIT_CONFIRM, /**< The phone asked; waiting for a button click. */
    PROVISIONING_STATE_CONFIRMED,     /**< Confirmed; the phone may configure the node. */
    PROVISIONING_STATE_WIFI_TESTING,  /**< Trying the WiFi credentials the phone sent. */
    PROVISIONING_STATE_WIFI_FAILED,   /**< That attempt failed; the phone may send others. */
    PROVISIONING_STATE_WIFI_OK,       /**< WiFi works; waiting for the broker address. */
} provisioning_state_t;

/** How a pairing window ended. */
typedef enum
{
    PROVISIONING_RESULT_STORED,  /**< A new configuration was stored through node_config. */
    PROVISIONING_RESULT_TIMEOUT, /**< The window closed with nothing stored. */
} provisioning_result_t;

/**
 * @brief Progress callback.
 *
 * Runs with this component's lock held, on whichever task caused the change
 * (see @ref prov_threading). Keep it to something like status_led_set(): it
 * must not block and must not call back into this component.
 */
typedef void (*provisioning_state_cb_t)(provisioning_state_t state, void *user_ctx);

/** Configuration passed to provisioning_run(). Copied; need not outlive the call. */
typedef struct
{
    uint32_t window_s;                /**< How long the node stays discoverable.
                                           0 = PROVISIONING_DEFAULT_WINDOW_S. A window
                                           that expires while a phone is connected
                                           closes when that phone disconnects, or
                                           PROVISIONING_WINDOW_OVERRUN_S later at the
                                           latest (the phone is
                                           dropped then, unless the result is
                                           already being stored). */
    uint32_t confirm_timeout_s;       /**< How long a connected phone has to get its
                                           connection confirmed before the node drops it.
                                           0 = PROVISIONING_DEFAULT_CONFIRM_TIMEOUT_S. */
    const char *hw_revision;          /**< Reported as "hw" by bn-info. NULL = "". Copied. */
    provisioning_state_cb_t on_state; /**< Progress callback, NULL to skip. */
    void *user_ctx;                   /**< Passed to on_state. */
} provisioning_config_t;

/**
 * @brief Run one pairing window and block until it ends.
 *
 * Preconditions:
 *   - nvs_flash_init(), node_config_init(), esp_netif_init() and
 *     esp_event_loop_create_default() have been called;
 *   - WiFi has not been initialised yet - this function sets it up in STA mode
 *     itself (for prov-scan and for testing credentials).
 *
 * After PROVISIONING_RESULT_STORED the node is expected to restart: WiFi and
 * the NimBLE stack are left initialised, because tearing them down cleanly
 * just before a restart buys nothing. After PROVISIONING_RESULT_TIMEOUT the
 * caller may also stay up (main.c does on an unprovisioned node): WiFi is
 * stopped and the BLE service is shut down in the background, but neither is
 * deinitialised. Pairing is entered through a restart (see main.c), so this
 * runs at most once per boot.
 *
 * @param[in]  config  Window settings and progress callback. Must not be NULL.
 * @param[out] result  How the window ended. Must not be NULL.
 *
 * @return ESP_OK                 The window ran; see @p result.
 * @return ESP_ERR_INVALID_ARG    A NULL argument.
 * @return ESP_ERR_INVALID_STATE  Already run in this boot.
 * @return ESP_ERR_NO_MEM         Out of memory for the lock, events or timers.
 * @return other                  Propagated from WiFi or network_provisioning
 *                                 initialisation.
 */
esp_err_t provisioning_run(const provisioning_config_t *config, provisioning_result_t *result);

/**
 * @brief The user clicked the button: confirm the phone that is waiting.
 *
 * Confirms only a connection that has already asked (bn-confirm), so a click
 * before any phone is connected does not pre-authorise whichever phone comes
 * next. A no-op outside provisioning_run() or when nothing is waiting.
 */
void provisioning_confirm(void);
