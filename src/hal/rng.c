// nRF52840 TRNG (RNG peripheral with bias correction). Only powered while generating.
#include "rng.h"
#include "nrf.h"

void rng_fill(void* out, size_t len) {
  uint8_t* p = (uint8_t*)out;
  NRF_RNG->CONFIG = RNG_CONFIG_DERCEN_Msk;
  NRF_RNG->EVENTS_VALRDY = 0;
  NRF_RNG->TASKS_START = 1;
  while (len--) {
    while (!NRF_RNG->EVENTS_VALRDY) { }
    NRF_RNG->EVENTS_VALRDY = 0;
    *p++ = (uint8_t)NRF_RNG->VALUE;
  }
  NRF_RNG->TASKS_STOP = 1;
}

uint32_t rng_u32(void) {
  uint32_t v;
  rng_fill(&v, sizeof(v));
  return v;
}
