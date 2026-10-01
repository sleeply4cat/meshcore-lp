// Drop-in for the tiny part of Adafruit_LittleFS that MeshCore uses.
// Files live in RAM; every close() of a modified file persists the whole (small) file set
// into one of two flash banks (A/B, CRC protected, header written last).
#pragma once
#include <stdint.h>
#include "Stream.h"

class Adafruit_LittleFS;

namespace Adafruit_LittleFS_Namespace {

enum { FILE_O_READ = 0, FILE_O_WRITE = 1 };

class File : public Stream {
  friend class ::Adafruit_LittleFS;
  Adafruit_LittleFS* _fs = nullptr;
  int      _slot = -1;
  uint32_t _pos = 0;
  uint8_t  _mode = FILE_O_READ;
public:
  File() {}
  operator bool() const { return _fs != nullptr && _slot >= 0; }

  int available() override;
  int read() override;
  int peek() override;
  int read(void* buf, uint16_t n);
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buf, size_t n) override;
  using Print::write;
  bool seek(uint32_t pos);
  uint32_t position() const { return _pos; }
  uint32_t size() const;
  void flush() override;
  void close();
};

}  // namespace

class Adafruit_LittleFS {
public:
  static const int MAX_FILES = 12;
  struct Slot { char name[32]; uint8_t* data; uint16_t len, cap; bool used, dirty; };

  bool begin();
  Adafruit_LittleFS_Namespace::File open(const char* path, uint8_t mode = Adafruit_LittleFS_Namespace::FILE_O_READ);
  bool exists(const char* path);
  bool remove(const char* path);
  bool mkdir(const char* path) { return true; }
  bool format();

  // internal
  Slot* slot(int i) { return (i >= 0 && i < MAX_FILES && _slots[i].used) ? &_slots[i] : nullptr; }
  bool commit();
  // removals are committed together with the next write (MeshCore does remove()+rewrite, which
  // must not lose the file on power failure) or by sync() from the main loop
  void sync() { if (_pending) commit(); }
  uint32_t usedBytes() const;
  uint32_t commitCount() const { return _commits; }

private:
  Slot _slots[MAX_FILES] = {};
  uint32_t _seq = 0;
  uint32_t _commits = 0;
  bool _mounted = false;
  bool _pending = false;
  int find(const char* path) const;
  bool load();
};
