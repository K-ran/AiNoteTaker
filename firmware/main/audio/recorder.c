#include "recorder.h"
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "encoder/impl/esp_opus_enc.h"
#include "app_config.h"
#include "capture.h"
#include "ogg_opus.h"
#include "chunk_store.h"
#include "device_config.h"
#include "timekeep.h"
#include "event_log.h"
#include "status_led.h"

static const char *TAG = "recorder";
static atomic_int s_pause_req = -1;         // -1 none, 0 resume, 1 pause (set by any task)
static atomic_bool s_rotate;
static atomic_int s_state = REC_STOPPED;
static atomic_uint s_last_seq;
static TaskHandle_t s_task;

static void *open_encoder(uint16_t *pre_skip)
{
    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = SAMPLE_RATE_HZ;
    cfg.channel = 1;
    cfg.bitrate = OPUS_BITRATE_BPS;
    cfg.complexity = OPUS_COMPLEXITY;
    cfg.enable_vbr = true;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    void *enc = NULL;
    if (esp_opus_enc_open(&cfg, sizeof(cfg), &enc) != ESP_AUDIO_ERR_OK) return NULL;
    *pre_skip = 312;                         // libopus lookahead at 16 kHz is 104 samples = 312 at 48 kHz
    esp_audio_enc_info_t info = {0};         // get_info may leave codec_spec_info unset
    if (esp_opus_enc_get_info(enc, &info) == ESP_AUDIO_ERR_OK && info.codec_spec_info &&
        info.spec_info_len >= 12 && !memcmp(info.codec_spec_info, "OpusHead", 8)) {
        *pre_skip = info.codec_spec_info[10] | (info.codec_spec_info[11] << 8);
    }
    return enc;
}

// The chunk's metadata also travels inside the file as Ogg comment tags.
static bool begin_ogg(ogg_opus_t *ogg, chunk_t *c, uint16_t pre_skip)
{
    char store[33], sales[33], iso[24];
    config_get_names(store, sizeof(store), sales, sizeof(sales));
    timekeep_iso(iso, sizeof(iso));
    char t_dev[40], t_store[48], t_sales[48], t_seq[24], t_start[40], t_fw[48];
    snprintf(t_dev, sizeof(t_dev), "DEVICE_ID=%s", config_device_id());
    snprintf(t_store, sizeof(t_store), "STORE=%s", store);
    snprintf(t_sales, sizeof(t_sales), "SALESPERSON=%s", sales);
    snprintf(t_seq, sizeof(t_seq), "SEQ=%lu", (unsigned long)c->seq);
    snprintf(t_start, sizeof(t_start), "START_UTC=%s", iso);
    snprintf(t_fw, sizeof(t_fw), "FIRMWARE=%s", esp_app_get_description()->version);
    const char *tags[] = {t_dev, t_store, t_sales, t_seq, t_start, t_fw};
    return ogg_opus_begin(ogg, c->f, esp_random(), SAMPLE_RATE_HZ, pre_skip, "smaarthi-notetaker", tags, 6) == 0;
}

// After an OTA (P1) the bootloader rolls back unless the new image confirms itself.
// Recording one chunk end-to-end is our definition of "this firmware works".
static void confirm_firmware_once(void)
{
    static bool done;
    if (done) return;
    done = true;
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        event_log("ota_result", "firmware %s confirmed", esp_app_get_description()->version);
    }
}

