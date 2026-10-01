#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void   usb_init(void);            // arms VBUS detection; USB stack only runs while VBUS present
void   usb_task(void);            // call from main loop
bool   usb_vbus(void);            // VBUS present
bool   usb_connected(void);       // host has the CDC port open (DTR)
int    usb_read(void);            // -1 if nothing
size_t usb_write(const void* data, size_t len);   // non-blocking-ish (short wait if port open)
void   usb_flush(void);
void   usb_discard_input(void);
uint32_t usb_connected_ms(void);   // millis() since DTR went up (0 if not connected)

#ifdef __cplusplus
}
#endif
