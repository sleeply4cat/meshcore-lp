// DateTime subset of Adafruit RTClib (only what MeshCore uses).
#pragma once
#include <stdint.h>

class DateTime {
  uint16_t y; uint8_t m, d, hh, mm, ss;
  uint32_t t;
public:
  DateTime(uint32_t unix_time = 0) : t(unix_time) {
    uint32_t days = unix_time / 86400u, rem = unix_time % 86400u;
    hh = rem / 3600u; mm = (rem % 3600u) / 60u; ss = rem % 60u;
    // civil-from-days (Howard Hinnant), valid for 1970+
    uint32_t z = days + 719468u;
    uint32_t era = z / 146097u;
    uint32_t doe = z - era * 146097u;
    uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint32_t mp = (5u * doy + 2u) / 153u;
    d = doy - (153u * mp + 2u) / 5u + 1u;
    m = mp < 10u ? mp + 3u : mp - 9u;
    y = yoe + era * 400u + (m <= 2u ? 1u : 0u);
  }
  uint16_t year() const { return y; }
  uint8_t month() const { return m; }
  uint8_t day() const { return d; }
  uint8_t hour() const { return hh; }
  uint8_t minute() const { return mm; }
  uint8_t second() const { return ss; }
  uint32_t unixtime() const { return t; }
};
