// Minimal Arduino API shim so MeshCore sources compile unchanged on the bare-metal HAL.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "hal/system.h"

#ifdef __cplusplus
#include <algorithm>
#include "Stream.h"

using std::min;
using std::max;

template <class T, class L, class H>
static inline T constrain(T x, L lo, H hi) { return x < lo ? (T)lo : (x > hi ? (T)hi : x); }

static inline unsigned long millis() { return sys_millis(); }
static inline unsigned long micros() { return (unsigned long)((sys_ticks() * 1000000ull) >> 15); }
static inline void delay(unsigned long ms) { sys_delay_ms(ms); }
static inline void yield() {}

extern "C" char* ltoa(long value, char* str, int base);
extern "C" char* ultoa(unsigned long value, char* str, int base);

// Arduino random(): [min, max)
long random(long max);
long random(long min, long max);
void randomSeed(unsigned long seed);

// USB CDC console
class USBSerial : public Stream {
public:
  void begin(unsigned long) {}
  int available() override;
  int read() override;
  int peek() override;
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buf, size_t n) override;
  using Print::write;
  void flush() override;
  operator bool();
};
extern USBSerial Serial;

#endif
