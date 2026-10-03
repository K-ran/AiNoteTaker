#include "wifi_http.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "led_web.h"

// ponytail: hardcoded credentials, move to menuconfig/NVS before sharing the firmware
#define WIFI_SSID "<your-ssid>"
#define WIFI_PASS "<your-wifi-password>"   // scrubbed: never commit real credentials
// ponytail: Mac's LAN IP hardcoded; breaks if DHCP gives it a new one (use mDNS or a router reservation)
#define UPLOAD_URL "http://192.168.1.3:8000/upload/"
#define SD_MOUNT   "/sdcard"

static const char *TAG = "wifi_http";

#define GOT_IP_BIT BIT0
static EventGroupHandle_t s_wifi_events;

// Runs in the ESP-IDF event task, not ours: keep it short.
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "Disconnected (reason %d), retrying...", d->reason);
        xEventGroupClearBits(s_wifi_events, GOT_IP_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, GOT_IP_BIT);
    }
}

static void wifi_start(void)
{
    esp_err_t ret = nvs_flash_init();   // Wi-Fi stores calibration data in NVS
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// Blocking scan; logs every AP the radio can hear.
static void wifi_scan_print(void)
{
    ESP_ERROR_CHECK(esp_wifi_scan_start(NULL, true));
    uint16_t n = 20;
    static wifi_ap_record_t aps[20];
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&n, aps));
    ESP_LOGI(TAG, "Scan found %u APs (2.4 GHz only):", n);
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "  %-32s  ch %2d  rssi %4d dBm  auth %d",
                 (char *)aps[i].ssid, aps[i].primary, aps[i].rssi, aps[i].authmode);
    }
}

// PUT one file to UPLOAD_URL<name>, streamed from the SD card. Returns true on HTTP 200.
static bool upload_file(const char *path, const char *name)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char url[sizeof(UPLOAD_URL) + 256];
    snprintf(url, sizeof(url), UPLOAD_URL "%s", name);
    esp_http_client_config_t cfg = { .url = url, .method = HTTP_METHOD_PUT, .timeout_ms = 10000 };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "audio/wav");

    bool ok = false;
    esp_err_t err = esp_http_client_open(client, size);
    if (err == ESP_OK) {
        static char buf[4096];   // static: keeps it off the task stack
        size_t n;
        long sent = 0;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            if (esp_http_client_write(client, buf, n) != (int)n) break;
            sent += n;
        }
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        ok = sent == size && status == 200;
        ESP_LOGI(TAG, "PUT %s: %ld/%ld bytes -> %d", name, sent, size, status);
    } else {
        ESP_LOGE(TAG, "PUT %s failed: %s", name, esp_err_to_name(err));
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    fclose(f);
    return ok;
}

// Upload every REC*.WAV on the card; rename each to .SNT once the server has it.
static void upload_pending(void)
{
    DIR *dir = opendir(SD_MOUNT);
    if (!dir) return;   // no SD card
    struct dirent *e;
    int uploaded = 0;
    bool failed = false;
    while ((e = readdir(dir)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 4 || strcmp(e->d_name + len - 4, ".WAV") != 0) continue;

        char path[sizeof(SD_MOUNT) + sizeof(e->d_name)], sent[sizeof(path)];
        snprintf(path, sizeof(path), SD_MOUNT "/%s", e->d_name);
        led_set_mode(LED_UPLOADING);
        if (!upload_file(path, e->d_name)) {   // server down: retry next round
            failed = true;
            break;
        }
        snprintf(sent, sizeof(sent), "%.*s.SNT", (int)strlen(path) - 4, path);
        rename(path, sent);
        uploaded++;
    }
    closedir(dir);

    if (failed || uploaded) {
        led_set_mode(LED_IDLE);
        if (failed) led_flash(90, 40, 0);    // amber: will retry
        else        led_flash(0, 90, 20);    // green: all sent
    }
}

static void wifi_http_task(void *arg)
{
    wifi_start();
    wifi_scan_print();
    esp_wifi_connect();   // later reconnects happen in the DISCONNECTED handler

    xEventGroupWaitBits(s_wifi_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    led_web_start();      // server keeps running across Wi-Fi reconnects

    while (1) {
        // Blocks (no CPU used) until the event handler sets GOT_IP_BIT.
        xEventGroupWaitBits(s_wifi_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        upload_pending();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void wifi_http_start(void)
{
    s_wifi_events = xEventGroupCreate();
    xTaskCreate(wifi_http_task, "wifi_http", 6 * 1024, NULL, 4, NULL);
}
