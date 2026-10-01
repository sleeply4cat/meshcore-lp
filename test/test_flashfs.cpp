// Host test for src/compat/flashfs.cpp: NOR flash emulated at the real addresses via mmap.
#include <sys/mman.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csetjmp>
#include <string>
#include "Adafruit_LittleFS.h"
#include "hal/nvmc.h"

using namespace Adafruit_LittleFS_Namespace;

static uint8_t* flash;
static long write_budget = -1;          // simulated power loss after N word writes / erases
static jmp_buf power_loss;

extern "C" void nvmc_erase_page(uint32_t addr) {
  if (write_budget == 0) longjmp(power_loss, 1);
  if (write_budget > 0) write_budget--;
  memset((void*)(uintptr_t)addr, 0xFF, NVMC_PAGE_SIZE);
}
extern "C" void nvmc_write_words(uint32_t addr, const uint32_t* data, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    if (write_budget == 0) longjmp(power_loss, 1);
    if (write_budget > 0) write_budget--;
    uint32_t* d = (uint32_t*)(uintptr_t)(addr + 4 * i);
    *d &= data[i];                       // NOR: bits can only be cleared
  }
}
extern "C" void sys_wdt_feed(void) {}

static void write_file(Adafruit_LittleFS& fs, const char* name, const char* content) {
  fs.remove(name);
  File f = fs.open(name, FILE_O_WRITE);
  assert(f);
  f.write((const uint8_t*)content, strlen(content));
  f.close();
}

static std::string read_file(Adafruit_LittleFS& fs, const char* name) {
  File f = fs.open(name);
  if (!f) return "<none>";
  std::string s;
  int c;
  while ((c = f.read()) >= 0) s += (char)c;
  f.close();
  return s;
}

int main() {
  void* p = mmap((void*)0xED000, 0x7000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  assert(p == (void*)0xED000);
  flash = (uint8_t*)p;
  memset(flash, 0xFF, 0x7000);

  { // 1. basic persistence
    Adafruit_LittleFS fs; fs.begin();
    write_file(fs, "/prefs.json", "{\"name\":\"a\"}");
    write_file(fs, "/_main.id", "IDENTITY-0123456789");
    File f = fs.open("/log", FILE_O_WRITE); f.print("x"); f.close();
    f = fs.open("/log", FILE_O_WRITE); f.print("y"); f.close();   // append semantics
  }
  {
    Adafruit_LittleFS fs; fs.begin();
    assert(read_file(fs, "/prefs.json") == "{\"name\":\"a\"}");
    assert(read_file(fs, "/_main.id") == "IDENTITY-0123456789");
    assert(read_file(fs, "/log") == "xy");
    assert(read_file(fs, "/nope") == "<none>");
    printf("basic persistence ok, used %u bytes\n", fs.usedBytes());
  }
  // 2. power loss at every possible point of a commit: old or new version, never garbage
  int old_ok = 0, new_ok = 0;
  for (long budget = 0; budget < 3 * 4 + 3200; budget++) {
    {
      Adafruit_LittleFS fs; fs.begin();
      write_file(fs, "/prefs.json", "OLD");
    }
    Adafruit_LittleFS* fs = new Adafruit_LittleFS(); fs->begin();
    write_budget = budget;
    if (setjmp(power_loss) == 0) {
      write_file(*fs, "/prefs.json", "NEW-VALUE");
    }
    write_budget = -1;
    Adafruit_LittleFS fs2; fs2.begin();
    std::string v = read_file(fs2, "/prefs.json");
    std::string id = read_file(fs2, "/_main.id");
    assert(id == "IDENTITY-0123456789");
    if (v == "OLD") old_ok++; else if (v == "NEW-VALUE") new_ok++;
    else { printf("CORRUPT at budget %ld: '%s'\n", budget, v.c_str()); return 1; }
  }
  printf("power-loss sweep ok: %d old/removed, %d new\n", old_ok, new_ok);
  // 2b. a lone remove() persists after sync()
  {
    Adafruit_LittleFS fs; fs.begin();
    write_file(fs, "/tmp", "t");
    fs.remove("/tmp");
    fs.sync();
  }
  {
    Adafruit_LittleFS fs; fs.begin();
    assert(read_file(fs, "/tmp") == "<none>");
  }
  // 3. many commits: banks alternate, sequence keeps growing
  {
    Adafruit_LittleFS fs; fs.begin();
    for (int i = 0; i < 500; i++) {
      char b[32]; snprintf(b, sizeof(b), "v%d", i);
      write_file(fs, "/prefs.json", b);
    }
  }
  {
    Adafruit_LittleFS fs; fs.begin();
    assert(read_file(fs, "/prefs.json") == "v499");
  }
  // 4. oversize image is refused, previous content stays
  {
    Adafruit_LittleFS fs; fs.begin();
    std::string big(13000, 'z');
    File f = fs.open("/big", FILE_O_WRITE); f.write((const uint8_t*)big.data(), big.size()); f.close();
  }
  {
    Adafruit_LittleFS fs; fs.begin();
    assert(read_file(fs, "/prefs.json") == "v499");
    assert(read_file(fs, "/big") == "<none>");
  }
  printf("all flashfs tests passed\n");
  return 0;
}
