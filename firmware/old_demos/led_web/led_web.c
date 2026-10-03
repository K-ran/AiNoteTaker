#include "led_web.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "led_strip.h"

// From the vendor factory demo (factory_01/main/hardeware_driver/bsp_board.h)
#define LED_GPIO        38
#define LED_COUNT       7
#define FRAME_MS        33     // ~30 fps
#define ANIM_BRIGHT     90     // 0-255 peak for recording/upload/flash animations

static const char *TAG = "led_web";
static led_strip_handle_t s_strip;

// Idle colour + on/off, set from the web page. Default: soft teal breathing.
static bool s_on = true;
static uint8_t s_rgb[3] = {0, 60, 50};

// Written by other tasks, read by the animation task. Single-word values, so
// volatile is enough: a torn read would at worst glitch one frame.
static volatile led_mode_t s_mode = LED_IDLE;
static volatile float s_level;                 // mic level 0..1 while recording
static volatile uint8_t s_flash[3];            // one-shot flash colour
static volatile TickType_t s_flash_start;
static volatile bool s_flash_pending;

// Colour wheel: hue = angle, saturation = distance from centre; slider = brightness.
// At most one request in flight; while busy only the newest colour is kept.
static const char PAGE[] =
    "<!doctype html><meta name=viewport content='width=device-width'>"
    "<title>Board LEDs</title>"
    "<style>body{font-family:sans-serif;text-align:center;margin:2em 1em}"
    "#w{position:relative;width:260px;height:260px;margin:auto;touch-action:none}"
    "#m{position:absolute;width:14px;height:14px;margin:-9px;border:2px solid #fff;"
    "border-radius:50%;box-shadow:0 0 2px #000;pointer-events:none}"
    "#sw{width:60px;height:24px;margin:1em auto;border:1px solid #888;border-radius:4px}"
    "button{font-size:1.2em;padding:.4em 1.6em;margin:.3em}</style>"
    "<h1>LEDs: <span id=s>...</span></h1>"
    "<div id=w><canvas id=c width=260 height=260></canvas><div id=m></div></div>"
    "<p>Brightness <input id=b type=range min=0 max=100 value=30></p>"
    "<div id=sw></div>"
    "<button onclick=\"go('state=on')\">On</button>"
    "<button onclick=\"go('state=off')\">Off</button>"
    "<script>"
    "const c=document.getElementById('c'),x=c.getContext('2d'),R=130,"
    "m=document.getElementById('m'),b=document.getElementById('b');"
    "let h=0,sat=0,busy=0,next=null;"
    "function rgb(h,s,v){const f=n=>{const k=(n+h/60)%6;"
    "return Math.round(255*(v-v*s*Math.max(Math.min(k,4-k,1),0)))};return[f(5),f(3),f(1)]}"
    "const img=x.createImageData(260,260);"
    "for(let y=0;y<260;y++)for(let i=0;i<260;i++){const dx=i-R,dy=y-R,d=Math.hypot(dx,dy),"
    "p=(y*260+i)*4;if(d>R)continue;"
    "const[r,g,bb]=rgb((Math.atan2(dy,dx)*180/Math.PI+360)%360,d/R,1);"
    "img.data.set([r,g,bb,255],p)}"
    "x.putImageData(img,0,0);"
    "const hex=a=>a.map(v=>v.toString(16).padStart(2,'0')).join('');"
    "function show(t){const[st,col]=t.split(' ');"
    "document.getElementById('s').textContent=st;"
    "document.getElementById('sw').style.background=st=='on'?'#'+col:'#000'}"
    "function go(q){if(busy){next=q;return}busy=1;"
    "fetch('/led?'+q).then(r=>r.text()).then(show).catch(()=>{})"
    ".finally(()=>{busy=0;if(next){const n=next;next=null;go(n)}})}"
    "function send(){go('rgb='+hex(rgb(h,sat,b.value/100)))}"
    "function pick(e){const r=c.getBoundingClientRect();let dx=e.clientX-r.left-R,"
    "dy=e.clientY-r.top-R,d=Math.hypot(dx,dy);if(d>R){dx*=R/d;dy*=R/d;d=R}"
    "h=(Math.atan2(dy,dx)*180/Math.PI+360)%360;sat=d/R;"
    "m.style.left=(dx+R)+'px';m.style.top=(dy+R)+'px';send()}"
    "c.onpointerdown=e=>{c.setPointerCapture(e.pointerId);pick(e)};"
    "c.onpointermove=e=>{if(e.buttons)pick(e)};"
    "b.oninput=send;"
    "m.style.left=R+'px';m.style.top=R+'px';"
    "fetch('/led').then(r=>r.text()).then(show);"
    "</script>";

static uint8_t u8(float v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)v; }

// Idle: slow breathing in the web-picked colour (4 s cycle, never fully dark).
static void draw_idle(uint8_t px[][3], float t)
{
    if (!s_on) return;
    float b = 0.25f + 0.75f * (0.5f - 0.5f * cosf(t * 2 * (float)M_PI / 4.0f));
    for (int i = 0; i < LED_COUNT; i++)
        for (int c = 0; c < 3; c++) px[i][c] = u8(s_rgb[c] * b);
}

