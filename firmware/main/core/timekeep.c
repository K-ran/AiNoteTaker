#include "timekeep.h"
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "rtc_pcf85063.h"

static const char *TAG = "time";
static volatile time_source_t s_source = TIME_NONE;
static bool s_sntp_started;

static void on_sync(struct timeval *tv)
{
    s_source = TIME_NTP;
    pcf85063_write(tv->tv_sec);
    ESP_LOGI(TAG, "NTP sync, RTC updated");
}

void timekeep_init(i2c_master_bus_handle_t bus)
{
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t t;
    if (pcf85063_init(bus) == ESP_OK && pcf85063_read(&t)) {
        struct timeval tv = {.tv_sec = t};
        settimeofday(&tv, NULL);
        s_source = TIME_RTC;
    }
    char iso[24];
    timekeep_iso(iso, sizeof(iso));
    ESP_LOGI(TAG, "source=%s now=%s", timekeep_source_name(), iso[0] ? iso : "(unknown)");
}

void timekeep_start_sntp(void)
{
    if (s_sntp_started) return;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
    cfg.sync_cb = on_sync;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) s_sntp_started = true;
}

bool timekeep_wait_sync(int timeout_ms)
{
    if (s_source == TIME_NTP) return true;
    bool ok = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) == ESP_OK;
    if (!ok) ESP_LOGW(TAG, "NTP not synced within %d ms", timeout_ms);
    return ok;
}

const char *timekeep_source_name(void)
{
    return s_source == TIME_NTP ? "ntp" : s_source == TIME_HTTP ? "http" : s_source == TIME_RTC ? "rtc" : "none";
}
bool timekeep_valid(void) { return s_source != TIME_NONE; }

void timekeep_from_http_date(const char *date)
{
    if (s_source == TIME_NTP || s_source == TIME_HTTP) return;   // NTP is more precise; set once per boot
    struct tm tm = {0};
    if (!date || !strptime(date, "%a, %d %b %Y %H:%M:%S GMT", &tm) || tm.tm_year < 125) return;
    time_t t = mktime(&tm);                  // TZ is UTC
    struct timeval tv = {.tv_sec = t};
    settimeofday(&tv, NULL);
    s_source = TIME_HTTP;
    pcf85063_write(t);
    ESP_LOGI(TAG, "time set from HTTPS Date header (NTP unavailable)");
}

static void fmt(char *buf, size_t len, const char *f)
{
    if (!timekeep_valid()) { buf[0] = 0; return; }
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(buf, len, f, &tm);
}

void timekeep_iso(char *buf, size_t len) { fmt(buf, len, "%Y-%m-%dT%H:%M:%SZ"); }
