// Stub replacing rweather's RNG (which pulls in Arduino EEPROM/noise sources).
// Only Ed25519::generatePrivateKey() / Curve25519::dh1() use it; backed by the nRF52 TRNG.
#pragma once
#include <stddef.h>
#include <stdint.h>

class RNGClass {
public:
  void rand(uint8_t* data, size_t len);
};

extern RNGClass RNG;
