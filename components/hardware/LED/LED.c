#include "LED.h"
#include <stdbool.h>
#include "esp_err.h"
#include "led_strip.h"

static led_strip_handle_t led_strip;
static bool led_on;

void LED_Init(void)
{
    if (led_strip != NULL) {
        return;
    }
    const led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO_PIN,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    LED_Set(1);
}

void LED_Set(int on)
{
    ESP_ERROR_CHECK(led_strip != NULL ? ESP_OK : ESP_ERR_INVALID_STATE);
    if (on) {
        ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, LED_RED, LED_GREEN, LED_BLUE));
        ESP_ERROR_CHECK(led_strip_refresh(led_strip));
    } else {
        ESP_ERROR_CHECK(led_strip_clear(led_strip));
    }
    led_on = !!on;
}

void LED_Toggle(void)
{
    LED_Set(!led_on);
}
