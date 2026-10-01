#include "console.h"
#include "usb.h"
#include <stdio.h>
#include <stdarg.h>

void con_printf(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
  usb_write(buf, n);
}

void con_write(const char* s, int n) {
  usb_write(s, n);
}

// newlib stdout -> USB CDC
int _write(int fd, const char* buf, int len) {
  (void)fd;
  usb_write(buf, len);
  return len;
}

// simple line editor: returns pointer to a complete line or NULL
char* con_readline(void) {
  static char line[192];
  static int len = 0;
  int c;
  while ((c = usb_read()) >= 0) {
    if (c == '\r' || c == '\n') {
      if (len == 0) continue;
      line[len] = 0;
      len = 0;
      con_write("\r\n", 2);
      return line;
    }
    if (c == 8 || c == 127) {
      if (len > 0) { len--; con_write("\b \b", 3); }
      continue;
    }
    if (len < (int)sizeof(line) - 1 && c >= 32) {
      line[len++] = (char)c;
      char ch = (char)c;
      con_write(&ch, 1);
    }
  }
  return NULL;
}
