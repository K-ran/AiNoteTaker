#include "event_log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "app_config.h"
#include "device_config.h"
#include "timekeep.h"

static const char *TAG = "event";
static bool s_sd_ok;
static SemaphoreHandle_t s_lock;

// Appends one line; rotates the file to .OLD past LOG_MAX_BYTES.
static void append(const char *path, const char *line)
{
    if (!s_sd_ok) return;
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > LOG_MAX_BYTES) {
        char old[48];
        snprintf(old, sizeof(old), "%.*s.OLD", (int)(strlen(path) - 4), path);
        unlink(old);
        rename(path, old);
    }
    FILE *f = fopen(path, "a");
    if (!f) return;
    fputs(line, f);
    fputc('\n', f);
    fclose(f);
}

void event_log_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}

void event_log_set_sd(bool sd_ok) { s_sd_ok = sd_ok; }

void event_log(const char *event, const char *fmt, ...)
{
    char detail[192], line[320], t[24];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    for (char *p = detail; *p; p++) {                                       // keep each line valid JSON
        if (*p == '"' || *p == '\\') *p = '\'';
        else if ((unsigned char)*p < 0x20) *p = ' ';
    }
    timekeep_iso(t, sizeof(t));
    snprintf(line, sizeof(line), "{\"t\":\"%s\",\"up\":%lld,\"boot\":%lu,\"ev\":\"%s\",\"detail\":\"%s\"}",
             t, esp_timer_get_time() / 1000, (unsigned long)config_boot_id(), event, detail);
    ESP_LOGI(TAG, "%s %s", event, detail);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    append(LOG_DIR "/EVENTS.LOG", line);
    xSemaphoreGive(s_lock);
}

void event_log_status(const char *json_line)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    append(LOG_DIR "/STATUS.LOG", json_line);
    xSemaphoreGive(s_lock);
}
