#include "voice_rec.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp_board.h"
#include "tca9555_driver.h"
#include "led_web.h"

#define SAMPLE_RATE   16000
#define MAX_SECONDS   60     // ponytail: fixed PSRAM buffer (1.9 MB); stream to SD while recording if you need longer
#define CHUNK_FRAMES  160    // 10 ms per read
#define MIC_CHANNEL   1      // raw feed is 4 ch "RMNM": 0=ref, 1=mic, 2=none, 3=mic
#define PA_PIN        IO_EXPANDER_PIN_NUM_8   // speaker amplifier enable (vendor audio_driver.c)
#define MOUNT         "/sdcard"
#define LEVEL_GAIN    1.6f   // LED meter sensitivity: raise if normal speech barely lights it

static const char *TAG = "voice_rec";

// The three user keys on the TCA9555 expander, active low (vendor button_driver.c).
static const struct { uint32_t pin; const char *name; } KEYS[] = {
    { IO_EXPANDER_PIN_NUM_9,  "KEY 9"  },
    { IO_EXPANDER_PIN_NUM_10, "KEY 10" },
    { IO_EXPANDER_PIN_NUM_11, "KEY 11" },
};

static int16_t *s_rec;        // mono samples, in PSRAM
static bool s_sd_ok;

static bool key_down(uint32_t pin) { return !Read_EXIO(pin); }

static void write_wav(const char *path, const int16_t *pcm, size_t samples)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", path);
        return;
    }
    uint32_t data_len = samples * 2, riff_len = 36 + data_len;
    uint32_t fmt_len = 16, rate = SAMPLE_RATE, byte_rate = SAMPLE_RATE * 2;
    uint16_t pcm_fmt = 1, channels = 1, block_align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff_len, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmt_len, 4, 1, f);
    fwrite(&pcm_fmt, 2, 1, f); fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_len, 4, 1, f);
    size_t written = fwrite(pcm, 2, samples, f);
    fclose(f);
    ESP_LOGI(TAG, "Wrote %s (%u bytes)%s", path, (unsigned)(44 + written * 2),
             written == samples ? "" : " -- SHORT WRITE, card full?");
}

// Next free REC##### number: neither .WAV (pending) nor .SNT (uploaded) exists.
// FAT here is 8.3 names only.
static int next_index(void)
{
    struct stat st;
    char path[32];
    for (int i = 1; i < 100000; i++) {
        snprintf(path, sizeof(path), MOUNT "/REC%05d.WAV", i);
        if (stat(path, &st) == 0) continue;
        snprintf(path, sizeof(path), MOUNT "/REC%05d.SNT", i);
        if (stat(path, &st) != 0) return i;
    }
    return 0;
}

static void play(const int16_t *pcm, size_t samples)
{
    static int16_t stereo[CHUNK_FRAMES * 2];
    Set_EXIO(PA_PIN, 1);
    for (size_t i = 0; i < samples; i += CHUNK_FRAMES) {
        size_t n = samples - i < CHUNK_FRAMES ? samples - i : CHUNK_FRAMES;
        for (size_t j = 0; j < n; j++) {
            stereo[2 * j] = stereo[2 * j + 1] = pcm[i + j];
        }
        esp_audio_play(stereo, n * 2 * sizeof(int16_t), portMAX_DELAY);
    }
    // Flush the I2S DMA with silence so the tail isn't cut off when the amp turns off.
    memset(stereo, 0, sizeof(stereo));
    for (int k = 0; k < 10; k++) esp_audio_play(stereo, sizeof(stereo), portMAX_DELAY);
    Set_EXIO(PA_PIN, 0);
}

static void voice_rec_task(void *arg)
{
    static int16_t raw[CHUNK_FRAMES * 4];
    const size_t max_samples = SAMPLE_RATE * MAX_SECONDS;
    ESP_LOGI(TAG, "Ready: hold KEY 9, 10 or 11 to record");

    while (1) {
        int k = -1;
        for (int i = 0; i < 3 && k < 0; i++) if (key_down(KEYS[i].pin)) k = i;
        if (k < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        ESP_LOGI(TAG, "%s pressed: recording...", KEYS[k].name);
        led_set_mode(LED_RECORDING);
        size_t n = 0;
        while (key_down(KEYS[k].pin) && n < max_samples) {
            esp_get_feed_data(true, raw, sizeof(raw));   // blocks ~10 ms for a chunk
            int peak = 0;
            for (int f = 0; f < CHUNK_FRAMES && n < max_samples; f++) {
                int16_t s = raw[4 * f + MIC_CHANNEL];
                s_rec[n++] = s;
                if (abs(s) > peak) peak = abs(s);
            }
            float lvl = sqrtf(peak / 32768.0f) * LEVEL_GAIN;   // sqrt: closer to how loud it sounds
            led_set_level(lvl > 1 ? 1 : lvl);
        }
        led_set_mode(LED_IDLE);
        if (n >= max_samples) ESP_LOGW(TAG, "Hit %d s limit", MAX_SECONDS);
        ESP_LOGI(TAG, "%s released: %.2f s recorded", KEYS[k].name, (float)n / SAMPLE_RATE);

        if (s_sd_ok) {
            // Write as .TMP, then rename: the uploader only picks up finished .WAV files.
            char tmp[32], path[32];
            int i = next_index();
            snprintf(tmp, sizeof(tmp), MOUNT "/REC%05d.TMP", i);
            snprintf(path, sizeof(path), MOUNT "/REC%05d.WAV", i);
            write_wav(tmp, s_rec, n);
            if (rename(tmp, path) == 0) ESP_LOGI(TAG, "Saved %s", path);
        }
        ESP_LOGI(TAG, "Playing back...");
        play(s_rec, n);
        ESP_LOGI(TAG, "Done");

        while (key_down(KEYS[k].pin)) vTaskDelay(pdMS_TO_TICKS(20));   // if we hit the limit
    }
}

void voice_rec_start(void)
{
    s_rec = heap_caps_malloc(SAMPLE_RATE * MAX_SECONDS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    assert(s_rec);

    esp_err_t err = esp_sdcard_init(MOUNT, 5);
    s_sd_ok = (err == ESP_OK);
    if (!s_sd_ok) {
        ESP_LOGE(TAG, "SD card mount failed (%s): recordings will only be played, not saved",
                 esp_err_to_name(err));
    }
    xTaskCreate(voice_rec_task, "voice_rec", 6 * 1024, NULL, 5, NULL);
}
