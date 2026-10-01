#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "nrf.h"

static inline NRF_GPIO_Type* gpio_port(uint32_t pin) { return (pin & 32) ? NRF_P1 : NRF_P0; }
static inline uint32_t gpio_bit(uint32_t pin) { return 1u << (pin & 31); }

static inline void gpio_set(uint32_t pin)   { gpio_port(pin)->OUTSET = gpio_bit(pin); }
static inline void gpio_clr(uint32_t pin)   { gpio_port(pin)->OUTCLR = gpio_bit(pin); }
static inline void gpio_write(uint32_t pin, bool v) { if (v) gpio_set(pin); else gpio_clr(pin); }
static inline bool gpio_read(uint32_t pin)  { return (gpio_port(pin)->IN & gpio_bit(pin)) != 0; }

static inline void gpio_output(uint32_t pin, bool initial) {
  gpio_write(pin, initial);
  gpio_port(pin)->PIN_CNF[pin & 31] = (GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos)
                                    | (GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos);
}

// pull: GPIO_PIN_CNF_PULL_Disabled / _Pulldown / _Pullup
static inline void gpio_input(uint32_t pin, uint32_t pull) {
  gpio_port(pin)->PIN_CNF[pin & 31] = (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos)
                                    | (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos)
                                    | (pull << GPIO_PIN_CNF_PULL_Pos);
}

// fully disconnected (lowest power default state)
static inline void gpio_disconnect(uint32_t pin) {
  gpio_port(pin)->PIN_CNF[pin & 31] = (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos)
                                    | (GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos);
}
