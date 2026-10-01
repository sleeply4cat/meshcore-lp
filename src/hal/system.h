#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_TICK_HZ   32768u

void     sys_init(void);             // clocks, DC/DC, RTC timebase, fault handling
uint64_t sys_ticks(void);            // monotonic 32768 Hz ticks since boot
uint32_t sys_millis(void);           // wraps after ~49 days (MeshCore handles wrap)
uint64_t sys_millis64(void);
void     sys_delay_us(uint32_t us);  // busy wait (short delays only)
void     sys_delay_ms(uint32_t ms);  // sleeps (WFI) until elapsed

// Race-free event-driven sleep.
// ISRs call sys_wake() to mark "work to do". The main loop calls sys_clear_wake()
// before polling everything, then sys_sleep_until(); the sleep is skipped if any
// sys_wake() happened in between.
void     sys_wake(void);
void     sys_clear_wake(void);
bool     sys_wake_pending(void);
void     sys_sleep_until_ms(uint32_t deadline_ms);   // absolute sys_millis() value

bool     sys_lfclk_is_xtal(void);
void     sys_lfclk_maintain(void);   // periodic LFRC calibration (no-op for LFXO)

void     sys_wdt_start(uint32_t timeout_s);
void     sys_wdt_feed(void);

void     sys_reset(void);
void     sys_reset_to_bootloader(void);   // Adafruit bootloader DFU (UF2/CDC) mode
void     sys_reset_to_bootloader_mode(uint8_t gpregret);   // 0x57 USB DFU, 0xA8 BLE OTA DFU

uint32_t sys_reset_reason(void);          // captured RESETREAS at boot
const char* sys_reset_reason_str(void);

// retained crash info from a previous HardFault (0 if none)
uint32_t sys_last_fault_pc(void);
void     sys_boot_stage(uint32_t stage);     // progress marker, survives watchdog reset
uint32_t sys_prev_boot_stage(void);
const uint32_t* sys_prev_dbg(void);
const uint32_t* sys_dbg(void);

// sleep statistics
uint64_t sys_sleep_ticks(void);           // total ticks spent in WFI
uint32_t sys_wakeups(void);

#ifdef __cplusplus
}
#endif
