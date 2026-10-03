#include "battery.h"
#include "app_config.h"

#if BATTERY_ADC_FITTED
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;

void battery_init(void)
{
    adc_unit_t unit;
    adc_oneshot_io_to_channel(BATTERY_ADC_GPIO, &unit, &s_chan);
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = unit};
    adc_oneshot_new_unit(&ucfg, &s_adc);
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_oneshot_config_channel(s_adc, s_chan, &ccfg);
    adc_cali_curve_fitting_config_t cal = {.unit_id = unit, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_cali_create_scheme_curve_fitting(&cal, &s_cali);
}

int battery_mv(void)
{
    int raw, mv;
    if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK || adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return -1;
    return mv * 3;   // 200k over 100k divider
}
#else
void battery_init(void) {}
int battery_mv(void) { return -1; }
#endif
