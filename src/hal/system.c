// System layer: clocks, 32.768 kHz RTC timebase, race-free sleep, watchdog, fault capture.
#include "system.h"
#include "nrf.h"
#include <string.h>

extern uint32_t __isr_vector[];

// ---------------------------------------------------------------------------
// retained RAM (survives soft reset / watchdog, not power loss)
#define RETAIN_MAGIC 0x52544E31u   // "RTN1"
typedef struct {
  uint32_t magic;
  uint32_t fault_pc;
  uint32_t fault_lr;
  uint32_t fault_cfsr;
  uint32_t fault_count;
  uint32_t boot_stage;
  uint32_t prev_boot_stage;
  uint32_t dbg[8];
  uint32_t prev_dbg[8];
} retained_t;
static retained_t g_retained __attribute__((section(".retained")));
static uint32_t   g_prev_fault_pc;

// ---------------------------------------------------------------------------
#define RTC            NRF_RTC2
#define RTC_IRQn       RTC2_IRQn

static volatile uint32_t g_ovf;          // RTC overflow count (24-bit counter)
static volatile uint32_t g_wake;         // set by ISRs
static bool     g_lfxo;
static uint32_t g_resetreas;
static uint64_t g_sleep_ticks;
static uint32_t g_wakeups;
static uint32_t g_last_cal_ms;

void RTC2_IRQHandler(void) {
  if (RTC->EVENTS_OVRFLW) {
    RTC->EVENTS_OVRFLW = 0;
    (void)RTC->EVENTS_OVRFLW;
    g_ovf++;
  }
  if (RTC->EVENTS_COMPARE[0]) {
    RTC->EVENTS_COMPARE[0] = 0;
    (void)RTC->EVENTS_COMPARE[0];
    RTC->INTENCLR = RTC_INTENCLR_COMPARE0_Msk;
    g_wake = 1;
  }
}

uint64_t sys_ticks(void) {
  uint32_t ovf, cnt;
  uint32_t pm = __get_PRIMASK();
  __disable_irq();
  ovf = g_ovf;
  cnt = RTC->COUNTER;
  // overflow happened but the ISR has not run yet
  if (RTC->EVENTS_OVRFLW) {
    cnt = RTC->COUNTER;   // re-read, it is guaranteed to be post-wrap now
    ovf++;
  }
  if (!pm) __enable_irq();
  return ((uint64_t)ovf << 24) | cnt;
}

uint64_t sys_millis64(void) {
  return (sys_ticks() * 1000u) >> 15;
}

uint32_t sys_millis(void) {
  return (uint32_t)sys_millis64();
}

void sys_delay_us(uint32_t us) {
  // ~64 MHz core, running from flash with cache: 4 cycles per iteration is close enough
  // for the short busy waits we need (SX1262 reset pulse etc).
  volatile uint32_t n = us * 16u;
  while (n--) { __NOP(); }
}

// ---------------------------------------------------------------------------
void sys_wake(void)          { g_wake = 1; }
void sys_clear_wake(void)    { g_wake = 0; }
bool sys_wake_pending(void)  { return g_wake != 0; }

static inline void fpu_clear_pending(void) {
  // nRF52 anomaly 87: a pending FPU exception keeps the CPU from sleeping.
  __set_FPSCR(__get_FPSCR() & ~0x0000009Fu);
  (void)__get_FPSCR();
  NVIC_ClearPendingIRQ(FPU_IRQn);
}

void sys_sleep_until_ms(uint32_t deadline_ms) {
  uint64_t now_t = sys_ticks();
  uint32_t now_ms = (uint32_t)((now_t * 1000u) >> 15);
  int32_t  delta_ms = (int32_t)(deadline_ms - now_ms);
  if (delta_ms <= 0) return;
  if (delta_ms > 300000) delta_ms = 300000;             // stay well inside the 24-bit RTC range

  uint64_t target = now_t + (((uint64_t)delta_ms << 15) + 999u) / 1000u;
  if (target < now_t + 3) target = now_t + 3;           // RTC needs CC >= COUNTER + 2

  RTC->EVENTS_COMPARE[0] = 0;
  RTC->CC[0] = (uint32_t)target & 0xFFFFFFu;
  RTC->INTENSET = RTC_INTENSET_COMPARE0_Msk;

  __disable_irq();
  // re-check under interrupt lock: an ISR could have fired after the caller last looked
  if (!g_wake && sys_ticks() + 1 < target) {
    fpu_clear_pending();
    uint64_t t0 = sys_ticks();
    __DSB();
    __WFI();                   // wakes on any pending interrupt even with PRIMASK set
    g_sleep_ticks += sys_ticks() - t0;
    g_wakeups++;
  }
  __enable_irq();              // pending ISRs run here
}

