#include "uplink.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "app_config.h"
#include "device_config.h"
#include "chunk_store.h"
#include "smaarthi_api.h"
#include "wifi_link.h"
#include "timekeep.h"
#include "event_log.h"
#include "status_led.h"
#include "recorder.h"
#include "capture.h"
#include "battery.h"

static const char *TAG = "uplink";
static TaskHandle_t s_task;
static atomic_llong s_last_ok_us;              // last window that emptied the queue
static atomic_bool s_auth_problem;
static char s_last_upload_iso[24];             // uplink task only

#define TOKEN_MAX 1536
#define BATCH     32                           // chunks per window; the rest wait for the next one

static const char *meta_str(cJSON *m, const char *k)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(m, k));
    return v ? v : "";
}

// The file name sent to Smaarthi carries the P0 metadata (the API has no metadata field yet).
static void build_file_name(cJSON *m, char *out, size_t len)
{
    char store[25], sales[25], when[24];
    config_slug(meta_str(m, "store"), store, sizeof(store));
    config_slug(meta_str(m, "salesperson"), sales, sizeof(sales));
    const char *iso = meta_str(m, "start_utc");
    if (iso[0]) {                               // 2026-10-03T10:15:00Z -> 20261003T101500Z
        int j = 0;
        for (const char *p = iso; *p && j < (int)sizeof(when) - 1; p++) if (*p != '-' && *p != ':') when[j++] = *p;
        when[j] = 0;
    } else {                                    // clock unknown: boot id + uptime, server reconciles
        snprintf(when, sizeof(when), "b%.0fu%.0f", cJSON_GetNumberValue(cJSON_GetObjectItem(m, "boot_id")),
                 cJSON_GetNumberValue(cJSON_GetObjectItem(m, "start_uptime_ms")) / 1000.0);
    }
    snprintf(out, len, "%s_%s_%s_%08.0f_%s.ogg", config_device_id(), store, sales,
             cJSON_GetNumberValue(cJSON_GetObjectItem(m, "seq")), when);
}

static void set_str(cJSON *m, const char *k, const char *v)
{
    cJSON_DeleteItemFromObject(m, k);
    cJSON_AddStringToObject(m, k, v);
}

static bool in_state(cJSON *m, const char *s) { return !strcmp(meta_str(m, "state"), s); }

// One chunk through the four API calls, resuming from the state in its sidecar.
static api_result_t upload_chunk(uint32_t seq, const char *token)
{
    cJSON *m = chunk_meta_load(seq);
    if (!m) return API_LOCAL;
    char *name = malloc(160), *key = malloc(512), path[48], asset[96], conv[96];
    if (!name || !key) { free(name); free(key); cJSON_Delete(m); return API_LOCAL; }
    build_file_name(m, name, 160);
    chunk_path(seq, "OGG", path, sizeof(path));
    api_result_t r = API_OK;

    // An .OGG whose sidecar still says OPEN finished without its final sidecar write.
    if (in_state(m, "OPEN")) set_str(m, "state", "READY");

    if (in_state(m, "READY")) {
        char *url = NULL;
        r = api_request_upload_url(token, name, AUDIO_MIME, &url, key, 512);
        if (r == API_OK) r = api_put_file(url, path, AUDIO_MIME);
        free(url);
        if (r == API_OK) { set_str(m, "key", key); set_str(m, "state", "PUT_OK"); chunk_meta_save(seq, m); }
    }
    if (r == API_OK && in_state(m, "PUT_OK")) {
        r = api_create_asset(token, meta_str(m, "key"), AUDIO_MIME, name, asset, sizeof(asset));
        if (r == API_OK) { set_str(m, "asset_id", asset); set_str(m, "state", "ASSET"); chunk_meta_save(seq, m); }
    }
    if (r == API_OK && in_state(m, "ASSET")) {
        char *url = malloc(256);
        if (!url) r = API_LOCAL;
        else r = api_create_conversation(token, meta_str(m, "asset_id"), conv, sizeof(conv), url, 256);
        if (r == API_OK) {
            set_str(m, "conversation_id", conv);
            set_str(m, "state", "DONE");
            chunk_meta_save(seq, m);
            // The recordings viewer (tools/recordings_viewer.py) is built from these lines.
            event_log("uploaded", "seq=%lu conv=%s url=%s", (unsigned long)seq, conv, url);
            event_log("uploaded_name", "seq=%lu name=%s", (unsigned long)seq, name);
        }
        free(url);
    }
    if (r == API_OK && in_state(m, "DONE")) {
        chunk_delete(seq);                      // the only path that deletes audio: after the conversation exists
    } else if (r == API_REJECT) {
        int attempts = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(m, "attempts")) + 1;
        cJSON_DeleteItemFromObject(m, "attempts");
        cJSON_AddNumberToObject(m, "attempts", attempts);
        chunk_meta_save(seq, m);
        if (attempts >= REJECT_LIMIT) chunk_quarantine(seq);
    }
    free(name);
    free(key);
    cJSON_Delete(m);
    return r;
}

