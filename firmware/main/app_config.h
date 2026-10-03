#pragma once
// P0 tunables in one place. In P2 most of these become remote settings
// (design doc: "Remote settings"); keeping them here makes that move mechanical.

// Audio
#define SAMPLE_RATE_HZ          16000
#define MIC_CHANNEL             1       // raw ES7210 feed is 4 ch "RMNM": 0=ref, 1=mic, 2=none, 3=mic
#define FRAME_SAMPLES           320     // 20 ms at 16 kHz = one Opus frame
#define OPUS_BITRATE_BPS        24000
#define OPUS_COMPLEXITY         5       // 0..10; P1 tunes this against CPU clock and battery
#define RING_SECONDS            30      // PCM buffer between capture and encoder (absorbs SD stalls)

// Chunks and storage
#define CHUNK_SECONDS           600     // 10-minute files in P0
#define FLUSH_SECONDS           10      // max audio lost on a sudden power cut
#define SD_MOUNT                "/sdcard"
#define REC_DIR                 SD_MOUNT "/REC"
#define BAD_DIR                 SD_MOUNT "/REC/BAD"
#define LOG_DIR                 SD_MOUNT "/LOG"
#define SD_MIN_FREE_MB          50      // below this: stop recording, keep backlog (card-full policy)
#define LOG_MAX_BYTES           (4 * 1024 * 1024)

// Uplink
#define UPLOAD_INTERVAL_S       (15 * 60)
#define FIRST_UPLOAD_DELAY_S    60
#define WIFI_CONNECT_TIMEOUT_S  20
#define REJECT_LIMIT            5       // same chunk refused this often -> moved to BAD
#define STALE_UPLOAD_ALERT_S    (4 * 3600)
#define SMAARTHI_GRAPHQL_URL    "https://backboard.smaarthi.com/graphql"
#define AUDIO_MIME              "audio/ogg"

// Staff controls
#define PAUSE_AUTO_RESUME_S     (10 * 60)
#define SETUP_HOLD_S            10
#define SETUP_WINDOW_S          (15 * 60)

// ponytail: one shared developer code for every P0 unit (setup hotspot password).
// Per-device codes arrive with the device registry in P2.
#define DEV_SETUP_CODE          "notetaker-p0"

// Developer serial console (wifi/token/upload commands). P0 developer build only.
#define DEV_CONSOLE_ENABLED     1

// Hardware options
#define BATTERY_ADC_FITTED      0       // 1 once the 0-ohm BAT_ADC resistor is fitted (disables camera)
#define BATTERY_ADC_GPIO        1       // schematic v1.1; vendor Arduino example says GPIO8: verify when fitting
#define LED_COUNT               7       // production board: 1
#define LED_GPIO                38
