#include "telemetry.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "driver/temperature_sensor.h"
#include "app_config.h"
#include "device_config.h"
#include "timekeep.h"
#include "chunk_store.h"
#include "recorder.h"
#include "capture.h"
#include "battery.h"
#include "uplink.h"

static const char *TAG = "telemetry";

#define QUEUE_PATH LOG_DIR "/TQ.LOG"   // one JSON record per line, oldest first
#define TMP_PATH   LOG_DIR "/TQ.TMP"

static SemaphoreHandle_t s_lock;       // guards the queue file and the RAM ring
static bool s_sd_ok;
static char s_boot_id[40];
static temperature_sensor_handle_t s_temp;

// RAM queue, used while there is no SD card. Entries carry a running number so a
// flush can drop exactly what it sent even if new records arrived meanwhile.
static char *s_ram[TELEMETRY_RAM_RECORDS];
static uint32_t s_ram_head, s_ram_tail;          // [head, tail) are queued

// Counters reported by other modules, reset by each heartbeat.
static atomic_uint s_chunks_recorded, s_uploads, s_upload_bytes, s_dropped;
static atomic_int s_rssi, s_connect_ms, s_wifi_fail;
static char s_last_error[48];
static char s_last_sync_iso[24];

static const char *SEVERITY[] = {"Trace", "Debug", "Info", "Warn", "Error", "Fatal"};

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "crash";
        case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_USB: return "usb";
        case ESP_RST_EXT: return "external";
        default: return "other";
    }
}

static void uuid4(char out[37])
{
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

// ---------------------------------------------------------------- queue storage

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

// Stores one record. Called with s_lock held.
static void enqueue_locked(const char *line, bool is_event)
{
    if (s_sd_ok) {
        long size = file_size(QUEUE_PATH);
        if (size > TELEMETRY_HARD_CAP || (size > TELEMETRY_SOFT_CAP && !is_event)) {
            atomic_fetch_add(&s_dropped, 1);     // long offline period: keep events, shed heartbeats
            return;
        }
        FILE *f = fopen(QUEUE_PATH, "a");
        if (f) {
            fputs(line, f);
            fputc('\n', f);
            fclose(f);
            return;
        }
    }
    if (s_ram_tail - s_ram_head == TELEMETRY_RAM_RECORDS) {   // full: drop the oldest
        free(s_ram[s_ram_head % TELEMETRY_RAM_RECORDS]);
        s_ram_head++;
        atomic_fetch_add(&s_dropped, 1);
    }
    s_ram[s_ram_tail % TELEMETRY_RAM_RECORDS] = strdup(line);
    s_ram_tail++;
}

// Builds the stored form of a record and queues it. Consumes `rec`.
static void store(cJSON *rec, bool is_event)
{
    char id[37], at[24];
    uuid4(id);
    timekeep_iso(at, sizeof(at));               // "" until the clock is set: fixed at send time
    cJSON_AddStringToObject(rec, "eventId", id);
    cJSON_AddStringToObject(rec, "bootId", s_boot_id);
    cJSON_AddStringToObject(rec, "recordedAt", at);
    cJSON_AddNumberToObject(rec, "_up", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(rec, "_boot", config_boot_id());
    cJSON_AddBoolToObject(rec, "_ev", is_event);
    char *line = cJSON_PrintUnformatted(rec);
    cJSON_Delete(rec);
    if (!line) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    enqueue_locked(line, is_event);
    xSemaphoreGive(s_lock);
    free(line);
}

void telemetry_event(const char *name, tel_severity_t sev, const char *message, cJSON *attrs)
{
    if (!s_lock) { cJSON_Delete(attrs); return; }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "name", name);
    cJSON_AddStringToObject(r, "severity", SEVERITY[sev]);
    if (message && message[0]) cJSON_AddStringToObject(r, "message", message);
    cJSON_AddItemToObject(r, "attributes", attrs ? attrs : cJSON_CreateObject());
    store(r, true);
}

// ---------------------------------------------------------------- heartbeat

void telemetry_note_chunk_recorded(void) { atomic_fetch_add(&s_chunks_recorded, 1); }

void telemetry_note_upload(uint32_t bytes)
{
    atomic_fetch_add(&s_uploads, 1);
    atomic_fetch_add(&s_upload_bytes, bytes);
}

void telemetry_note_window(int rssi_dbm, int connect_ms, int wifi_fail_reason, const char *last_error)
{
    atomic_store(&s_rssi, rssi_dbm);
    atomic_store(&s_connect_ms, connect_ms);
    atomic_store(&s_wifi_fail, wifi_fail_reason);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_last_error, last_error ? last_error : "", sizeof(s_last_error));
    if (!strcmp(timekeep_source_name(), "ntp") && !s_last_sync_iso[0]) timekeep_iso(s_last_sync_iso, sizeof(s_last_sync_iso));
    xSemaphoreGive(s_lock);
}

