/**
 * @file status_led.c
 * @brief Status LED patterns, rendered by a dedicated low-priority task.
 */

#include "status_led.h"

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdbool.h>

#if CONFIG_STATUS_LED_TYPE_WS2812
#include "led_strip.h"
#elif CONFIG_STATUS_LED_TYPE_GPIO
#include "driver/gpio.h"
#endif

static const char *TAG = "STATUS_LED";

#define TICK_MS 50
#define ONLINE_SHOW_MS 3000
#define TASK_STACK_SIZE 2048
#define TASK_PRIORITY 2

typedef struct
{
    uint8_t r, g, b;
} rgb_t;

static atomic_int s_pattern = STATUS_LED_OFF;
static bool s_initialized;

#if CONFIG_STATUS_LED_TYPE_WS2812
static led_strip_handle_t s_strip;
#endif

/** Turn the LED on in @p color (an RGB LED) or simply on (a GPIO LED), or off. */
static void led_write(bool on, rgb_t color)
{
#if CONFIG_STATUS_LED_TYPE_WS2812
    if (on)
    {
        const uint32_t k = CONFIG_STATUS_LED_BRIGHTNESS;
        led_strip_set_pixel(s_strip, 0, color.r * k / 255, color.g * k / 255, color.b * k / 255);
        led_strip_refresh(s_strip);
    }
    else
    {
        led_strip_clear(s_strip);
    }
#elif CONFIG_STATUS_LED_TYPE_GPIO
    (void)color;
    gpio_set_level(CONFIG_STATUS_LED_GPIO, on == CONFIG_STATUS_LED_GPIO_ACTIVE_HIGH);
#else
    (void)on;
    (void)color;
#endif
}

/**
 * @brief What @p pattern looks like @p elapsed_ms after it was set.
 *
 * @param[out] color  Colour to show while on.
 * @return Whether the LED is lit at that moment.
 */
static bool render(status_led_pattern_t pattern, uint32_t elapsed_ms, rgb_t *color)
{
    static const rgb_t BLUE = {0, 0, 255}, YELLOW = {255, 160, 0}, GREEN = {0, 255, 0},
                       RED = {255, 0, 0}, CYAN = {0, 200, 255};
    const bool slow = (elapsed_ms / 500) % 2 == 0; /* 1 Hz, 50 % */
    const bool fast = (elapsed_ms / 100) % 2 == 0; /* 5 Hz, 50 % */

    switch (pattern)
    {
    case STATUS_LED_WIFI_CONNECTING:
        *color = YELLOW;
        return slow;
    case STATUS_LED_PAIRING:
        *color = BLUE;
        return slow;
    case STATUS_LED_AWAIT_CONFIRM:
        *color = BLUE;
        return fast;
    case STATUS_LED_PAIRED:
        *color = BLUE;
        return true;
    case STATUS_LED_ONLINE:
        *color = GREEN;
        return elapsed_ms < ONLINE_SHOW_MS;
    case STATUS_LED_ERROR:
        *color = RED;
        return fast;
    case STATUS_LED_HOLD_PAIRING:
        *color = CYAN;
        return true;
    case STATUS_LED_HOLD_RESET:
        *color = RED;
        return true;
    case STATUS_LED_OFF:
    default:
        return false;
    }
}

static void render_task(void *arg)
{
    status_led_pattern_t shown = STATUS_LED_OFF;
    uint32_t elapsed_ms = 0;
    int last_written = -1; /* -1 forces the first write; then 0 = off, 1 = on */
    rgb_t last_color = {0};

    while (true)
    {
        status_led_pattern_t pattern = (status_led_pattern_t)atomic_load(&s_pattern);
        if (pattern != shown)
        {
            shown = pattern;
            elapsed_ms = 0;
        }

        rgb_t color = {0};
        bool on = render(shown, elapsed_ms, &color);
        /* Only touch the driver on a change: a WS2812 refresh is an RMT
         * transaction, not worth repeating 20 times a second for a solid LED. */
        bool color_changed = color.r != last_color.r || color.g != last_color.g || color.b != last_color.b;
        if ((int)on != last_written || (on && color_changed))
        {
            led_write(on, color);
            last_written = on;
            last_color = color;
        }

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        /* Saturate instead of wrapping: only ONLINE looks at large values. */
        if (elapsed_ms < UINT32_MAX - TICK_MS)
        {
            elapsed_ms += TICK_MS;
        }
    }
}

esp_err_t status_led_init(void)
{
    ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG, "already initialized");

#if CONFIG_STATUS_LED_TYPE_WS2812
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = CONFIG_STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip), TAG,
                        "WS2812 on GPIO%d failed", CONFIG_STATUS_LED_GPIO);
    led_strip_clear(s_strip);
#elif CONFIG_STATUS_LED_TYPE_GPIO
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "GPIO%d config failed", CONFIG_STATUS_LED_GPIO);
    gpio_set_level(CONFIG_STATUS_LED_GPIO, !CONFIG_STATUS_LED_GPIO_ACTIVE_HIGH);
#else
    s_initialized = true;
    return ESP_OK; /* No LED: no task either. */
#endif

#if !CONFIG_STATUS_LED_TYPE_NONE
    BaseType_t ok = xTaskCreate(render_task, "status_led", TASK_STACK_SIZE, NULL, TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "render task creation failed");
    s_initialized = true;
    return ESP_OK;
#endif
}

void status_led_set(status_led_pattern_t pattern)
{
    atomic_store(&s_pattern, (int)pattern);
}
