#include "rtc_pcf85063.h"
#include <string.h>
#include "esp_log.h"

#define RTC_ADDR      0x51
#define REG_CTRL1     0x00
#define REG_SECONDS   0x04          // bit 7 = OS (oscillator stopped: time invalid)

static const char *TAG = "rtc";
static i2c_master_dev_handle_t s_dev;

static int bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0f); }
static uint8_t dec2bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

esp_err_t pcf85063_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) return err;
    uint8_t ctrl1[2] = {REG_CTRL1, 0x00};   // normal mode, running, 24 h, 7 pF load
    return i2c_master_transmit(s_dev, ctrl1, sizeof(ctrl1), 100);
}

bool pcf85063_read(time_t *out)
{
    uint8_t reg = REG_SECONDS, r[7];
    if (!s_dev || i2c_master_transmit_receive(s_dev, &reg, 1, r, sizeof(r), 100) != ESP_OK) return false;
    if (r[0] & 0x80) return false;          // oscillator stopped since last set
    struct tm tm = {
        .tm_sec = bcd2dec(r[0] & 0x7f), .tm_min = bcd2dec(r[1] & 0x7f), .tm_hour = bcd2dec(r[2] & 0x3f),
        .tm_mday = bcd2dec(r[3] & 0x3f), .tm_mon = bcd2dec(r[5] & 0x1f) - 1, .tm_year = bcd2dec(r[6]) + 100,
    };
    if (tm.tm_year < 125) return false;      // before 2025: never set
    // TZ is UTC on this device, so mktime == timegm.
    *out = mktime(&tm);
    return true;
}

esp_err_t pcf85063_write(time_t t)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    uint8_t buf[8] = {REG_SECONDS, dec2bcd(tm.tm_sec), dec2bcd(tm.tm_min), dec2bcd(tm.tm_hour),
                      dec2bcd(tm.tm_mday), (uint8_t)tm.tm_wday, dec2bcd(tm.tm_mon + 1), dec2bcd(tm.tm_year - 100)};
    esp_err_t err = s_dev ? i2c_master_transmit(s_dev, buf, sizeof(buf), 100) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) ESP_LOGW(TAG, "write failed: %s", esp_err_to_name(err));
    return err;
}
