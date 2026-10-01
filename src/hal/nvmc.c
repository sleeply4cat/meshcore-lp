// Internal flash erase/write (SoftDevice is never enabled, so direct NVMC access is fine).
#include "nvmc.h"
#include "nrf.h"

static void wait_ready(void) {
  while (NRF_NVMC->READY == NVMC_READY_READY_Busy) { }
}

void nvmc_erase_page(uint32_t addr) {
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een << NVMC_CONFIG_WEN_Pos;
  wait_ready();
  NRF_NVMC->ERASEPAGE = addr;
  wait_ready();
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
  wait_ready();
}

void nvmc_write_words(uint32_t addr, const uint32_t* data, uint32_t nwords) {
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos;
  wait_ready();
  volatile uint32_t* dst = (volatile uint32_t*)addr;
  for (uint32_t i = 0; i < nwords; i++) {
    dst[i] = data[i];
    wait_ready();
  }
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
  wait_ready();
}
