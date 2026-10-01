// Encoder-only subset of ElectronicCats CayenneLPP 1.6.1 (byte-identical output for the used types).
#pragma once
#include <stdint.h>
#include <stdlib.h>

#define LPP_TEMPERATURE  103   // 2 bytes, 0.1 C signed
#define LPP_VOLTAGE      116   // 2 bytes, 0.01 V unsigned
#define LPP_GPS          136   // 9 bytes

class CayenneLPP {
  uint8_t* _buffer;
  uint8_t  _maxsize;
  uint8_t  _cursor = 0;

  uint8_t addField(uint8_t type, uint8_t channel, float value, uint32_t mult, uint8_t size, bool is_signed) {
    if (_cursor + size + 2 > _maxsize) return 0;
    bool sign = value < 0;
    if (sign) value = -value;
    uint32_t v = value * mult;
    if (is_signed && sign) {
      uint32_t mask = (1u << (size * 8)) - 1;
      v = v & mask;
      v = mask - v + 1;
    }
    _buffer[_cursor++] = channel;
    _buffer[_cursor++] = type;
    for (uint8_t i = 1; i <= size; i++) { _buffer[_cursor + size - i] = v & 0xFF; v >>= 8; }
    _cursor += size;
    return _cursor;
  }

public:
  explicit CayenneLPP(uint8_t size) : _maxsize(size) { _buffer = (uint8_t*)malloc(size); }
  ~CayenneLPP() { free(_buffer); }
  void reset() { _cursor = 0; }
  uint8_t getSize() const { return _cursor; }
  uint8_t* getBuffer() { return _buffer; }
  uint8_t addVoltage(uint8_t channel, float v) { return addField(LPP_VOLTAGE, channel, v, 100, 2, false); }
  uint8_t addTemperature(uint8_t channel, float t) { return addField(LPP_TEMPERATURE, channel, t, 10, 2, true); }
};