static void write_status(int backlog)
{
    uint32_t total_mb, free_mb;
    chunk_store_space(&total_mb, &free_mb);
    char t[24];
    timekeep_iso(t, sizeof(t));
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "t", t);
    cJSON_AddStringToObject(s, "device_id", config_device_id());
    cJSON_AddStringToObject(s, "fw_version", esp_app_get_description()->version);
    cJSON_AddNumberToObject(s, "boot_id", config_boot_id());
    cJSON_AddNumberToObject(s, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(s, "reset_reason", esp_reset_reason());
    cJSON_AddNumberToObject(s, "battery_mv", battery_mv());
    cJSON_AddBoolToObject(s, "sd_present", chunk_store_ok());
    cJSON_AddNumberToObject(s, "sd_total_mb", total_mb);
    cJSON_AddNumberToObject(s, "sd_free_mb", free_mb);
    cJSON_AddNumberToObject(s, "backlog_files", backlog);
    cJSON_AddStringToObject(s, "rec_state", recorder_state_name());
    cJSON_AddNumberToObject(s, "audio_level_dbfs", (int)capture_level_dbfs());
    cJSON_AddNumberToObject(s, "buffer_overruns", capture_overruns());
    cJSON_AddNumberToObject(s, "last_seq", recorder_last_seq());
    cJSON_AddNumberToObject(s, "wifi_rssi_dbm", wifi_link_rssi());
    cJSON_AddStringToObject(s, "last_upload_at", s_last_upload_iso);
    cJSON_AddStringToObject(s, "time_source", timekeep_source_name());
    cJSON_AddNumberToObject(s, "heap_internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(s, "heap_internal_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(s, "heap_internal_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(s, "heap_psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(s, "stack_free_recorder", uxTaskGetStackHighWaterMark(recorder_task_handle()));
    cJSON_AddNumberToObject(s, "stack_free_uplink", uxTaskGetStackHighWaterMark(NULL));
    char *line = cJSON_PrintUnformatted(s);
    if (line) { event_log_status(line); ESP_LOGI(TAG, "status %s", line); free(line); }
    cJSON_Delete(s);
}

static void run_window(void)
{
    int backlog = chunk_count_ready();
    char ssid[33], pass[65];
    config_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));
    if (!ssid[0] || !config_has_token()) {
        ESP_LOGW(TAG, "not configured (wifi=%d token=%d); %d chunks waiting", !!ssid[0], config_has_token(), backlog);
        explicit_bzero(pass, sizeof(pass));
        write_status(backlog);
        return;
    }
    if (!wifi_link_session_begin(5000)) {         // setup portal is testing: try again next window
        explicit_bzero(pass, sizeof(pass));
        return;
    }
    bool online = wifi_link_connect(ssid, pass, WIFI_CONNECT_TIMEOUT_S * 1000);
    explicit_bzero(pass, sizeof(pass));
    if (!online) {
        event_log("wifi_fail", "reason=%d backlog=%d", wifi_link_last_reason(), backlog);
        write_status(backlog);
        wifi_link_session_end();
        return;
    }
    timekeep_start_sntp();
    timekeep_wait_sync(5000);

    char *token = heap_caps_malloc(TOKEN_MAX, MALLOC_CAP_INTERNAL);
    uint32_t seqs[BATCH];
    int n = chunk_list_ready(seqs, BATCH), done = 0;
    api_result_t r = API_OK;
    if (token && config_copy_token(token, TOKEN_MAX)) {
        for (int i = 0; i < n; i++) {
            r = upload_chunk(seqs[i], token);
            if (r == API_OK) done++;
            else if (r == API_NET || r == API_SERVER || r == API_AUTH) break;   // retry next window
        }
    }
    if (token) { explicit_bzero(token, TOKEN_MAX); free(token); }

    bool auth = (r == API_AUTH);
    if (auth && !atomic_load(&s_auth_problem)) event_log("auth_failed", "%s", "uploads paused until the token is fixed");
    atomic_store(&s_auth_problem, auth);
    if (done) timekeep_iso(s_last_upload_iso, sizeof(s_last_upload_iso));
    int left = chunk_count_ready();
    if (left == 0) atomic_store(&s_last_ok_us, esp_timer_get_time());
    ESP_LOGI(TAG, "window: uploaded %d, %d left (%s)", done, left, api_result_name(r));
    write_status(left);
    wifi_link_session_end();
    if (left && done == n && n == BATCH) uplink_trigger();      // more than one batch waiting: keep going
}

static void uplink_task(void *arg)
{
    atomic_store(&s_last_ok_us, esp_timer_get_time());
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FIRST_UPLOAD_DELAY_S * 1000));
    while (1) {
        run_window();
        status_led_set(LED_PROBLEM_UPLOAD, !uplink_healthy());
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UPLOAD_INTERVAL_S * 1000));
    }
}

void uplink_start(void)
{
    xTaskCreate(uplink_task, "uplink", 10 * 1024, NULL, 4, &s_task);
}

void uplink_trigger(void) { if (s_task) xTaskNotifyGive(s_task); }

bool uplink_healthy(void)
{
    return !atomic_load(&s_auth_problem) &&
           esp_timer_get_time() - atomic_load(&s_last_ok_us) < STALE_UPLOAD_ALERT_S * 1000000LL;
}
