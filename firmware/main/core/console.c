#include "console.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "device_config.h"
#include "chunk_store.h"
#include "recorder.h"
#include "capture.h"
#include "uplink.h"
#include "setup_portal.h"
#include "timekeep.h"
#include "app_config.h"

#define LINE_MAX_LEN 1800                       // a JWT plus the command

static void cmd_status(void)
{
    uint32_t total, free_mb;
    chunk_store_space(&total, &free_mb);
    char t[24];
    timekeep_iso(t, sizeof(t));
    printf("device %s fw %s boot %lu\n", config_device_id(), esp_app_get_description()->version,
           (unsigned long)config_boot_id());
    printf("time %s (%s)  rec %s  last seq %lu  level %.0f dBFS  overruns %lu\n", t[0] ? t : "-",
           timekeep_source_name(), recorder_state_name(), (unsigned long)recorder_last_seq(),
           capture_level_dbfs(), (unsigned long)capture_overruns());
    printf("sd %s %lu/%lu MB free  backlog %d  uploads %s  setup %s\n", chunk_store_ok() ? "ok" : "MISSING",
           (unsigned long)free_mb, (unsigned long)total, chunk_count_ready(),
           uplink_healthy() ? "healthy" : "STALE", setup_portal_active() ? "open" : "closed");
    printf("heap internal %u free (min %u), psram %u free\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void handle(char *line)
{
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = 0;
    if (!strcmp(line, "status")) cmd_status();
    else if (!strcmp(line, "upload")) { uplink_trigger(); printf("upload window triggered\n"); }
    else if (!strcmp(line, "rotate")) { recorder_rotate(); printf("closing current chunk\n"); }
    else if (!strcmp(line, "pause")) recorder_set_paused(true);
    else if (!strcmp(line, "resume")) recorder_set_paused(false);
    else if (!strcmp(line, "setup")) setup_portal_start();
    else if (!strcmp(line, "rec-clear") && arg && !strcmp(arg, "yes")) printf("removed %d files\n", chunk_clear_all());
    else if (!strcmp(line, "setup-off")) setup_portal_stop();
    else if (!strcmp(line, "reboot")) esp_restart();
    else if (!strcmp(line, "wifi") && arg) {             // wifi <ssid> <password>  (ssid without spaces)
        char *pass = strchr(arg, ' ');
        if (pass) *pass++ = 0;
        printf(config_set_wifi(arg, pass ? pass : "") == ESP_OK ? "wifi saved\n" : "wifi: too long\n");
    } else if (!strcmp(line, "names") && arg) {          // names <store> <salesperson>
        char *sales = strchr(arg, ' ');
        if (sales) *sales++ = 0;
        printf(config_set_names(arg, sales ? sales : "") == ESP_OK ? "names saved\n" : "names: too long\n");
    } else if (!strcmp(line, "token") && arg) {
        size_t n = strlen(arg);
        printf(config_set_token(arg) == ESP_OK ? "token saved (%u chars)\n" : "token rejected (length or characters)\n", (unsigned)n);
        explicit_bzero(arg, n);
    } else {
        printf("commands: status | upload | rotate | pause | resume | setup | setup-off | reboot |\n"
               "          wifi <ssid> <pass> | names <store> <salesperson> | token <jwt> | rec-clear yes\n");
    }
}

static void console_task(void *arg)
{
    char *line = malloc(LINE_MAX_LEN);
    size_t n = 0;
    while (line) {
        int c = fgetc(stdin);
        if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (c == '\r' || c == '\n') {
            line[n] = 0;
            if (n) handle(line);
            explicit_bzero(line, n);             // don't leave a pasted token in RAM
            n = 0;
        } else if (n < LINE_MAX_LEN - 1) {
            line[n++] = (char)c;
        }
    }
    vTaskDelete(NULL);
}

void console_start(void)
{
#if !DEV_CONSOLE_ENABLED
    return;                                      // production builds: no serial command surface
#endif
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 2048;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) return;
    usb_serial_jtag_vfs_use_driver();            // blocking stdin reads instead of polling
    setvbuf(stdin, NULL, _IONBF, 0);
    xTaskCreate(console_task, "console", 4096, NULL, 2, NULL);
}
