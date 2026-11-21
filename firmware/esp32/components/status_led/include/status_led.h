#pragma once

/**
 * @file status_led.h
 * @brief One status LED that shows what the node is doing.
 *
 * The application sets a *pattern* - a meaning, not a colour - and a small
 * task of this component renders it. Patterns are chosen so they can be told
 * apart on a plain single-colour LED by rhythm alone; on an RGB LED the colour
 * adds a second cue:
 *
 *   Pattern                 | Rhythm                   | WS2812 colour
 *   ------------------------+--------------------------+--------------
 *   OFF                     | off                      | -
 *   WIFI_CONNECTING         | slow blink (1 Hz)        | yellow
 *   PAIRING                 | slow blink (1 Hz)        | blue
 *   AWAIT_CONFIRM           | fast blink (5 Hz)        | blue
 *   PAIRED                  | solid                    | blue
 *   ONLINE                  | solid 3 s, then off      | green
 *   ERROR                   | fast blink (5 Hz)        | red
 *   HOLD_PAIRING            | solid                    | cyan
 *   HOLD_RESET              | solid                    | red
 *
 * HOLD_* are the feedback while the button is held: the LED changes the moment
 * a hold threshold is crossed, so the user knows when to let go.
 *
 * The LED type and pin come from Kconfig (menu "Status LED"); with
 * CONFIG_STATUS_LED_TYPE_NONE every call is a cheap no-op, so callers never
 * need to know whether a board has an LED.
 *
 * @section status_led_threading Threading
 *
 * status_led_set() may be called from any task or esp_timer callback (not
 * from an ISR): it only stores the pattern in an atomic, which the render task
 * picks up on its next tick (every 50 ms).
 */

#include "esp_err.h"

/** What the LED should say. See the table in the file comment. */
typedef enum
{
    STATUS_LED_OFF,
    STATUS_LED_WIFI_CONNECTING,
    STATUS_LED_PAIRING,
    STATUS_LED_AWAIT_CONFIRM,
    STATUS_LED_PAIRED,
    STATUS_LED_ONLINE,
    STATUS_LED_ERROR,
    STATUS_LED_HOLD_PAIRING,
    STATUS_LED_HOLD_RESET,
} status_led_pattern_t;

/**
 * @brief Set up the LED and start the render task.
 *
 * Starts in STATUS_LED_OFF. Call once at boot.
 *
 * @return ESP_OK                 Ready (also when the LED type is NONE).
 * @return ESP_ERR_INVALID_STATE  Already initialised.
 * @return ESP_ERR_NO_MEM         The render task could not be created.
 * @return other                  Propagated from the LED driver.
 */
esp_err_t status_led_init(void);

/**
 * @brief Show a pattern from now on.
 *
 * Setting the pattern that is already showing does not restart it, so ONLINE's
 * 3 s countdown is not reset by a redundant call.
 *
 * @param[in] pattern  Pattern to show.
 */
void status_led_set(status_led_pattern_t pattern);