void sys_delay_ms(uint32_t ms) {
  uint32_t end = sys_millis() + ms;
  while ((int32_t)(end - sys_millis()) > 0) {
    sys_sleep_until_ms(end);
  }
}

uint64_t sys_sleep_ticks(void) { return g_sleep_ticks; }
uint32_t sys_wakeups(void)     { return g_wakeups; }

// ---------------------------------------------------------------------------
// Start LFCLK from 'src'. Always issues LFCLKSTART and waits for LFCLKSTARTED: a watchdog that
// survived a soft reset keeps the RC oscillator running (LFCLKSTAT says "running") but the clock
// is not delivered to RTC until LFCLKSTART is triggered.
static bool lfclk_start(uint32_t src, uint32_t timeout_ms) {
  NRF_CLOCK->TASKS_LFCLKSTOP = 1;
  for (int i = 0; i < 1000 && (NRF_CLOCK->LFCLKSTAT & CLOCK_LFCLKSTAT_STATE_Msk); i++) sys_delay_us(10);
  NRF_CLOCK->LFCLKSRC = src << CLOCK_LFCLKSRC_SRC_Pos;
  NRF_CLOCK->EVENTS_LFCLKSTARTED = 0;
  NRF_CLOCK->TASKS_LFCLKSTART = 1;
  for (uint32_t i = 0; i < timeout_ms * 10; i++) {
    if (NRF_CLOCK->EVENTS_LFCLKSTARTED) {
      NRF_CLOCK->EVENTS_LFCLKSTARTED = 0;
      return ((NRF_CLOCK->LFCLKSTAT & CLOCK_LFCLKSTAT_SRC_Msk) >> CLOCK_LFCLKSTAT_SRC_Pos) == src;
    }
    sys_delay_us(100);
  }
  return false;
}

static bool hfxo_start(void) {
  if (NRF_CLOCK->HFCLKSTAT & CLOCK_HFCLKSTAT_SRC_Msk) return false;   // already on (USB), not ours
  NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
  NRF_CLOCK->TASKS_HFCLKSTART = 1;
  for (int i = 0; i < 50 && !NRF_CLOCK->EVENTS_HFCLKSTARTED; i++) sys_delay_us(100);
  NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
  return true;
}

static void lfrc_calibrate(void) {
  bool started = hfxo_start();
  NRF_CLOCK->EVENTS_DONE = 0;
  NRF_CLOCK->TASKS_CAL = 1;
  for (int i = 0; i < 1000 && !NRF_CLOCK->EVENTS_DONE; i++) sys_delay_us(50);
  NRF_CLOCK->EVENTS_DONE = 0;
  // USB may have claimed HFXO meanwhile; only stop it if USBD is not enabled
  if (started && NRF_USBD->ENABLE == 0) NRF_CLOCK->TASKS_HFCLKSTOP = 1;
}

bool sys_lfclk_is_xtal(void) { return g_lfxo; }

void sys_lfclk_maintain(void) {
  if (g_lfxo) return;
  uint32_t now = sys_millis();
  if (now - g_last_cal_ms >= 16000) {   // LFRC: keep within +-500 ppm
    g_last_cal_ms = now;
    lfrc_calibrate();
  }
}

// ---------------------------------------------------------------------------
void sys_wdt_start(uint32_t timeout_s) {
  NRF_WDT->CONFIG = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos) | (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos);
  NRF_WDT->CRV = timeout_s * 32768u;
  NRF_WDT->RREN = WDT_RREN_RR0_Msk;
  NRF_WDT->TASKS_START = 1;
}

void sys_wdt_feed(void) {
  NRF_WDT->RR[0] = WDT_RR_RR_Reload;
}

void sys_reset(void) {
  NVIC_SystemReset();
}

void sys_reset_to_bootloader(void) {
  sys_reset_to_bootloader_mode(0x57);   // DFU_MAGIC_UF2_RESET: bootloader stays in DFU (CDC + MSC if built in)
}

void sys_reset_to_bootloader_mode(uint8_t gpregret) {
  NRF_POWER->GPREGRET = gpregret;
  NVIC_SystemReset();
}

uint32_t sys_reset_reason(void) { return g_resetreas; }

const char* sys_reset_reason_str(void) {
  uint32_t r = g_resetreas;
  if (r & POWER_RESETREAS_RESETPIN_Msk) return "pin";
  if (r & POWER_RESETREAS_DOG_Msk)      return "watchdog";
  if (r & POWER_RESETREAS_SREQ_Msk)     return "soft";
  if (r & POWER_RESETREAS_LOCKUP_Msk)   return "lockup";
  if (r & POWER_RESETREAS_OFF_Msk)      return "wake-gpio";
  if (r & POWER_RESETREAS_VBUS_Msk)     return "wake-vbus";
  return "power-on";
}

uint32_t sys_last_fault_pc(void) { return g_prev_fault_pc; }

