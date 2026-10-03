#include "device_config.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "esp_log.h"

static const char *TAG = "config";

typedef struct {
    char device_id[16];        // nt-<12 hex of the base MAC>
    char wifi_ssid[33];
    char wifi_pass[65];
    char store[33];
    char salesperson[33];
    char api_token[1536];      // Smaarthi bearer token (P0: shared low-privilege user)
} device_config_t;

static device_config_t s_cfg;   // only reachable through the accessors below
static SemaphoreHandle_t s_lock;
static uint32_t s_boot_id;
static nvs_handle_t s_nvs;

static void load_str(const char *key, char *dst, size_t len)
{
    size_t n = len;
    if (nvs_get_str(s_nvs, key, dst, &n) != ESP_OK) dst[0] = 0;
}

static esp_err_t save_str(const char *key, const char *val)
{
    esp_err_t err = nvs_set_str(s_nvs, key, val);
    return err == ESP_OK ? nvs_commit(s_nvs) : err;
}

esp_err_t config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open("nt", NVS_READWRITE, &s_nvs));
    s_lock = xSemaphoreCreateMutex();

    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(s_cfg.device_id, sizeof(s_cfg.device_id), "nt-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    load_str("ssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid));
    load_str("pass", s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass));
    load_str("store", s_cfg.store, sizeof(s_cfg.store));
    load_str("sales", s_cfg.salesperson, sizeof(s_cfg.salesperson));
    load_str("token", s_cfg.api_token, sizeof(s_cfg.api_token));

    nvs_get_u32(s_nvs, "boot", &s_boot_id);
    s_boot_id++;
    nvs_set_u32(s_nvs, "boot", s_boot_id);
    nvs_commit(s_nvs);

    ESP_LOGI(TAG, "%s boot %lu wifi=%s store=%s sales=%s token=%s", s_cfg.device_id,
             (unsigned long)s_boot_id, s_cfg.wifi_ssid[0] ? s_cfg.wifi_ssid : "(none)",
             s_cfg.store, s_cfg.salesperson, s_cfg.api_token[0] ? "set" : "(none)");
    return ESP_OK;
}

void config_get_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(ssid, s_cfg.wifi_ssid, ssid_len);
    strlcpy(pass, s_cfg.wifi_pass, pass_len);
    xSemaphoreGive(s_lock);
}

bool config_has_token(void) { return s_cfg.api_token[0] != 0; }

void config_get_names(char *store, size_t store_len, char *sales, size_t sales_len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(store, s_cfg.store, store_len);
    strlcpy(sales, s_cfg.salesperson, sales_len);
    xSemaphoreGive(s_lock);
}

const char *config_device_id(void) { return s_cfg.device_id; }   // immutable after init
uint32_t config_boot_id(void) { return s_boot_id; }

static esp_err_t set_field(char *dst, size_t len, const char *key, const char *val)
{
    if (strlen(val) >= len) return ESP_ERR_INVALID_SIZE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strcpy(dst, val);
    esp_err_t err = save_str(key, val);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t config_set_wifi(const char *ssid, const char *pass)
{
    esp_err_t err = set_field(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), "ssid", ssid);
    return err == ESP_OK ? set_field(s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), "pass", pass) : err;
}

esp_err_t config_set_names(const char *store, const char *salesperson)
{
    esp_err_t err = set_field(s_cfg.store, sizeof(s_cfg.store), "store", store);
    return err == ESP_OK ? set_field(s_cfg.salesperson, sizeof(s_cfg.salesperson), "sales", salesperson) : err;
}

esp_err_t config_set_token(const char *token)
{
    while (isspace((unsigned char)*token)) token++;           // pasted tokens often carry newlines
    size_t n = strlen(token);
    while (n && isspace((unsigned char)token[n - 1])) n--;
    if (n == 0 || n >= sizeof(s_cfg.api_token)) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < n; i++)                            // JWT alphabet only: no header injection
        if (!isalnum((unsigned char)token[i]) && !strchr("._-", token[i])) return ESP_ERR_INVALID_ARG;
    char *tmp = calloc(1, n + 1);
    if (!tmp) return ESP_ERR_NO_MEM;
    memcpy(tmp, token, n);
    esp_err_t err = set_field(s_cfg.api_token, sizeof(s_cfg.api_token), "token", tmp);
    explicit_bzero(tmp, n);
    free(tmp);
    return err;
}

bool config_copy_token(char *buf, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_cfg.api_token[0] && strlcpy(buf, s_cfg.api_token, len) < len;
    xSemaphoreGive(s_lock);
    return ok;
}

static uint32_t s_seq;                  // last used sequence number

void config_seed_seq(uint32_t highest_on_card)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_get_u32(s_nvs, "seq", &s_seq);
    if (highest_on_card > s_seq) s_seq = highest_on_card;
    xSemaphoreGive(s_lock);
}

uint32_t config_peek_next_seq(void) { return s_seq + 1; }   // only the recorder task allocates

void config_commit_seq(uint32_t seq)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_seq = seq;
    nvs_set_u32(s_nvs, "seq", seq);
    nvs_commit(s_nvs);
    xSemaphoreGive(s_lock);
}

void config_slug(const char *in, char *out, size_t out_len)
{
    size_t n = 0, max = out_len - 1 < 24 ? out_len - 1 : 24;
    for (; *in && n < max; in++)
        out[n++] = (isalnum((unsigned char)*in) || *in == '-') ? *in : '-';
    out[n] = 0;
    if (!n) snprintf(out, out_len, "unset");
}
