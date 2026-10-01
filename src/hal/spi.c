// SPIM2 master for the SX1262. NSS is driven manually (SX126x needs NSS held over a command).
#include "spi.h"
#include "board.h"
#include "gpio.h"
#include "nrf.h"

#define SPIM NRF_SPIM2

void spi_init(void) {
  gpio_output(PIN_LORA_NSS, true);
  gpio_output(PIN_LORA_SCK, false);
  gpio_output(PIN_LORA_MOSI, false);
  gpio_input(PIN_LORA_MISO, GPIO_PIN_CNF_PULL_Disabled);

  SPIM->ENABLE = 0;
  SPIM->PSEL.SCK  = PIN_LORA_SCK;
  SPIM->PSEL.MOSI = PIN_LORA_MOSI;
  SPIM->PSEL.MISO = PIN_LORA_MISO;
  SPIM->PSEL.CSN  = 0xFFFFFFFF;
  SPIM->FREQUENCY = SPIM_FREQUENCY_FREQUENCY_M8;
  SPIM->CONFIG = (SPIM_CONFIG_ORDER_MsbFirst << SPIM_CONFIG_ORDER_Pos)
               | (SPIM_CONFIG_CPHA_Leading << SPIM_CONFIG_CPHA_Pos)
               | (SPIM_CONFIG_CPOL_ActiveHigh << SPIM_CONFIG_CPOL_Pos);
  SPIM->ORC = 0x00;
  SPIM->INTENCLR = 0xFFFFFFFF;
}

// Full duplex transfer of n bytes; tx/rx must be in RAM (EasyDMA). rx may be NULL.
static void xfer_chunk(const uint8_t* tx, uint8_t* rx, uint32_t n) {
  SPIM->TXD.PTR = (uint32_t)tx;
  SPIM->TXD.MAXCNT = tx ? n : 0;
  SPIM->RXD.PTR = (uint32_t)rx;
  SPIM->RXD.MAXCNT = rx ? n : 0;
  SPIM->EVENTS_END = 0;
  SPIM->TASKS_START = 1;
  while (!SPIM->EVENTS_END) { }
  SPIM->EVENTS_END = 0;
}

void spi_begin(void) {
  SPIM->ENABLE = SPIM_ENABLE_ENABLE_Enabled << SPIM_ENABLE_ENABLE_Pos;
  gpio_clr(PIN_LORA_NSS);
}

void spi_end(void) {
  gpio_set(PIN_LORA_NSS);
  SPIM->ENABLE = 0;   // idle SPIM draws nothing, but be explicit
}

void spi_transfer(const uint8_t* tx, uint8_t* rx, uint32_t n) {
  // EasyDMA MAXCNT is 16 bits on nRF52840, our transfers are <= 260 bytes anyway
  xfer_chunk(tx, rx, n);
}
