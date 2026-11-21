#pragma once

/**
 * @file user_button.h
 * @brief The node's single button, reduced to the gestures the node acts on.
 *
 * Thin layer over espressif/button (iot_button): debouncing and timing are
 * that component's job; this one turns raw press/release timing into five
 * events with a fixed meaning:
 *
 *   Event                     | When                                 | Typical use
 *   --------------------------+--------------------------------------+---------------------
 *   USER_BUTTON_CLICK         | released before the pairing hold     | confirm pairing
 *   USER_BUTTON_PAIRING_ARMED | still held, pairing hold reached     | LED feedback
 *   USER_BUTTON_RESET_ARMED   | still held, reset hold reached       | LED feedback
 *   USER_BUTTON_PAIRING_HOLD  | released between the two hold times  | enter pairing
 *   USER_BUTTON_RESET_HOLD    | released after the reset hold        | factory reset
 *
 * Actions fire on *release*, not when a threshold is crossed: otherwise the
 * pairing action would always win, because holding for the reset necessarily
 * passes the pairing threshold first. The *_ARMED events exist so the LED can
 * tell the user, while they are still holding, which action letting go now
 * would trigger.
 *
 * GPIO, polarity and both hold times come from Kconfig (menu "User button").
 *
 * @section user_button_threading Threading
 *
 * The callback runs on the esp_timer task, which iot_button polls the button
 * from. Keep it short and non-blocking - other esp_timer callbacks (including
 * the system's) wait while it runs. Restarting the chip from it is fine.
 */

#include "esp_err.h"

/** Gestures, see the table in the file comment. */
typedef enum
{
    USER_BUTTON_CLICK,
    USER_BUTTON_PAIRING_ARMED,
    USER_BUTTON_RESET_ARMED,
    USER_BUTTON_PAIRING_HOLD,
    USER_BUTTON_RESET_HOLD,
} user_button_event_t;

/**
 * @brief Called for every gesture.
 *
 * @param[in] event     What happened.
 * @param[in] user_ctx  The pointer passed to user_button_init().
 */
typedef void (*user_button_cb_t)(user_button_event_t event, void *user_ctx);

/**
 * @brief Start watching the button.
 *
 * @param[in] cb        Gesture callback. Must not be NULL.
 * @param[in] user_ctx  Opaque pointer handed back to @p cb.
 *
 * @return ESP_OK                 Watching.
 * @return ESP_ERR_INVALID_ARG    @p cb is NULL.
 * @return ESP_ERR_INVALID_STATE  Already initialised.
 * @return other                  Propagated from iot_button.
 */
esp_err_t user_button_init(user_button_cb_t cb, void *user_ctx);
