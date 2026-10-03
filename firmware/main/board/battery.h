#pragma once
// Battery voltage via the board's 100k/200k divider. Returns -1 when the
// BAT_ADC resistor is not fitted (BATTERY_ADC_FITTED 0 in app_config.h).
void battery_init(void);
int battery_mv(void);