static void capture_heartbeat(void)
{
    uint32_t total_mb, free_mb, files = 0;
    uint64_t backlog_bytes = 0;
    int64_t oldest_mtime = 0;
    chunk_store_space(&total_mb, &free_mb);
    chunk_store_backlog(&files, &backlog_bytes, &oldest_mtime);
    float temp = 0;
    bool have_temp = s_temp && temperature_sensor_get_celsius(s_temp, &temp) == ESP_OK;

    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "firmwareVersion", esp_app_get_description()->version);
    cJSON_AddStringToObject(a, "boardId", config_device_id());
    cJSON_AddNumberToObject(a, "uptimeSec", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(a, "bootCount", config_boot_id());
    cJSON_AddStringToObject(a, "resetReason", reset_reason_name(esp_reset_reason()));

    cJSON *rec = cJSON_AddObjectToObject(a, "recording");
    cJSON_AddStringToObject(rec, "state", recorder_state_name());
    cJSON_AddNumberToObject(rec, "audioLevelDbfs", (int)capture_level_dbfs());
    cJSON_AddNumberToObject(rec, "chunksRecorded", atomic_exchange(&s_chunks_recorded, 0));
    cJSON_AddNumberToObject(rec, "bufferOverruns", capture_overruns());
    cJSON_AddNumberToObject(rec, "lastSeq", recorder_last_seq());

    cJSON *st = cJSON_AddObjectToObject(a, "storage");
    cJSON_AddBoolToObject(st, "sdPresent", chunk_store_ok());
    cJSON_AddNumberToObject(st, "sdTotalMb", total_mb);
    cJSON_AddNumberToObject(st, "sdFreeMb", free_mb);
    cJSON_AddNumberToObject(st, "backlogFiles", files);
    cJSON_AddNumberToObject(st, "backlogMb", (double)(backlog_bytes / 10485) / 100.0);
    if (oldest_mtime && timekeep_valid())
        cJSON_AddNumberToObject(st, "oldestBacklogAgeMin", (double)((time(NULL) - oldest_mtime) / 60));
    cJSON_AddNumberToObject(st, "telemetryPending", telemetry_pending());
    cJSON_AddNumberToObject(st, "telemetryDropped", atomic_exchange(&s_dropped, 0));

    cJSON *up = cJSON_AddObjectToObject(a, "uploads");
    cJSON_AddNumberToObject(up, "chunksUploaded", atomic_exchange(&s_uploads, 0));
    cJSON_AddNumberToObject(up, "bytesUploaded", atomic_exchange(&s_upload_bytes, 0));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddStringToObject(up, "lastError", s_last_error);
    cJSON *wifi = cJSON_AddObjectToObject(a, "wifi");
    cJSON_AddNumberToObject(wifi, "rssiDbm", atomic_load(&s_rssi));
    cJSON_AddNumberToObject(wifi, "connectMs", atomic_load(&s_connect_ms));
    cJSON_AddNumberToObject(wifi, "lastFailReason", atomic_load(&s_wifi_fail));
    cJSON *tm = cJSON_AddObjectToObject(a, "time");
    cJSON_AddStringToObject(tm, "source", timekeep_source_name());
    cJSON_AddStringToObject(tm, "lastNtpSyncAt", s_last_sync_iso);
    xSemaphoreGive(s_lock);

    cJSON *h = cJSON_AddObjectToObject(a, "health");
    cJSON_AddNumberToObject(h, "heapInternalFree", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(h, "heapInternalMin", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(h, "heapInternalLargest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(h, "psramFree", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(h, "stackFreeRecorder", uxTaskGetStackHighWaterMark(recorder_task_handle()));
    if (uplink_task_handle()) cJSON_AddNumberToObject(h, "stackFreeUplink", uxTaskGetStackHighWaterMark(uplink_task_handle()));
    if (have_temp) cJSON_AddNumberToObject(h, "chipTempC", (int)(temp * 10) / 10.0);

    cJSON *pw = cJSON_AddObjectToObject(a, "power");
    int mv = battery_mv();
    cJSON_AddBoolToObject(pw, "batteryMonitoring", mv >= 0);
    if (mv >= 0) cJSON_AddNumberToObject(pw, "batteryMv", mv);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "attributes", a);
    store(r, false);
}

static void heartbeat_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(30 * 1000));          // first one after boot settles, before the first window
    while (1) {
        capture_heartbeat();
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_S * 1000));
    }
}