static void recorder_task(void *arg)
{
    static int16_t pcm[FRAME_SAMPLES];
    static uint8_t pkt[1500];
    static ogg_opus_t ogg;                   // 16 KB page buffer: keep off the stack
    chunk_t chunk = {0};
    void *enc = NULL;                        // one encoder per chunk: each file is an independent stream
    uint16_t pre_skip = 312;
    uint32_t frames = 0, last_overruns = 0;
    int64_t last_flush = 0, pause_until = 0, retry_at = 0;
    int retry_delay_s = 1;
    bool paused = false, space_warned = false;

    esp_task_wdt_add(NULL);                  // a hung encoder or SD write reboots the device

    while (1) {
        esp_task_wdt_reset();
        bool have = capture_read(pcm, FRAME_SAMPLES, 200);
        int64_t now = esp_timer_get_time();

        int req = atomic_exchange(&s_pause_req, -1);   // only this task changes pause state
        if (req >= 0 && req != paused) {
            paused = req;
            pause_until = paused ? now + PAUSE_AUTO_RESUME_S * 1000000LL : 0;
            status_led_set(LED_PAUSED, paused);
            event_log(paused ? "privacy_pause_on" : "privacy_pause_off", "%s",
                      paused ? "auto-resume scheduled" : "resumed");
        } else if (paused && now > pause_until) {
            paused = false;                  // auto-resume so a forgotten pause can't lose a day
            status_led_set(LED_PAUSED, false);
            event_log("privacy_pause_off", "%s", "auto-resume");
        }

        uint32_t ov = capture_overruns();
        if (ov != last_overruns) {           // dropped audio compresses time: make it visible
            event_log("buffer_overrun", "frames=%lu seq=%lu", (unsigned long)(ov - last_overruns), (unsigned long)chunk.seq);
            last_overruns = ov;
        }

        bool open = chunk.f != NULL;
        bool can_record = chunk_store_ok() && !paused;
        if (can_record && !open && !chunk_store_has_space()) {
            can_record = false;
            if (!space_warned) { event_log("sd_full", "%s", "stopped recording; backlog kept"); space_warned = true; }
        } else if (can_record && space_warned) {
            space_warned = false;
        }
        status_led_set(LED_PROBLEM_SD, !chunk_store_ok() || space_warned);

        if (open && (!can_record || atomic_load(&s_rotate) || frames >= CHUNK_SECONDS * 1000 / 20)) {
            ogg_opus_end(&ogg);
            if (chunk_finish(&chunk, frames * 20, ogg.bytes_written)) {
                atomic_store(&s_last_seq, chunk.seq);
                confirm_firmware_once();
            }
            esp_opus_enc_close(enc);
            enc = NULL;
            open = false;
            atomic_store(&s_rotate, false);
        }
        atomic_store(&s_state, paused ? REC_PAUSED : can_record ? REC_RECORDING : REC_STOPPED);
        if (!can_record || !have || now < retry_at) continue;

        if (!open) {
            enc = open_encoder(&pre_skip);
            status_led_set(LED_PROBLEM_ENCODER, enc == NULL);
            bool ok = enc && chunk_open(&chunk);
            if (ok && !begin_ogg(&ogg, &chunk, pre_skip)) {
                chunk_finish(&chunk, 0, 0);
                ok = false;
            }
            if (!ok) {
                if (enc) { esp_opus_enc_close(enc); enc = NULL; }
                retry_at = now + retry_delay_s * 1000000LL;          // back off: 1, 2, 4 ... 60 s
                retry_delay_s = retry_delay_s < 60 ? retry_delay_s * 2 : 60;
                continue;
            }
            retry_delay_s = 1;
            frames = 0;
            last_flush = now;
            ESP_LOGI(TAG, "chunk %lu started", (unsigned long)chunk.seq);
        }

        esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)pcm, .len = sizeof(pcm)};
        esp_audio_enc_out_frame_t out = {.buffer = pkt, .len = sizeof(pkt)};
        if (esp_opus_enc_process(enc, &in, &out) != ESP_AUDIO_ERR_OK) continue;
        if (ogg_opus_write_packet(&ogg, pkt, out.encoded_bytes, 960) != 0) {
            event_log("sd_fault", "write failed seq=%lu", (unsigned long)chunk.seq);
            chunk_store_report_error();
            atomic_store(&s_rotate, true);
        }
        frames++;

        if (now - last_flush >= FLUSH_SECONDS * 1000000LL) {
            ogg_opus_flush(&ogg);
            if (fflush(chunk.f) != 0 || fsync(fileno(chunk.f)) != 0) {   // bounds loss on a power cut
                chunk_store_report_error();
                atomic_store(&s_rotate, true);
            }
            last_flush = now;
        }
    }
}

void recorder_start(void)
{
    // libopus keeps large analysis arrays on the stack. The stack must be internal RAM
    // because this task writes NVS (flash writes disable the PSRAM cache).
    xTaskCreatePinnedToCore(recorder_task, "recorder", RECORDER_STACK, NULL, 6, &s_task, 1);
}

void recorder_set_paused(bool paused) { atomic_store(&s_pause_req, paused ? 1 : 0); }
bool recorder_paused(void) { return atomic_load(&s_state) == REC_PAUSED; }
void recorder_toggle_pause(void) { recorder_set_paused(!recorder_paused()); }
void recorder_rotate(void) { atomic_store(&s_rotate, true); }
const char *recorder_state_name(void)
{
    int s = atomic_load(&s_state);
    return s == REC_RECORDING ? "recording" : s == REC_PAUSED ? "paused" : "stopped";
}
uint32_t recorder_last_seq(void) { return atomic_load(&s_last_seq); }
TaskHandle_t recorder_task_handle(void) { return s_task; }
