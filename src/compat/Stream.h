#pragma once
#include "Print.h"

class Stream : public Print {
public:
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() = 0;

  size_t readBytes(uint8_t* buf, size_t len) {
    size_t n = 0;
    while (n < len) {
      int c = read();
      if (c < 0) break;
      buf[n++] = (uint8_t)c;
    }
    return n;
  }
  size_t readBytes(char* buf, size_t len) { return readBytes((uint8_t*)buf, len); }
};
