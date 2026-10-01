#include "Arduino.h"
#include "hal/usb.h"
#include "hal/rng.h"
#include "RNG.h"
#include <stdarg.h>

// ---------------------------------------------------------------- Print
size_t Print::printNumber(unsigned long long n, uint8_t base) {
  char buf[8 * sizeof(n) + 1];
  char* p = &buf[sizeof(buf) - 1];
  *p = 0;
  if (base < 2) base = 10;
  do {
    char c = n % base;
    n /= base;
    *--p = c < 10 ? c + '0' : c + 'A' - 10;
  } while (n);
  return write(p);
}

size_t Print::print(long v, int base) {
  if (base == 10 && v < 0) { size_t t = print('-'); return t + printNumber((unsigned long)(-v), 10); }
  return printNumber((unsigned long)v, base);
}
size_t Print::print(unsigned long v, int base) { return printNumber(v, base); }
size_t Print::print(long long v, int base) {
  if (base == 10 && v < 0) { size_t t = print('-'); return t + printNumber((unsigned long long)(-v), 10); }
  return printNumber((unsigned long long)v, base);
}
size_t Print::print(unsigned long long v, int base) { return printNumber(v, base); }

// identical algorithm to Arduino's Print::printFloat
size_t Print::print(double number, int digits) {
  size_t n = 0;
  if (isnan(number)) return print("nan");
  if (isinf(number)) return print("inf");
  if (number > 4294967040.0) return print("ovf");
  if (number < -4294967040.0) return print("ovf");
  if (number < 0.0) { n += print('-'); number = -number; }
  double rounding = 0.5;
  for (uint8_t i = 0; i < digits; ++i) rounding /= 10.0;
  number += rounding;
  unsigned long int_part = (unsigned long)number;
  double remainder = number - (double)int_part;
  n += print(int_part);
  if (digits > 0) n += print('.');
  while (digits-- > 0) {
    remainder *= 10.0;
    unsigned int toPrint = (unsigned int)remainder;
    n += print(toPrint);
    remainder -= toPrint;
  }
  return n;
}

size_t Print::printf(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (len < 0) return 0;
  if (len > (int)sizeof(buf) - 1) len = sizeof(buf) - 1;
  return write((const uint8_t*)buf, len);
}

// ---------------------------------------------------------------- ltoa (not in newlib)
extern "C" char* ultoa(unsigned long v, char* str, int base) {
  char tmp[33];
  int i = 0;
  if (base < 2 || base > 36) base = 10;
  do { int d = v % base; tmp[i++] = d < 10 ? '0' + d : 'a' + d - 10; v /= base; } while (v);
  int j = 0;
  while (i) str[j++] = tmp[--i];
  str[j] = 0;
  return str;
}

extern "C" char* ltoa(long v, char* str, int base) {
  if (base == 10 && v < 0) { str[0] = '-'; ultoa((unsigned long)(-v), str + 1, 10); return str; }
  return ultoa((unsigned long)v, str, base);
}

// ---------------------------------------------------------------- random
static uint32_t s_rand_state = 0x12345678;

static uint32_t xorshift32() {
  uint32_t x = s_rand_state;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return s_rand_state = x;
}

void randomSeed(unsigned long seed) { if (seed) s_rand_state = seed; }
long random(long max) { return max <= 0 ? 0 : (long)(xorshift32() % (unsigned long)max); }
long random(long min, long max) { return min >= max ? min : min + random(max - min); }

// ---------------------------------------------------------------- Serial
USBSerial Serial;

int USBSerial::available() { return usb_connected() ? 1 : 0; }   // only used as "has data?" hint
int USBSerial::read() { return usb_read(); }
int USBSerial::peek() { return -1; }
size_t USBSerial::write(uint8_t c) { usb_write(&c, 1); return 1; }
size_t USBSerial::write(const uint8_t* buf, size_t n) { usb_write(buf, n); return n; }
void USBSerial::flush() { usb_flush(); }
USBSerial::operator bool() { return usb_connected(); }

// ---------------------------------------------------------------- rweather RNG stub
RNGClass RNG;
void RNGClass::rand(uint8_t* data, size_t len) { rng_fill(data, len); }
