#pragma once
// PCF85063 RTC on the board's I2C bus (address 0x51). Times are UTC.
#include <stdbool.h>
#include <time.h>
#include "driver/i2c_master.h"

esp_err_t pcf85063_init(i2c_master_bus_handle_t bus);
// false when the oscillator stopped (power lost) or the stored date is implausible.
bool pcf85063_read(time_t *out);
esp_err_t pcf85063_write(time_t t);
