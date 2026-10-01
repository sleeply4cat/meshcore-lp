// SX1262 DIO1 -> MCU wakeup through GPIOTE PORT/SENSE (no HFCLK, ~0 uA), not a GPIOTE IN channel.
#include "dio1.h"
#include "board.h"
#include "gpio.h"
#include "system.h"
#include "nrf.h"

static volatile uint32_t g_edges;
static volatile uint64_t g_last_edge;

void GPIOTE_IRQHandler(void) {
  if (NRF_GPIOTE->EVENTS_PORT) {
    NRF_GPIOTE->EVENTS_PORT = 0;
    (void)NRF_GPIOTE->EVENTS_PORT;
    g_edges++;
    g_last_edge = sys_ticks();
    sys_wake();
  }
}

void dio1_init(void) {
  NRF_GPIO_Type* port = gpio_port(PIN_LORA_DIO1);
  port->DETECTMODE = 0;   // direct DETECT (only one pin senses on this port)
  port->PIN_CNF[PIN_LORA_DIO1 & 31] = (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos)
                                    | (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos)
                                    | (GPIO_PIN_CNF_PULL_Disabled << GPIO_PIN_CNF_PULL_Pos)
                                    | (GPIO_PIN_CNF_SENSE_High << GPIO_PIN_CNF_SENSE_Pos);
  NRF_GPIOTE->EVENTS_PORT = 0;
  NRF_GPIOTE->INTENSET = GPIOTE_INTENSET_PORT_Msk;
  NVIC_SetPriority(GPIOTE_IRQn, 5);
  NVIC_ClearPendingIRQ(GPIOTE_IRQn);
  NVIC_EnableIRQ(GPIOTE_IRQn);
}

bool dio1_level(void) { return gpio_read(PIN_LORA_DIO1); }
uint32_t dio1_edges(void) { return g_edges; }
uint64_t dio1_last_edge_ticks(void) { return g_last_edge; }