// Recording: live level meter. Loud = more LEDs, shading red -> orange -> white-hot
// at the top; peaks fall back smoothly. A faint red heartbeat underneath means
// "recording" even in silence.
static void draw_recording(uint8_t px[][3], float t)
{
    static float shown;
    float lvl = s_level;
    shown = lvl > shown ? lvl : shown * 0.85f;          // fast attack, slow decay
    float lit = shown * LED_COUNT;
    float beat = 8 + 6 * sinf(t * 2 * (float)M_PI * 1.2f);   // ~72 bpm
    for (int i = 0; i < LED_COUNT; i++) {
        float f = lit - i;                               // how "on" this LED is
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        float pos = (float)i / (LED_COUNT - 1);          // 0 = bottom, 1 = top
        float r = ANIM_BRIGHT;
        float g = ANIM_BRIGHT * (pos < 0.5f ? pos * 0.9f : 0.45f + (pos - 0.5f) * 1.1f);
        float b = pos > 0.8f ? ANIM_BRIGHT * (pos - 0.8f) * 3.5f : 0;
        px[i][0] = u8(beat + r * f);
        px[i][1] = u8(g * f);
        px[i][2] = u8(b * f);
    }
}

// Uploading: a blue comet with a fading tail running round the LEDs.
static void draw_uploading(uint8_t px[][3], float t)
{
    float head = fmodf(t * LED_COUNT / 0.9f, LED_COUNT);    // one lap per 0.9 s
    for (int i = 0; i < LED_COUNT; i++) {
        float behind = fmodf(head - i + LED_COUNT, LED_COUNT);
        float f = behind < 3.5f ? 1 - behind / 3.5f : 0;
        f *= f;
        px[i][0] = u8(ANIM_BRIGHT * 0.1f * f);
        px[i][1] = u8(ANIM_BRIGHT * 0.5f * f);
        px[i][2] = u8(ANIM_BRIGHT * f);
    }
}

// The only task that touches the LED hardware.
static void led_anim_task(void *arg)
{
    uint8_t px[LED_COUNT][3];
    TickType_t last = xTaskGetTickCount();
    while (1) {
        float t = pdTICKS_TO_MS(xTaskGetTickCount()) / 1000.0f;
        memset(px, 0, sizeof(px));
        switch (s_mode) {
            case LED_RECORDING: draw_recording(px, t); break;
            case LED_UPLOADING: draw_uploading(px, t); break;
            default:            draw_idle(px, t);      break;
        }

        // One-shot flash fades out over 700 ms, blended over whatever is showing.
        if (s_flash_pending) {
            float age = pdTICKS_TO_MS(xTaskGetTickCount() - s_flash_start) / 700.0f;
            if (age >= 1) {
                s_flash_pending = false;
            } else {
                float k = 1 - age;
                for (int i = 0; i < LED_COUNT; i++)
                    for (int c = 0; c < 3; c++)
                        px[i][c] = u8(px[i][c] * (1 - k) + s_flash[c] * k);
            }
        }

        for (int i = 0; i < LED_COUNT; i++) led_strip_set_pixel(s_strip, i, px[i][0], px[i][1], px[i][2]);
        led_strip_refresh(s_strip);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(FRAME_MS));
    }
}

void led_set_mode(led_mode_t mode) { s_mode = mode; }
void led_set_level(float level)    { s_level = level; }

void led_flash(uint8_t r, uint8_t g, uint8_t b)
{
    s_flash[0] = r; s_flash[1] = g; s_flash[2] = b;
    s_flash_start = xTaskGetTickCount();
    s_flash_pending = true;
}

void led_anim_start(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,  // as in vendor driver
    };
    led_strip_rmt_config_t rmt_cfg = { .resolution_hz = 10 * 1000 * 1000 };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip));
    xTaskCreate(led_anim_task, "led_anim", 3 * 1024, NULL, 3, NULL);
}

// GET /  -> the control page
static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
}

// GET /led                 -> current state
// GET /led?state=on|off    -> switch on (last colour) / off
// GET /led?rgb=RRGGBB      -> set colour and switch on
// Replies "on rrggbb" or "off rrggbb".
static esp_err_t led_get(httpd_req_t *req)
{
    char query[32], val[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "state", val, sizeof(val)) == ESP_OK &&
            (!strcmp(val, "on") || !strcmp(val, "off"))) {
            s_on = !strcmp(val, "on");
            ESP_LOGI(TAG, "LEDs %s", val);
        } else if (httpd_query_key_value(query, "rgb", val, sizeof(val)) == ESP_OK &&
                   strlen(val) == 6 && strspn(val, "0123456789abcdefABCDEF") == 6) {
            unsigned long v = strtoul(val, NULL, 16);
            s_rgb[0] = v >> 16; s_rgb[1] = v >> 8; s_rgb[2] = v;
            s_on = true;
            ESP_LOGD(TAG, "colour %s", val);   // debug level: fires many times a second while dragging
        } else {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "use /led?state=on|off or /led?rgb=RRGGBB");
        }
        // led_anim_task picks up the new idle colour on its next frame.
    }
    char reply[16];
    snprintf(reply, sizeof(reply), "%s %02x%02x%02x", s_on ? "on" : "off", s_rgb[0], s_rgb[1], s_rgb[2]);
    return httpd_resp_sendstr(req, reply);
}

void led_web_start(void)
{
    httpd_handle_t server;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));   // starts its own task

    const httpd_uri_t root = { .uri = "/",    .method = HTTP_GET, .handler = root_get };
    const httpd_uri_t led  = { .uri = "/led", .method = HTTP_GET, .handler = led_get };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &led);
    ESP_LOGI(TAG, "Web server running on port %d", cfg.server_port);
}
