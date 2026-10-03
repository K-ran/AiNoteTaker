// AI Note Taker, P0 firmware. Boot order matters: config -> board -> storage ->
// recording first (it never depends on the network) -> network and UI.
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "bsp_board.h"
#include "tca9555_driver.h"
#include "app_config.h"
#include "device_config.h"
#include "timekeep.h"
#include "event_log.h"
#include "chunk_store.h"
#include "capture.h"
#include "recorder.h"
#include "wifi_link.h"
#include "uplink.h"
#include "status_led.h"
#include "buttons.h"
#include "battery.h"
#include "console.h"
#include "setup_portal.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#define PA_PIN IO_EXPANDER_PIN_NUM_8   // speaker amp enable; speaker is removed, keep it off

static const char *TAG = "main";

// cJSON allocations (sidecars, GraphQL, status) go to PSRAM: keeps internal RAM for Wi-Fi/TLS/DMA.
static void *json_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }

void app_main(void)
{
    cJSON_Hooks hooks = {.malloc_fn = json_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
    event_log_init();
    ESP_ERROR_CHECK(config_init());
    esp_board_init(SAMPLE_RATE_HZ, 2, 16);     // vendor BSP: always returns OK
    tca9555_driver_init();
    Set_EXIO(PA_PIN, 0);
    status_led_start();
    battery_init();
    timekeep_init(esp_ret_i2c_handle());

    bool sd_ok = chunk_store_init();
    event_log("boot", "fw=%s reset=%d sd=%d", esp_app_get_description()->version, esp_reset_reason(), sd_ok);
    if (!sd_ok) {
        event_log("sd_fault", "no card: not recording");
        status_led_set(LED_PROBLEM_SD, true);
    }

    capture_start();
    recorder_start();
    wifi_link_init();
    setup_portal_init();
    uplink_start();
    buttons_start();
    console_start();
    ESP_LOGI(TAG, "%s ready", config_device_id());
}
