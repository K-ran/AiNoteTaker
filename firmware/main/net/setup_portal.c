#include "setup_portal.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "app_config.h"
#include "device_config.h"
#include "wifi_link.h"
#include "status_led.h"
#include "event_log.h"
#include "uplink.h"

static const char *TAG = "setup";

typedef enum { CMD_START, CMD_STOP, CMD_TEST } cmd_t;

typedef struct {                       // form values waiting for a successful Wi-Fi test
    char ssid[33], pass[65], store[33], sales[33], devid[40];
    char token[1536];
} pending_t;

static QueueHandle_t s_cmds;
static SemaphoreHandle_t s_lock;       // protects s_result and s_pending
static httpd_handle_t s_server;
static esp_timer_handle_t s_close_timer;
static atomic_bool s_active, s_testing;
static char s_result[112] = "idle";
static pending_t *s_pending;           // PSRAM
static char *s_networks_json;          // scan result cached at start (no scanning inside HTTP handlers)

#define MAX_BODY 4096                  // URL-encoded form: token can triple in size when encoded

static const char PAGE[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Note Taker setup</title>"
    "<style>body{font-family:sans-serif;max-width:420px;margin:1.5em auto;padding:0 1em;color:#16262b}"
    "label{display:block;margin-top:1em;font-weight:600}input,select,textarea{width:100%;padding:.6em;font-size:1em;box-sizing:border-box}"
    "button{margin-top:1.4em;width:100%;padding:.8em;font-size:1.1em;background:#1d6b63;color:#fff;border:0;border-radius:6px}"
    "#r{margin-top:1em;padding:.8em;border-radius:6px;background:#e6eeec}small{color:#5e686e}</style>"
    "<h2>Note Taker setup</h2><small id=d></small>"
    "<form id=f><label>Store Wi-Fi</label><select id=s></select>"
    "<input id=o placeholder='or type the network name' maxlength=32>"
    "<label>Wi-Fi password</label><input name=pass type=password maxlength=64>"
    "<label>Store name</label><input name=store maxlength=32 required>"
    "<label>Salesperson</label><input name=sales maxlength=32 required>"
    "<label>Smaarthi device ID</label><input name=devid maxlength=39 autocapitalize=off autocorrect=off spellcheck=false placeholder='from the project lead, starts with cm'>"
    "<label>API token <small>(leave empty to keep the current one)</small></label>"
    "<textarea name=token rows=3></textarea>"
    "<button>Save and connect</button></form><div id=r>Fill in the form and press Save.</div>"
    "<script>"
    "fetch('/info').then(r=>r.json()).then(i=>{d.textContent=i.device_id+' \\u00b7 firmware '+i.fw+(i.token?' \\u00b7 token set':' \\u00b7 no token');"
    "f.store.value=i.store;f.sales.value=i.sales;f.devid.value=i.devid;"
    "i.networks.forEach(n=>{let e=document.createElement('option');e.value=e.textContent=n;s.appendChild(e)})});"
    "f.onsubmit=e=>{e.preventDefault();let p=new URLSearchParams(new FormData(f));p.set('ssid',o.value||s.value);"
    "r.textContent='Saving...';fetch('/save',{method:'POST',body:p}).then(x=>x.text()).then(t=>{r.textContent=t;poll()})};"
    "function poll(){fetch('/status').then(x=>x.text()).then(t=>{r.textContent=t;if(/Testing/.test(t))setTimeout(poll,1500)})}"
    "</script>";

static void set_result(const char *fmt, const char *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_result, sizeof(s_result), fmt, arg);
    xSemaphoreGive(s_lock);
}

// Only clients on the hotspot may use the page: httpd also listens on the store LAN
// interface while the station is connected, where the setup code gives no protection.
static bool from_hotspot(httpd_req_t *req)
{
    // httpd listens on a dual-stack IPv6 socket, so IPv4 clients show up as
    // IPv4-mapped addresses (::ffff:192.168.4.1). Handle both forms.
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    esp_netif_ip_info_t ap;
    esp_netif_t *apif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&ss, &len) != 0 || !apif ||
        esp_netif_get_ip_info(apif, &ap) != ESP_OK) return false;
    uint32_t local = 0;
    if (ss.ss_family == AF_INET) {
        local = ((struct sockaddr_in *)&ss)->sin_addr.s_addr;
    } else if (ss.ss_family == AF_INET6) {
        const uint8_t *a = ((struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr;
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(a, mapped, sizeof(mapped)) == 0) memcpy(&local, a + 12, 4);
    }
    if (local != ap.ip.addr) {
        ESP_LOGW(TAG, "refused request to %s (local " IPSTR ", family %d): not from the hotspot",
                 req->uri, IP2STR((esp_ip4_addr_t *)&local), ss.ss_family);
        return false;
    }
    return true;
}

