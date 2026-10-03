#include "capture.h"
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp_board.h"
#include "app_config.h"

static const char *TAG = "capture";
static RingbufHandle_t s_rb;
static volatile float s_level_ms = 0;     // smoothed mean square, normalised 0..1
static volatile uint32_t s_overruns;

static void capture_task(void *arg)
{
    static int16_t raw[FRAME_SAMPLES * 4];
    static int16_t mono[FRAME_SAMPLES];
    while (1) {
        esp_get_feed_data(true, raw, sizeof(raw));   // blocks ~20 ms
        float ms = 0;
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            mono[i] = raw[4 * i + MIC_CHANNEL];
            float s = mono[i] / 32768.0f;
            ms += s * s;
        }
        s_level_ms = 0.99f * s_level_ms + 0.01f * (ms / FRAME_SAMPLES);   // ~2 s time constant
        if (xRingbufferSend(s_rb, mono, sizeof(mono), 0) != pdTRUE) s_overruns++;
    }
}

void capture_start(void)
{
    s_rb = xRingbufferCreateWithCaps(RING_SECONDS * SAMPLE_RATE_HZ * sizeof(int16_t),
                                     RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    assert(s_rb);
    xTaskCreatePinnedToCore(capture_task, "capture", 4096, NULL, 7, NULL, 1);
    ESP_LOGI(TAG, "capturing %d Hz mono from channel %d", SAMPLE_RATE_HZ, MIC_CHANNEL);
}

bool capture_read(int16_t *out, size_t samples, int timeout_ms)
{
    size_t want = samples * sizeof(int16_t), got = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (got < want) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) return false;
        size_t n;
        void *p = xRingbufferReceiveUpTo(s_rb, &n, deadline - now, want - got);
        if (!p) return false;
        memcpy((uint8_t *)out + got, p, n);
        vRingbufferReturnItem(s_rb, p);
        got += n;
    }
    return true;
}

float capture_level_dbfs(void)
{
    float ms = s_level_ms;
    return ms > 1e-10f ? 10.0f * log10f(ms) : -99.0f;
}

uint32_t capture_overruns(void) { return s_overruns; }
