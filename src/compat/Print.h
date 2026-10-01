// Arduino-compatible Print (subset used by MeshCore), same number formatting as the Arduino core.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

class Print {
public:
  virtual ~Print() {}
  virtual size_t write(uint8_t c) = 0;
  virtual size_t write(const uint8_t* buf, size_t n) {
    size_t k = 0;
    while (n--) { if (write(*buf++)) k++; else break; }
    return k;
  }
  size_t write(const char* s) { return s ? write((const uint8_t*)s, strlen(s)) : 0; }
  size_t write(const char* buf, size_t n) { return write((const uint8_t*)buf, n); }
  virtual void flush() {}

  size_t print(const char* s) { return write(s); }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(unsigned char v, int base = DEC) { return print((unsigned long)v, base); }
  size_t print(int v, int base = DEC) { return print((long)v, base); }
  size_t print(unsigned int v, int base = DEC) { return print((unsigned long)v, base); }
  size_t print(long v, int base = DEC);
  size_t print(unsigned long v, int base = DEC);
  size_t print(long long v, int base = DEC);
  size_t print(unsigned long long v, int base = DEC);
  size_t print(double v, int digits = 2);

  size_t println() { return write("\r\n"); }
  template <typename T> size_t println(T v) { size_t n = print(v); return n + println(); }
  template <typename T> size_t println(T v, int f) { size_t n = print(v, f); return n + println(); }

  size_t printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

private:
  size_t printNumber(unsigned long long n, uint8_t base);
};
