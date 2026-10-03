#include "buttons.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tca9555_driver.h"
#include "app_config.h"
#include "recorder.h"
#include "setup_portal.h"
#include "status_led.h"
#include "uplink.h"

#define POLL_MS 50
#define KEY1 IO_EXPANDER_PIN_NUM_9
#define KEY2 IO_EXPANDER_PIN_NUM_10
#define KEY3 IO_EXPANDER_PIN_NUM_11

static bool down(uint32_t pin) { return !Read_EXIO(pin); }

static void buttons_task(void *arg)
{
    int held1 = 0, held2 = 0, combo = 0;
    bool combo_fired = false;
    while (1) {
        bool k1 = down(KEY1), k2 = down(KEY2), k3 = down(KEY3);

        if (k1 && k3) {                      // posts a request; the portal task does the work
            if (++combo * POLL_MS >= SETUP_HOLD_S * 1000 && !combo_fired) {
                combo_fired = true;
                setup_portal_start();
            }
            held1 = 0;                       // a combo is never also a short press
        } else {
            combo = 0;
            if (!k1 && !k3) combo_fired = false;
            if (k1) held1++;
            else if (held1 && held1 * POLL_MS < 1500 && !combo_fired) {
                recorder_toggle_pause();
                held1 = 0;
            } else held1 = 0;
        }

        if (k2) held2++;
        else if (held2) {
            if (held2 * POLL_MS < 1500) status_led_show_status(uplink_healthy());
            held2 = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void buttons_start(void)
{
    xTaskCreate(buttons_task, "buttons", 3072, NULL, 3, NULL);
}
