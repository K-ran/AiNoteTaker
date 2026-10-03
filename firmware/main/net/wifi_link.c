#include "wifi_link.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

#define BIT_GOT_IP  BIT0
#define BIT_FAIL    BIT1
#define MAX_RETRIES 3

static const char *TAG = "wifi";
static EventGroupHandle_t s_ev;
static SemaphoreHandle_t s_session;    // held for a whole station session
static SemaphoreHandle_t s_lock;       // short: protects mode transitions
static bool s_started, s_ap_on, s_sta_on;
static volatile bool s_sta_wanted;
static volatile int s_retries;
static volatile uint8_t s_last_reason;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        s_last_reason = d->reason;
        xEventGroupClearBits(s_ev, BIT_GOT_IP);
        if (s_sta_wanted && ++s_retries <= MAX_RETRIES) esp_wifi_connect();
        else xEventGroupSetBits(s_ev, BIT_FAIL);
        ESP_LOGW(TAG, "disconnected, reason %d", d->reason);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    }
}

void wifi_link_init(void)
{
    s_ev = xEventGroupCreate();
    s_session = xSemaphoreCreateMutex();
    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));   // credentials live in our NVS namespace
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
}

// Radio mode follows what is in use: AP+STA, AP, STA, or fully off.
static void apply_mode(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_ap_on && !s_sta_on) {
        if (s_started) { esp_wifi_stop(); s_started = false; }
    } else {
        esp_wifi_set_mode(s_ap_on ? WIFI_MODE_APSTA : WIFI_MODE_STA);  // APSTA keeps scanning possible
        if (!s_started) { esp_wifi_start(); s_started = true; }
    }
    xSemaphoreGive(s_lock);
}

bool wifi_link_session_begin(int timeout_ms)
{
    if (xSemaphoreTake(s_session, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    s_sta_on = true;
    apply_mode();
    return true;
}

void wifi_link_session_end(void)
{
    s_sta_wanted = false;
    esp_wifi_disconnect();
    xEventGroupClearBits(s_ev, BIT_GOT_IP);
    s_sta_on = false;
    apply_mode();
    xSemaphoreGive(s_session);
}

bool wifi_link_connect(const char *ssid, const char *pass, int timeout_ms)
{
    if (!ssid || !ssid[0]) return false;
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    s_sta_wanted = false;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_FAIL);
    s_retries = 0;
    s_sta_wanted = true;
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    bool ok = bits & BIT_GOT_IP;
    if (!ok) {
        ESP_LOGW(TAG, "connect to '%s' failed (reason %d)", ssid, s_last_reason);
        s_sta_wanted = false;
        esp_wifi_disconnect();
    }
    return ok;
}

bool wifi_link_connected(void) { return xEventGroupGetBits(s_ev) & BIT_GOT_IP; }

int wifi_link_rssi(void)
{
    wifi_ap_record_t ap;
    return wifi_link_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

uint8_t wifi_link_last_reason(void) { return s_last_reason; }

int wifi_link_scan(wifi_ap_record_t *out, int max)
{
    uint16_t n = max;
    if (esp_wifi_scan_start(NULL, true) != ESP_OK || esp_wifi_scan_get_ap_records(&n, out) != ESP_OK) n = 0;
    return n;
}

esp_err_t wifi_link_ap_start(const char *ssid, const char *pass)
{
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, pass, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    s_ap_on = true;
    apply_mode();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    ESP_LOGI(TAG, "hotspot '%s' %s", ssid, err == ESP_OK ? "up" : esp_err_to_name(err));
    return err;
}

void wifi_link_ap_stop(void)
{
    s_ap_on = false;
    apply_mode();
}
