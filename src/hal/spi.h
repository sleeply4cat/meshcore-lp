#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void spi_init(void);
void spi_begin(void);     // enable SPIM, assert NSS
void spi_end(void);       // release NSS, disable SPIM
void spi_transfer(const uint8_t* tx, uint8_t* rx, uint32_t n);   // buffers must be in RAM

#ifdef __cplusplus
}
#endif