#define REQUIRE_HOTSPOT(req) \
    do { if (!from_hotspot(req)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Use the setup hotspot"); } while (0)

static esp_err_t page_get(httpd_req_t *req)
{
    REQUIRE_HOTSPOT(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
}

static esp_err_t info_get(httpd_req_t *req)
{
    REQUIRE_HOTSPOT(req);
    char store[33], sales[33], devid[40];
    config_get_names(store, sizeof(store), sales, sizeof(sales));
    config_get_backend_id(devid, sizeof(devid));
    bool has_token = config_has_token();          // never echo the token itself
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "device_id", config_device_id());
    cJSON_AddStringToObject(j, "fw", esp_app_get_description()->version);
    cJSON_AddStringToObject(j, "store", store);
    cJSON_AddStringToObject(j, "sales", sales);
    cJSON_AddStringToObject(j, "devid", devid);
    cJSON_AddBoolToObject(j, "token", has_token);
    cJSON_AddItemToObject(j, "networks", cJSON_Parse(s_networks_json ? s_networks_json : "[]"));
    char *s = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    cJSON_Delete(j);
    return err;
}

// In-place application/x-www-form-urlencoded decode.
static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

// Reads one form field. Encoded values can be 3x longer than decoded ones, so decode
// into a scratch buffer and check the decoded length; truncation is an error, never
// a silently shortened (or emptied) value.
static bool field(const char *body, const char *key, char *out, size_t max, char *scratch, size_t scratch_len)
{
    esp_err_t err = httpd_query_key_value(body, key, scratch, scratch_len);
    if (err == ESP_ERR_NOT_FOUND) { out[0] = 0; return true; }
    if (err != ESP_OK) return false;
    url_decode(scratch);
    for (char *p = scratch; *p; p++) if ((unsigned char)*p < 0x20) return false;   // no control chars
    return strlcpy(out, scratch, max) < max;
}

static esp_err_t save_post(httpd_req_t *req)
{
    REQUIRE_HOTSPOT(req);
    if (req->content_len >= MAX_BODY) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too large");
    if (atomic_load(&s_testing)) return httpd_resp_sendstr(req, "Already testing, please wait.");
    char *body = heap_caps_calloc(1, MAX_BODY, MALLOC_CAP_SPIRAM);
    char *scratch = heap_caps_calloc(1, MAX_BODY, MALLOC_CAP_SPIRAM);
    if (!body || !scratch) { free(body); free(scratch); return httpd_resp_send_500(req); }
    int got = 0;
    while (got < (int)req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) { free(body); free(scratch); return ESP_FAIL; }
        got += n;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    pending_t *p = s_pending;
    bool ok = field(body, "ssid", p->ssid, sizeof(p->ssid), scratch, MAX_BODY) &&
              field(body, "pass", p->pass, sizeof(p->pass), scratch, MAX_BODY) &&
              field(body, "store", p->store, sizeof(p->store), scratch, MAX_BODY) &&
              field(body, "sales", p->sales, sizeof(p->sales), scratch, MAX_BODY) &&
              field(body, "devid", p->devid, sizeof(p->devid), scratch, MAX_BODY) &&
              field(body, "token", p->token, sizeof(p->token), scratch, MAX_BODY);
    bool complete = ok && p->ssid[0] && p->store[0] && p->sales[0];
    xSemaphoreGive(s_lock);
    explicit_bzero(body, MAX_BODY);
    explicit_bzero(scratch, MAX_BODY);
    free(body);
    free(scratch);

    if (!ok) return httpd_resp_sendstr(req, "A value is too long or contains invalid characters.");
    if (!complete) return httpd_resp_sendstr(req, "Wi-Fi, store and salesperson are required.");
    atomic_store(&s_testing, true);
    set_result("Testing connection to %s...", p->ssid);
    cmd_t c = CMD_TEST;
    xQueueSend(s_cmds, &c, 0);
    return httpd_resp_sendstr(req, "Testing connection...");
}

static esp_err_t status_get(httpd_req_t *req)
{
    REQUIRE_HOTSPOT(req);
    char copy[sizeof(s_result)];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strcpy(copy, s_result);
    xSemaphoreGive(s_lock);
    return httpd_resp_sendstr(req, copy);
}

static void close_cb(void *arg)        // esp_timer task: only hand off, never block here
{
    cmd_t c = CMD_STOP;
    xQueueSend(s_cmds, &c, 0);
}

static void do_start(void)
{
    if (atomic_load(&s_active)) return;
    atomic_store(&s_active, true);
    status_led_set(LED_SETUP, true);
    set_result("%s", "idle");

    char ssid[20];
    snprintf(ssid, sizeof(ssid), "NoteTaker-%s", config_device_id() + strlen(config_device_id()) - 4);
    wifi_link_ap_start(ssid, DEV_SETUP_CODE);

    // Scan once now (needs the station interface), so HTTP handlers never block on the radio.
    if (wifi_link_session_begin(30000)) {
        wifi_ap_record_t *aps = heap_caps_malloc(sizeof(wifi_ap_record_t) * 16, MALLOC_CAP_SPIRAM);
        int n = aps ? wifi_link_scan(aps, 16) : 0;
        wifi_link_session_end();
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < n; i++)
            if (aps[i].ssid[0]) cJSON_AddItemToArray(arr, cJSON_CreateString((char *)aps[i].ssid));
        free(s_networks_json);
        s_networks_json = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        free(aps);
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    if (httpd_start(&s_server, &cfg) == ESP_OK) {
        const httpd_uri_t uris[] = {
            {.uri = "/", .method = HTTP_GET, .handler = page_get},
            {.uri = "/info", .method = HTTP_GET, .handler = info_get},
            {.uri = "/save", .method = HTTP_POST, .handler = save_post},
            {.uri = "/status", .method = HTTP_GET, .handler = status_get},
        };
        for (int i = 0; i < 4; i++) httpd_register_uri_handler(s_server, &uris[i]);
    }
    esp_timer_start_once(s_close_timer, SETUP_WINDOW_S * 1000000LL);
    event_log("provisioning_entered", "hotspot %s for %d s", ssid, SETUP_WINDOW_S);
}

static void do_stop(void)
{
    if (!atomic_load(&s_active)) return;
    esp_timer_stop(s_close_timer);
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
    wifi_link_ap_stop();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    explicit_bzero(s_pending, sizeof(*s_pending));
    xSemaphoreGive(s_lock);
    atomic_store(&s_active, false);
    status_led_set(LED_SETUP, false);
    event_log("provisioning_closed", "%s", "window ended");
    uplink_trigger();                            // try the new settings right away
}

static void do_test(void)
{
    pending_t *p = heap_caps_malloc(sizeof(*p), MALLOC_CAP_SPIRAM);
    if (!p) { atomic_store(&s_testing, false); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *p = *s_pending;
    xSemaphoreGive(s_lock);

    bool ok = false;
    if (wifi_link_session_begin(60000)) {        // waits for an upload window to finish
        ok = wifi_link_connect(p->ssid, p->pass, WIFI_CONNECT_TIMEOUT_S * 1000);
        wifi_link_session_end();
    }
    if (ok) {                                    // commit only settings that are proven to work
        config_set_wifi(p->ssid, p->pass);
        config_set_names(p->store, p->sales);
        bool token_ok = !p->token[0] || config_set_token(p->token) == ESP_OK;
        bool devid_ok = config_set_backend_id(p->devid) == ESP_OK;   // empty clears it
        set_result(!token_ok ? "Connected to %s, but the API token was not accepted (invalid characters)."
                   : !devid_ok ? "Connected to %s, but the device ID was not accepted (lowercase letters and digits only)."
                   : "Connected to %s. Setup done: the hotspot closes in 30 s.", p->ssid);
        event_log("wifi_updated", "ssid=%s store=%s sales=%s devid=%s token=%s", p->ssid, p->store, p->sales, p->devid,
                  p->token[0] ? (token_ok ? "updated" : "invalid") : "kept");
        esp_timer_stop(s_close_timer);
        esp_timer_start_once(s_close_timer, 30 * 1000000LL);   // let the page show the result
    } else {
        set_result("Could not connect to %s. Check the name and password and try again.", p->ssid);
        event_log("wifi_setup_failed", "ssid=%s reason=%d", p->ssid, wifi_link_last_reason());
    }
    explicit_bzero(p, sizeof(*p));
    free(p);
    atomic_store(&s_testing, false);
}

static void portal_task(void *arg)
{
    cmd_t c;
    while (xQueueReceive(s_cmds, &c, portMAX_DELAY)) {
        if (c == CMD_START) do_start();
        else if (c == CMD_STOP) do_stop();
        else if (c == CMD_TEST) do_test();
    }
}

void setup_portal_init(void)
{
    s_cmds = xQueueCreate(4, sizeof(cmd_t));
    s_lock = xSemaphoreCreateMutex();
    s_pending = heap_caps_calloc(1, sizeof(pending_t), MALLOC_CAP_SPIRAM);
    esp_timer_create_args_t t = {.callback = close_cb, .name = "setup_close", .dispatch_method = ESP_TIMER_TASK};
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_close_timer));
    xTaskCreate(portal_task, "portal", 6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "ready");
}

void setup_portal_start(void) { cmd_t c = CMD_START; xQueueSend(s_cmds, &c, 0); }
void setup_portal_stop(void) { cmd_t c = CMD_STOP; xQueueSend(s_cmds, &c, 0); }
bool setup_portal_active(void) { return atomic_load(&s_active); }