// ---------------------------------------------------------------- delivery

// Converts a stored line into an API record; fills recordedAt when the clock
// was unknown at capture time.
static cJSON *to_api(const char *line, bool *is_event)
{
    cJSON *r = cJSON_Parse(line);
    if (!r) return NULL;
    *is_event = cJSON_IsTrue(cJSON_GetObjectItem(r, "_ev"));
    const char *at = cJSON_GetStringValue(cJSON_GetObjectItem(r, "recordedAt"));
    if (!at || !at[0]) {
        char iso[24];
        time_t when = time(NULL);
        bool same_boot = cJSON_GetNumberValue(cJSON_GetObjectItem(r, "_boot")) == config_boot_id();
        if (same_boot) when -= (time_t)((esp_timer_get_time() / 1000 - cJSON_GetNumberValue(cJSON_GetObjectItem(r, "_up"))) / 1000);
        struct tm tm;
        gmtime_r(&when, &tm);
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);
        cJSON_DeleteItemFromObject(r, "recordedAt");
        cJSON_AddStringToObject(r, "recordedAt", iso);
        if (!same_boot) {                          // a boot that never learned the time: estimate
            cJSON *attrs = cJSON_GetObjectItem(r, "attributes");
            if (cJSON_IsObject(attrs)) cJSON_AddBoolToObject(attrs, "timeEstimated", true);
        }
    }
    cJSON_DeleteItemFromObject(r, "_ev");
    cJSON_DeleteItemFromObject(r, "_up");
    cJSON_DeleteItemFromObject(r, "_boot");
    return r;
}

// Drops the first `bytes` of the queue file (records the backend has acknowledged).
static void trim_file_locked(long bytes)
{
    if (bytes <= 0) return;
    FILE *in = fopen(QUEUE_PATH, "r");
    if (!in) return;
    FILE *out = fopen(TMP_PATH, "w");
    if (!out) { fclose(in); return; }
    fseek(in, bytes, SEEK_SET);
    char *buf = malloc(4096);
    size_t n;
    while (buf && (n = fread(buf, 1, 4096, in)) > 0) fwrite(buf, 1, n, out);
    free(buf);
    fclose(in);
    fflush(out);
    fsync(fileno(out));
    fclose(out);
    unlink(QUEUE_PATH);                            // FAT can't rename over a file; init() repairs a crash here
    rename(TMP_PATH, QUEUE_PATH);
}

