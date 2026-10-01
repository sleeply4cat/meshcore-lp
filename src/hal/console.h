#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void  con_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void  con_write(const char* s, int n);
char* con_readline(void);

#ifdef __cplusplus
}
#endif