void sys_boot_stage(uint32_t stage) { g_retained.boot_stage = stage; }
uint32_t sys_prev_boot_stage(void) { return g_retained.prev_boot_stage; }
const uint32_t* sys_prev_dbg(void) { return g_retained.prev_dbg; }
const uint32_t* sys_dbg(void) { return g_retained.dbg; }

// ---------------------------------------------------------------------------
void HardFault_Handler_C(uint32_t* sp) {
  g_retained.magic = RETAIN_MAGIC;
  g_retained.fault_pc = sp[6];
  g_retained.fault_lr = sp[5];
  g_retained.fault_cfsr = SCB->CFSR;
  g_retained.fault_count++;
  NVIC_SystemReset();
}

__attribute__((naked)) void HardFault_Handler(void) {
  __asm volatile(
    "tst lr, #4      \n"
    "ite eq          \n"
    "mrseq r0, msp   \n"
    "mrsne r0, psp   \n"
    "b HardFault_Handler_C \n");
}
void MemoryManagement_Handler(void) __attribute__((alias("HardFault_Handler")));
void BusFault_Handler(void)         __attribute__((alias("HardFault_Handler")));
void UsageFault_Handler(void)       __attribute__((alias("HardFault_Handler")));

// ---------------------------------------------------------------------------
void sys_init(void) {
  // Our own vector table: SoftDevice/MBR forwarding state lives in RAM we overwrite.
  SCB->VTOR = (uint32_t)__isr_vector;
  __DSB();

  g_resetreas = NRF_POWER->RESETREAS;
  NRF_POWER->RESETREAS = 0xFFFFFFFF;

  if (g_retained.magic == RETAIN_MAGIC) {
    g_prev_fault_pc = g_retained.fault_pc;
    g_retained.fault_pc = 0;
    g_retained.prev_boot_stage = g_retained.boot_stage;
    g_retained.boot_stage = 0;
    memcpy(g_retained.prev_dbg, g_retained.dbg, sizeof(g_retained.dbg));
    memset(g_retained.dbg, 0, sizeof(g_retained.dbg));
  } else {
    memset(&g_retained, 0, sizeof(g_retained));
    g_retained.magic = RETAIN_MAGIC;
  }

  SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;

  // REG1 DC/DC (inductor fitted on ProMicro/nice!nano style boards, MeshCore enables it too)
  NRF_POWER->DCDCEN = 1;
  NRF_POWER->TASKS_LOWPWR = 1;

  // Low frequency clock: prefer the 32.768 kHz crystal, fall back to calibrated RC.
  g_retained.dbg[0] = NRF_CLOCK->LFCLKSTAT;
  g_retained.dbg[1] = NRF_WDT->RUNSTATUS;
  g_lfxo = lfclk_start(CLOCK_LFCLKSRC_SRC_Xtal, 1000);
  g_retained.dbg[2] = NRF_CLOCK->LFCLKSTAT | (g_lfxo << 31);
  if (!g_lfxo) {
    lfclk_start(CLOCK_LFCLKSRC_SRC_RC, 100);
    lfrc_calibrate();
  }
  g_retained.dbg[3] = NRF_CLOCK->LFCLKSTAT;

  // RTC2 as free running 32768 Hz timebase
  RTC->TASKS_STOP = 1;
  RTC->TASKS_CLEAR = 1;
  RTC->PRESCALER = 0;
  RTC->EVENTS_OVRFLW = 0;
  RTC->EVENTS_COMPARE[0] = 0;
  RTC->INTENSET = RTC_INTENSET_OVRFLW_Msk;
  NVIC_SetPriority(RTC_IRQn, 6);
  NVIC_ClearPendingIRQ(RTC_IRQn);
  NVIC_EnableIRQ(RTC_IRQn);
  RTC->TASKS_START = 1;
  // make sure the RTC really counts (a stuck LFCLK would turn every sleep into a hang)
  for (int attempt = 0; attempt < 3; attempt++) {
    sys_delay_us(2000);
    if (RTC->COUNTER != 0) break;
    g_lfxo = lfclk_start(CLOCK_LFCLKSRC_SRC_Xtal, 1000);
    if (!g_lfxo) lfclk_start(CLOCK_LFCLKSRC_SRC_RC, 100);
  }
  g_retained.dbg[4] = RTC->COUNTER;
  g_retained.dbg[5] = NRF_CLOCK->LFCLKSRCCOPY;

  g_last_cal_ms = 0;
  __enable_irq();
}

// newlib heap, bounded by the linker's heap section
extern char __HeapBase[], __HeapLimit[];
void* _sbrk(int incr) {
  static char* brk = __HeapBase;
  if (brk + incr > __HeapLimit) return (void*)-1;
  char* prev = brk;
  brk += incr;
  return prev;
}
