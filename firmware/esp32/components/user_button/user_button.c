/**
 * @file user_button.c
 * @brief Gesture decoding on top of iot_button.
 */

#include "user_button.h"

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "button_gpio.h"
#include "iot_button.h"

static const char *TAG = "USER_BUTTON";

/* Release classification (on_release) assumes the thresholds are ordered. */
_Static_assert(CONFIG_USER_BUTTON_RESET_HOLD_MS > CONFIG_USER_BUTTON_PAIRING_HOLD_MS,
               "USER_BUTTON_RESET_HOLD_MS must be longer than USER_BUTTON_PAIRING_HOLD_MS");

static struct
{
    button_handle_t btn;
    user_button_cb_t cb;
    void *user_ctx;
} s_ctx = {0};

static void emit(user_button_event_t event)
{
    s_ctx.cb(event, s_ctx.user_ctx);
}

static void on_pairing_armed(void *btn, void *usr_data)
{
    emit(USER_BUTTON_PAIRING_ARMED);
}

static void on_reset_armed(void *btn, void *usr_data)
{
    emit(USER_BUTTON_RESET_ARMED);
}

/* Classify by how long the button was down, see the header for why actions
 * wait for the release. */
static void on_release(void *btn, void *usr_data)
{
    uint32_t held_ms = iot_button_get_pressed_time(s_ctx.btn);

    if (held_ms >= CONFIG_USER_BUTTON_RESET_HOLD_MS)
    {
        ESP_LOGI(TAG, "Held %lu ms: factory reset", (unsigned long)held_ms);
        emit(USER_BUTTON_RESET_HOLD);
    }
    else if (held_ms >= CONFIG_USER_BUTTON_PAIRING_HOLD_MS)
    {
        ESP_LOGI(TAG, "Held %lu ms: pairing", (unsigned long)held_ms);
        emit(USER_BUTTON_PAIRING_HOLD);
    }
    else
    {
        emit(USER_BUTTON_CLICK);
    }
}

esp_err_t user_button_init(user_button_cb_t cb, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(cb, ESP_ERR_INVALID_ARG, TAG, "cb is NULL");
    ESP_RETURN_ON_FALSE(!s_ctx.btn, ESP_ERR_INVALID_STATE, TAG, "already initialized");

    s_ctx.cb = cb;
    s_ctx.user_ctx = user_ctx;

    /* long_press_time is the first long-press threshold; the reset threshold
     * is a second BUTTON_LONG_PRESS_START registration with its own press_time.
     * iot_button walks those registrations in order, so they must be added in
     * ascending press_time - pairing first. */
    const button_config_t btn_cfg = {
        .long_press_time = CONFIG_USER_BUTTON_PAIRING_HOLD_MS,
    };
    const button_gpio_config_t gpio_cfg = {
        .gpio_num = CONFIG_USER_BUTTON_GPIO,
        .active_level = CONFIG_USER_BUTTON_ACTIVE_LOW ? 0 : 1,
    };
    ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &s_ctx.btn), TAG,
                        "button on GPIO%d failed", CONFIG_USER_BUTTON_GPIO);

    button_event_args_t pairing_args = {.long_press.press_time = CONFIG_USER_BUTTON_PAIRING_HOLD_MS};
    button_event_args_t reset_args = {.long_press.press_time = CONFIG_USER_BUTTON_RESET_HOLD_MS};

    esp_err_t ret = iot_button_register_cb(s_ctx.btn, BUTTON_LONG_PRESS_START, &pairing_args,
                                           on_pairing_armed, NULL);
    if (ret == ESP_OK)
    {
        ret = iot_button_register_cb(s_ctx.btn, BUTTON_LONG_PRESS_START, &reset_args,
                                     on_reset_armed, NULL);
    }
    if (ret == ESP_OK)
    {
        ret = iot_button_register_cb(s_ctx.btn, BUTTON_PRESS_UP, NULL, on_release, NULL);
    }
    if (ret != ESP_OK)
    {
        iot_button_delete(s_ctx.btn);
        s_ctx.btn = NULL;
        ESP_RETURN_ON_ERROR(ret, TAG, "callback registration failed");
    }

    ESP_LOGI(TAG, "Button on GPIO%d: hold %d ms = pairing, %d ms = factory reset",
             CONFIG_USER_BUTTON_GPIO, CONFIG_USER_BUTTON_PAIRING_HOLD_MS,
             CONFIG_USER_BUTTON_RESET_HOLD_MS);
    return ESP_OK;
}
