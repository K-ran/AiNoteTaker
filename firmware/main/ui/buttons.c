#include "buttons.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tca9555_driver.h"
#include "app_config.h"
#include "recorder.h"
#include "setup_portal.h"
#include "status_led.h"
#include "uplink.h"
#include "button_logic.h"

#define POLL_MS 50
#define KEY1 IO_EXPANDER_PIN_NUM_9
#define KEY2 IO_EXPANDER_PIN_NUM_10
#define KEY3 IO_EXPANDER_PIN_NUM_11

static bool down(uint32_t pin) { return !Read_EXIO(pin); }

static void buttons_task(void *arg)
{
    button_state_t st = {0};
    while (1) {
        int act = button_step(&st, down(KEY1), down(KEY2), down(KEY3), POLL_MS, SETUP_HOLD_S * 1000);
        if (act & BTN_OPEN_SETUP) setup_portal_start();      // posts a request; the portal task does the work
        if (act & BTN_TOGGLE_PAUSE) recorder_toggle_pause();
        if (act & BTN_SHOW_STATUS) status_led_show_status(uplink_healthy());
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void buttons_start(void)
{
    xTaskCreate(buttons_task, "buttons", 3072, NULL, 3, NULL);
}
