#include "status_led.h"
#include <math.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "app_config.h"

#define FRAME_MS 50
#define DIM      24          // calm on the shop floor; also saves battery
#define PROBLEM_MASK ((1u << LED_PROBLEM_SD) | (1u << LED_PROBLEM_ENCODER) | (1u << LED_PROBLEM_UPLOAD))

static led_strip_handle_t s_strip;
static atomic_uint s_flags;
static atomic_llong s_show_until_us;
static atomic_bool s_show_healthy;

static void fill(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < LED_COUNT; i++) led_strip_set_pixel(s_strip, i, r, g, b);
    led_strip_refresh(s_strip);
}

// true during the first `on_ms` of every `period_ms`
static bool blink(int64_t ms, int period_ms, int on_ms) { return ms % period_ms < on_ms; }

static void led_task(void *arg)
{
    while (1) {
        int64_t now = esp_timer_get_time(), ms = now / 1000;
        unsigned f = atomic_load(&s_flags);
        if (now < atomic_load(&s_show_until_us)) {
            atomic_load(&s_show_healthy) ? fill(0, 60, 0) : fill(60, 0, 0);
        } else if (f & (1u << LED_SETUP)) {
            float k = 0.5f - 0.5f * cosf(ms * 2 * (float)M_PI / 3000.0f);   // slow blue pulse
            fill(0, 0, (uint8_t)(8 + 50 * k));
        } else if (f & PROBLEM_MASK) {
            blink(ms, 5000, 150) ? fill(60, 0, 0) : fill(0, 0, 0);       // red blink every 5 s
        } else if (f & (1u << LED_PAUSED)) {
            fill(DIM, DIM / 2, 0);                                       // steady amber
        } else {
            blink(ms, 5000, 60) ? fill(DIM, DIM, DIM) : fill(0, 0, 0);   // recording: dim white blink
        }
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
}

void status_led_start(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,   // as in the vendor driver
    };
    led_strip_rmt_config_t rmt = {.resolution_hz = 10 * 1000 * 1000};
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt, &s_strip));
    xTaskCreate(led_task, "led", 3072, NULL, 2, NULL);
}

void status_led_set(led_flag_t flag, bool on)
{
    if (on) atomic_fetch_or(&s_flags, 1u << flag);
    else atomic_fetch_and(&s_flags, ~(1u << flag));
}

bool status_led_any_problem(void) { return atomic_load(&s_flags) & PROBLEM_MASK; }

void status_led_show_status(bool healthy)
{
    atomic_store(&s_show_healthy, healthy);
    atomic_store(&s_show_until_us, esp_timer_get_time() + 3000000);
}