api_result_t telemetry_flush(const char *token, const char *device_id)
{
    if (!device_id || !device_id[0]) return API_LOCAL;     // deviceId is required by the backend
    if (!timekeep_valid()) return API_LOCAL;               // recordedAt can't be filled in yet: wait
    char *line = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!line) return API_LOCAL;
    api_result_t r = API_OK;

    for (int batch = 0; batch < TELEMETRY_BATCHES_MAX; batch++) {
        cJSON *input = cJSON_CreateObject();
        cJSON_AddStringToObject(input, "deviceId", device_id);
        cJSON *hbs = cJSON_AddArrayToObject(input, "heartbeats");
        cJSON *evs = cJSON_AddArrayToObject(input, "events");
        int count = 0;
        size_t bytes = 0;
        long file_pos = 0;
        uint32_t ram_upto;

        xSemaphoreTake(s_lock, portMAX_DELAY);       // collect, then release the lock for the network call
        ram_upto = s_ram_head;
        while (ram_upto != s_ram_tail && count < TELEMETRY_BATCH_RECORDS && bytes < TELEMETRY_BATCH_BYTES) {
            const char *l = s_ram[ram_upto % TELEMETRY_RAM_RECORDS];
            bool ev;
            cJSON *rec = to_api(l, &ev);
            if (rec) cJSON_AddItemToArray(ev ? evs : hbs, rec);
            bytes += strlen(l);
            count++;
            ram_upto++;
        }
        FILE *f = s_sd_ok ? fopen(QUEUE_PATH, "r") : NULL;
        while (f && count < TELEMETRY_BATCH_RECORDS && bytes < TELEMETRY_BATCH_BYTES && fgets(line, 4096, f)) {
            size_t len = strlen(line);
            if (len && line[len - 1] != '\n') {
                if (len < 4095) break;                // partial last line (being written): next time
                int c;                                 // over-long line: skip it so it can't block the queue
                while ((c = fgetc(f)) != EOF && c != '\n') len++;
                file_pos = ftell(f);
                bytes += len;
                count++;
                continue;
            }
            bool ev;
            cJSON *rec = to_api(line, &ev);           // an unparsable line is skipped (and trimmed)
            if (rec) cJSON_AddItemToArray(ev ? evs : hbs, rec);
            bytes += len;
            count++;
            file_pos = ftell(f);
        }
        if (f) fclose(f);
        xSemaphoreGive(s_lock);

        if (count == 0) { cJSON_Delete(input); break; }
        int hb_new = 0, ev_new = 0, hb_sent = cJSON_GetArraySize(hbs), ev_sent = cJSON_GetArraySize(evs);
        if (!hb_sent) cJSON_DeleteItemFromObject(input, "heartbeats");   // send only non-empty arrays
        if (!ev_sent) cJSON_DeleteItemFromObject(input, "events");
        r = api_report_telemetry(token, input, &hb_new, &ev_new);       // consumes input

        if (r == API_OK || r == API_REJECT) {        // acknowledged, or malformed: either way drop it
            xSemaphoreTake(s_lock, portMAX_DELAY);
            while (s_ram_head != ram_upto) { free(s_ram[s_ram_head % TELEMETRY_RAM_RECORDS]); s_ram_head++; }
            trim_file_locked(file_pos);
            xSemaphoreGive(s_lock);
            if (r == API_OK) ESP_LOGI(TAG, "sent %d heartbeats, %d events (%d/%d new)", hb_sent, ev_sent, hb_new, ev_new);
            else ESP_LOGW(TAG, "backend rejected a batch of %d records as invalid; dropped", count);
        } else {
            ESP_LOGW(TAG, "batch of %d kept for retry (%s)", count, api_result_name(r));
            break;                                   // auth/ownership, throttling, network: next window
        }
    }
    free(line);
    return r;
}

int telemetry_pending(void)
{
    int ram = (int)(s_ram_tail - s_ram_head);
    long size = s_sd_ok ? file_size(QUEUE_PATH) : 0;
    return ram + (size > 0 ? (int)(size / 700) : 0);   // ~700 bytes per stored record
}

void telemetry_dump(int max_records)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    for (uint32_t i = s_ram_head; i != s_ram_tail && n < max_records; i++, n++)
        printf("ram: %s\n", s_ram[i % TELEMETRY_RAM_RECORDS]);
    FILE *f = s_sd_ok ? fopen(QUEUE_PATH, "r") : NULL;
    char *line = f ? malloc(4096) : NULL;
    while (line && n < max_records && fgets(line, 4096, f)) { printf("sd:  %s", line); n++; }
    free(line);
    if (f) fclose(f);
    xSemaphoreGive(s_lock);
    printf("(%d shown, ~%d pending)\n", n, telemetry_pending());
}

// ---------------------------------------------------------------- lifecycle

void telemetry_set_sd(bool ok)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sd_ok = ok;
    if (ok && file_size(QUEUE_PATH) < 0 && file_size(TMP_PATH) >= 0) rename(TMP_PATH, QUEUE_PATH);  // crash mid-trim
    if (ok) unlink(LOG_DIR "/STATUS.LOG");           // replaced by heartbeats (pre-0.1.4 builds)
    xSemaphoreGive(s_lock);
}

void telemetry_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    snprintf(s_boot_id, sizeof(s_boot_id), "%s-%lu", config_device_id(), (unsigned long)config_boot_id());
    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    if (temperature_sensor_install(&tc, &s_temp) == ESP_OK) temperature_sensor_enable(s_temp);
    else s_temp = NULL;
}

void telemetry_start(void)
{
    xTaskCreate(heartbeat_task, "heartbeat", 6144, NULL, 2, NULL);
}
