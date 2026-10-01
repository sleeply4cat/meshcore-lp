// One-shot SAADC reading, configured exactly like Adafruit's analogRead() (internal 0.6V ref,
// gain 1/6, 3us TACQ, 12 bit) so MeshCore's adc.multiplier values keep their meaning.
// The SAADC is fully disabled between readings.
#include "adc.h"
#include "nrf.h"

uint16_t adc_read_raw(uint8_t ain, uint8_t samples) {
  return adc_read(ain + 1, SAADC_CH_CONFIG_TACQ_3us, samples);
}

// psel: SAADC PSELP value (AIN0 = 1 ... AIN7 = 8, VDD = 9, VDDHDIV5 = 0x0D); tacq: SAADC_CH_CONFIG_TACQ_*
uint16_t adc_read(uint8_t psel, uint8_t tacq, uint8_t samples) {
  static volatile int16_t result;
  uint32_t sum = 0;

  NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_12bit;
  NRF_SAADC->OVERSAMPLE = SAADC_OVERSAMPLE_OVERSAMPLE_Bypass;
  NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled;
  for (int i = 0; i < 8; i++) { NRF_SAADC->CH[i].PSELN = 0; NRF_SAADC->CH[i].PSELP = 0; }
  NRF_SAADC->CH[0].CONFIG = (SAADC_CH_CONFIG_RESP_Bypass << SAADC_CH_CONFIG_RESP_Pos)
                          | (SAADC_CH_CONFIG_RESN_Bypass << SAADC_CH_CONFIG_RESN_Pos)
                          | (SAADC_CH_CONFIG_GAIN_Gain1_6 << SAADC_CH_CONFIG_GAIN_Pos)
                          | (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos)
                          | ((uint32_t)tacq << SAADC_CH_CONFIG_TACQ_Pos)
                          | (SAADC_CH_CONFIG_MODE_SE << SAADC_CH_CONFIG_MODE_Pos)
                          | (SAADC_CH_CONFIG_BURST_Disabled << SAADC_CH_CONFIG_BURST_Pos);
  NRF_SAADC->CH[0].PSELN = SAADC_CH_PSELN_PSELN_NC;
  NRF_SAADC->CH[0].PSELP = (uint32_t)psel << SAADC_CH_PSELP_PSELP_Pos;

  for (uint8_t s = 0; s < samples; s++) {
    NRF_SAADC->RESULT.PTR = (uint32_t)&result;
    NRF_SAADC->RESULT.MAXCNT = 1;
    NRF_SAADC->EVENTS_STARTED = 0;
    NRF_SAADC->TASKS_START = 1;
    while (!NRF_SAADC->EVENTS_STARTED) { }
    NRF_SAADC->EVENTS_END = 0;
    NRF_SAADC->TASKS_SAMPLE = 1;
    while (!NRF_SAADC->EVENTS_END) { }
    NRF_SAADC->EVENTS_STOPPED = 0;
    NRF_SAADC->TASKS_STOP = 1;
    while (!NRF_SAADC->EVENTS_STOPPED) { }
    int16_t v = result;
    sum += v < 0 ? 0 : (uint16_t)v;
  }

  NRF_SAADC->CH[0].PSELP = SAADC_CH_PSELP_PSELP_NC;
  NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled;
  return (uint16_t)(sum / (samples ? samples : 1));
}

void adc_calibrate_offset(void) {
  NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled;
  NRF_SAADC->EVENTS_CALIBRATEDONE = 0;
  NRF_SAADC->TASKS_CALIBRATEOFFSET = 1;
  for (int i = 0; i < 200000 && !NRF_SAADC->EVENTS_CALIBRATEDONE; i++) { }
  NRF_SAADC->EVENTS_CALIBRATEDONE = 0;
  NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled;
}

int32_t adc_mcu_temp_x4(void) {
  NRF_TEMP->EVENTS_DATARDY = 0;
  NRF_TEMP->TASKS_START = 1;
  for (int i = 0; i < 20000 && !NRF_TEMP->EVENTS_DATARDY; i++) { }
  NRF_TEMP->EVENTS_DATARDY = 0;
  int32_t t = NRF_TEMP->TEMP;
  NRF_TEMP->TASKS_STOP = 1;
  return t;
}
