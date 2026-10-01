#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint16_t adc_read_raw(uint8_t ain, uint8_t samples);   // averaged 12-bit reading (0.6V ref, gain 1/6, 3us)
uint16_t adc_read(uint8_t psel, uint8_t tacq, uint8_t samples);   // any SAADC input / acquisition time
void     adc_calibrate_offset(void);
int32_t  adc_mcu_temp_x4(void);                        // die temperature in 0.25 C

#ifdef __cplusplus
}
#endif
