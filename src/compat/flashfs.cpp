#include "Adafruit_LittleFS.h"
#include "InternalFileSystem.h"
#include "hal/nvmc.h"
#include "hal/system.h"
#include <stdlib.h>
#include <string.h>

using namespace Adafruit_LittleFS_Namespace;

Adafruit_LittleFS InternalFS;

// flash layout: two banks of 3 pages each inside 0xED000..0xF4000
#define BANK_SIZE   (3 * NVMC_PAGE_SIZE)
#define FS_MAGIC    0x50464C4Du   // "MLFP"
static const uint32_t bank_addr[2] = { 0xED000, 0xF0000 };

struct FsHeader { uint32_t magic, seq, len, crc; };

static uint32_t crc32(const uint8_t* p, uint32_t n) {
  uint32_t c = 0xFFFFFFFFu;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

static bool bank_valid(int b, FsHeader* out) {
  const FsHeader* h = (const FsHeader*)bank_addr[b];
  if (h->magic != FS_MAGIC || h->len > BANK_SIZE - sizeof(FsHeader)) return false;
  if (crc32((const uint8_t*)(bank_addr[b] + sizeof(FsHeader)), h->len) != h->crc) return false;
  *out = *h;
  return true;
}

static uint32_t s_image[BANK_SIZE / 4];   // serialization buffer
static int s_active_bank = -1;

int Adafruit_LittleFS::find(const char* path) const {
  for (int i = 0; i < MAX_FILES; i++) {
    if (_slots[i].used && strcmp(_slots[i].name, path) == 0) return i;
  }
  return -1;
}

bool Adafruit_LittleFS::load() {
  FsHeader h[2];
  bool v0 = bank_valid(0, &h[0]), v1 = bank_valid(1, &h[1]);
  int b = -1;
  if (v0 && v1) b = ((int32_t)(h[1].seq - h[0].seq) > 0) ? 1 : 0;
  else if (v0) b = 0;
  else if (v1) b = 1;
  if (b < 0) return false;

  s_active_bank = b;
  _seq = h[b].seq;
  const uint8_t* p = (const uint8_t*)(bank_addr[b] + sizeof(FsHeader));
  const uint8_t* end = p + h[b].len;
  int i = 0;
  while (p + 3 <= end && i < MAX_FILES) {
    uint8_t nl = *p++;
    if (nl == 0 || nl >= sizeof(_slots[0].name) || p + nl + 2 > end) break;
    Slot& s = _slots[i];
    memcpy(s.name, p, nl); s.name[nl] = 0; p += nl;
    uint16_t len = p[0] | (p[1] << 8); p += 2;
    if (p + len > end) break;
    s.data = (uint8_t*)malloc(len ? len : 1);
    if (!s.data) break;
    memcpy(s.data, p, len); p += len;
    s.len = s.cap = len;
    s.used = true; s.dirty = false;
    i++;
  }
  return true;
}

bool Adafruit_LittleFS::begin() {
  if (_mounted) return true;
  load();
  _mounted = true;
  return true;
}

uint32_t Adafruit_LittleFS::usedBytes() const {
  uint32_t n = sizeof(FsHeader);
  for (int i = 0; i < MAX_FILES; i++) if (_slots[i].used) n += 3 + strlen(_slots[i].name) + _slots[i].len;
  return n;
}

bool Adafruit_LittleFS::commit() {
  uint8_t* img = (uint8_t*)s_image;
  uint32_t n = sizeof(FsHeader);
  for (int i = 0; i < MAX_FILES; i++) {
    Slot& s = _slots[i];
    if (!s.used) continue;
    uint32_t nl = strlen(s.name);
    if (n + 3 + nl + s.len > BANK_SIZE) return false;   // does not fit
    img[n++] = (uint8_t)nl;
    memcpy(&img[n], s.name, nl); n += nl;
    img[n++] = s.len & 0xFF; img[n++] = s.len >> 8;
    memcpy(&img[n], s.data, s.len); n += s.len;
  }
  while (n & 3) img[n++] = 0xFF;

  FsHeader h = { FS_MAGIC, _seq + 1, n - (uint32_t)sizeof(FsHeader), 0 };
  h.crc = crc32(&img[sizeof(FsHeader)], h.len);

  int target = (s_active_bank == 0) ? 1 : 0;
  uint32_t base = bank_addr[target];
  for (uint32_t off = 0; off < BANK_SIZE; off += NVMC_PAGE_SIZE) {
    sys_wdt_feed();
    nvmc_erase_page(base + off);
  }
  // body first, header last: an interrupted commit leaves the previous bank authoritative
  nvmc_write_words(base + sizeof(FsHeader), &s_image[sizeof(FsHeader) / 4], h.len / 4);
  nvmc_write_words(base, (const uint32_t*)&h, sizeof(h) / 4);

  FsHeader chk;
  if (!bank_valid(target, &chk) || chk.seq != h.seq) return false;
  s_active_bank = target;
  _seq = h.seq;
  _commits++;
  _pending = false;
  for (int i = 0; i < MAX_FILES; i++) _slots[i].dirty = false;
  return true;
}

File Adafruit_LittleFS::open(const char* path, uint8_t mode) {
  File f;
  int i = find(path);
  if (i < 0) {
    if (mode != FILE_O_WRITE) return f;
    for (i = 0; i < MAX_FILES && _slots[i].used; i++) { }
    if (i >= MAX_FILES || strlen(path) >= sizeof(_slots[0].name)) return f;
    Slot& s = _slots[i];
    memset(&s, 0, sizeof(s));
    strcpy(s.name, path);
    s.used = true;
    s.dirty = true;
  }
  f._fs = this;
  f._slot = i;
  f._mode = mode;
  f._pos = (mode == FILE_O_WRITE) ? _slots[i].len : 0;   // Adafruit FILE_O_WRITE appends
  return f;
}

bool Adafruit_LittleFS::exists(const char* path) { return find(path) >= 0; }

bool Adafruit_LittleFS::remove(const char* path) {
  int i = find(path);
  if (i < 0) return false;
  free(_slots[i].data);
  memset(&_slots[i], 0, sizeof(_slots[i]));
  _pending = true;   // committed with the following write or by sync()
  return true;
}

bool Adafruit_LittleFS::format() {
  for (int i = 0; i < MAX_FILES; i++) {
    if (_slots[i].used) free(_slots[i].data);
    memset(&_slots[i], 0, sizeof(_slots[i]));
  }
  return commit();
}

// ---------------------------------------------------------------- File
namespace Adafruit_LittleFS_Namespace {

uint32_t File::size() const {
  auto s = _fs ? _fs->slot(_slot) : nullptr;
  return s ? s->len : 0;
}

int File::available() {
  auto s = _fs ? _fs->slot(_slot) : nullptr;
  return (s && _pos < s->len) ? (int)(s->len - _pos) : 0;
}

int File::read() {
  auto s = _fs ? _fs->slot(_slot) : nullptr;
  if (!s || _pos >= s->len) return -1;
  return s->data[_pos++];
}

int File::peek() {
  auto s = _fs ? _fs->slot(_slot) : nullptr;
  if (!s || _pos >= s->len) return -1;
  return s->data[_pos];
}

int File::read(void* buf, uint16_t n) {
  auto s = _fs ? _fs->slot(_slot) : nullptr;
  if (!s) return -1;
  uint32_t avail = _pos < s->len ? s->len - _pos : 0;
  if (n > avail) n = avail;
  memcpy(buf, &s->data[_pos], n);
  _pos += n;
  return n;
}

size_t File::write(const uint8_t* buf, size_t n) {
  auto s = _fs ? _fs->slot(_slot) : nullptr;
  if (!s || _mode != FILE_O_WRITE) return 0;
  uint32_t need = _pos + n;
  if (need > 0xFFFF) return 0;
  if (need > s->cap) {
    uint32_t cap = need + 64;
    uint8_t* d = (uint8_t*)realloc(s->data, cap);
    if (!d) return 0;
    s->data = d; s->cap = cap;
  }
  memcpy(&s->data[_pos], buf, n);
  _pos += n;
  if (_pos > s->len) s->len = _pos;
  s->dirty = true;
  return n;
}

size_t File::write(uint8_t c) { return write(&c, 1); }

bool File::seek(uint32_t pos) {
  if (pos > size()) return false;
  _pos = pos;
  return true;
}

void File::flush() {}

void File::close() {
  if (!_fs) return;
  auto s = _fs->slot(_slot);
  if (s && s->dirty) _fs->commit();
  else _fs->sync();
  _fs = nullptr;
  _slot = -1;
}

}  // namespace
