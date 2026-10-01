// "Radio lab" firmware: bring-up + receive-only experiments (never transmits).
//  - verifies clocks, DC/DC, USB CDC, SX1262 + TCXO
//  - prints every received LoRa packet with RSSI/SNR and preamble->header timing
//  - can run continuous RX, SX1262 RxDutyCycle, or alternate both for A/B loss comparison
#include "hal/system.h"
#include "hal/board.h"
#include "hal/gpio.h"
#include "hal/usb.h"
#include "hal/console.h"
#include "hal/sx1262.h"
#include "hal/dio1.h"
#include "nrf.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// network parameters: same build-time defaults as the repeater (config.mk / repeater.mk)
static sx_config_t cfg = {
  .freq_hz = (uint32_t)(LORA_FREQ * 1000000.0 + 0.5), .bw_khz = LORA_BW, .sf = LORA_SF, .cr = LORA_CR,
  .preamble = LORA_SF <= 8 ? 32 : 16, .tx_power = 22, .rx_boosted = true
};

enum { MODE_CONT = 0, MODE_DUTY = 1, MODE_SWIN = 2 };

// software sniff: MCU-timed RX windows with precise timestamps (measures detection latency)
static uint32_t sw_win_us = 24576, sw_smin_ms = 5, sw_smax_ms = 60;
static int      sw_state;            // 0 = radio asleep, 1 = window open
static uint32_t sw_next_open_ms;
static uint64_t sw_t_cmd, sw_t_open, sw_t_pre, sw_t_hdr;
static uint32_t sw_windows, sw_pre, sw_fpre, sw_ok, sw_err;
static uint32_t sw_rand = 0x1234567;
static int      mode = MODE_CONT;
static uint32_t duty_rx_us = 20000, duty_sleep_us = 30000;
static int      duty_pre_irq = 0;   // 0: DIO1 = terminal events only, 1: + preamble/header (cleared), 2: + preamble/header (not cleared, polled)
static uint32_t poll_until;         // variant 2: next SPI poll while a reception is in progress
static int      rot_arm = -1;       // rotation experiment: 0 cont, 1..3 duty variants
static uint32_t rot_period_ms, rot_next_ms;
static const char* arm_name(void) {
  if (mode == MODE_CONT) return "cont";
  if (mode == MODE_DUTY) return duty_pre_irq == 0 ? "duty0" : duty_pre_irq == 1 ? "duty1" : "duty2";
  return "swin";
}
static uint32_t tcxo_us = 5000;
static bool     print_raw = true;

// A/B experiment
static uint32_t ab_period_ms = 0;
static uint32_t ab_next_ms;

typedef struct { uint32_t ok, crc, hdr_err, pre, hdr, timeouts; uint64_t ms; } stats_t;
static stats_t st[3];
static uint32_t mode_since_ms;

static uint64_t t_pre, t_hdr;

static const uint16_t DIO_CONT = SX_IRQ_RX_DONE | SX_IRQ_CRC_ERR | SX_IRQ_HEADER_ERR | SX_IRQ_TIMEOUT
                               | SX_IRQ_PREAMBLE | SX_IRQ_HEADER_VALID;
static const uint16_t DIO_DUTY = SX_IRQ_RX_DONE | SX_IRQ_CRC_ERR | SX_IRQ_HEADER_ERR | SX_IRQ_TIMEOUT;

static void account_mode_time(void) {
  uint32_t now = sys_millis();
  st[mode].ms += now - mode_since_ms;
  mode_since_ms = now;
}

static uint32_t xr(void) { sw_rand ^= sw_rand << 13; sw_rand ^= sw_rand >> 17; sw_rand ^= sw_rand << 5; return sw_rand; }

static void sw_close(void) {
  sx_standby();
  sx_sleep(true);
  sw_state = 0;
  sw_t_pre = sw_t_hdr = 0;
  sw_next_open_ms = sys_millis() + sw_smin_ms + xr() % (sw_smax_ms - sw_smin_ms + 1);
}

static int32_t us_since(uint64_t a, uint64_t b) { return (int32_t)(((int64_t)(b - a) * 1000000) >> 15); }

static void sw_service(void) {
  uint32_t now = sys_millis();
  if (sw_state == 0) {
    if ((int32_t)(now - sw_next_open_ms) < 0) return;
    sw_t_cmd = sys_ticks();
    sx_rx_single(sw_win_us, DIO_CONT, true);
    uint64_t lim = sys_ticks() + 330;
    while (sx_busy() && sys_ticks() < lim) { }
    sw_t_open = sys_ticks();
    sw_state = 1;
    sw_windows++;
    return;
  }
  if (dio1_level()) {
    uint64_t edge = dio1_last_edge_ticks();
    uint16_t irq = sx_irq_status();
    sx_irq_clear(irq);
    if ((irq & SX_IRQ_PREAMBLE) && !sw_t_pre) { sw_t_pre = edge; sw_pre++; }
    if ((irq & SX_IRQ_HEADER_VALID) && !sw_t_hdr) sw_t_hdr = edge;
    if (irq & SX_IRQ_RX_DONE) {
      bool bad = (irq & SX_IRQ_CRC_ERR) != 0;
      uint8_t buf[256];
      int len = bad ? 0 : sx_read_packet(buf, sizeof(buf));
      sx_pkt_status_t ps; sx_packet_status(&ps);
      if (bad) sw_err++; else sw_ok++;
      con_printf("SW wake=%ld pre=%ld hdr=%ld done=%ld rssi=%d snr=%d len=%d %s ", (long)us_since(sw_t_cmd, sw_t_open),
                 sw_t_pre ? (long)us_since(sw_t_open, sw_t_pre) : -1L, sw_t_hdr ? (long)us_since(sw_t_open, sw_t_hdr) : -1L,
                 (long)us_since(sw_t_open, sys_ticks()), ps.rssi_pkt, (int)ps.snr, len, bad ? "CRC" : "OK");
      for (int i = 0; i < len && i < 24; i++) con_printf("%02X", buf[i]);
      con_write("\r\n", 2);
      sw_close();
      return;
    }
    if (irq & SX_IRQ_HEADER_ERR) { sw_err++; con_printf("SW hdrerr pre=%ld\r\n", sw_t_pre ? (long)us_since(sw_t_open, sw_t_pre) : -1L); sw_close(); return; }
    if (irq & SX_IRQ_TIMEOUT) { sw_close(); return; }
  }
  // supervision (host side timing, the chip timer is stopped on preamble)
  uint64_t t = sys_ticks();
  if (sw_t_pre && !sw_t_hdr && us_since(sw_t_pre, t) > 120000) { sw_fpre++; con_printf("SW fpre at %ld\r\n", (long)us_since(sw_t_open, sw_t_pre)); sw_close(); }
  else if (sw_t_hdr && us_since(sw_t_hdr, t) > 1500000) { con_printf("SW lost\r\n"); sw_close(); }
  else if (!sw_t_pre && us_since(sw_t_open, t) > (int32_t)sw_win_us + 20000) { sw_close(); }   // missed timeout irq
}

static void start_rx(void) {
  t_pre = t_hdr = 0;
  if (mode == MODE_SWIN) { sw_state = 0; sw_next_open_ms = sys_millis(); sx_standby(); return; }
  if (mode == MODE_CONT) sx_rx_continuous(DIO_CONT);
  else sx_rx_duty_cycle(duty_rx_us, duty_sleep_us, duty_pre_irq ? DIO_CONT : DIO_DUTY, true);
}

static void set_mode(int m) {
  account_mode_time();
  mode = m;
  start_rx();
}

static void print_stats(void) {
  account_mode_time();
  for (int m = 0; m < 2; m++) {
    stats_t* s = &st[m];
    uint32_t mins = (uint32_t)(s->ms / 60000);
    con_printf("%s: time=%lum ok=%lu crc=%lu hdrerr=%lu pre=%lu hdr=%lu to=%lu  ok/h=%lu\r\n",
               m ? "duty" : "cont", (unsigned long)mins, (unsigned long)s->ok, (unsigned long)s->crc,
               (unsigned long)s->hdr_err, (unsigned long)s->pre, (unsigned long)s->hdr, (unsigned long)s->timeouts,
               (unsigned long)(s->ms ? (uint64_t)s->ok * 3600000u / s->ms : 0));
  }
  uint64_t up = sys_ticks();
  con_printf("sleep=%lu%% wakeups=%lu dio1_edges=%lu busy_to=%lu\r\n",
             (unsigned long)(sys_sleep_ticks() * 100 / (up ? up : 1)), (unsigned long)sys_wakeups(),
             (unsigned long)dio1_edges(), (unsigned long)sx_busy_timeouts());
}

static void print_diag(void) {
  con_printf("reset=%s (0x%08lX) last_fault_pc=0x%08lX prev_boot_stage=%lu\r\n", sys_reset_reason_str(),
             (unsigned long)sys_reset_reason(), (unsigned long)sys_last_fault_pc(), (unsigned long)sys_prev_boot_stage());
  const uint32_t* d = sys_dbg(); const uint32_t* pd = sys_prev_dbg();
  con_printf("dbg:  %08lX %08lX %08lX %08lX %08lX %08lX\r\n", (unsigned long)d[0], (unsigned long)d[1], (unsigned long)d[2], (unsigned long)d[3], (unsigned long)d[4], (unsigned long)d[5]);
  con_printf("pdbg: %08lX %08lX %08lX %08lX %08lX %08lX\r\n", (unsigned long)pd[0], (unsigned long)pd[1], (unsigned long)pd[2], (unsigned long)pd[3], (unsigned long)pd[4], (unsigned long)pd[5]);
  con_printf("lfclk=%s DCDCEN=%lu DCDCEN0=%lu MAINREGSTATUS=%lu(HV=%s) REGOUT0=0x%08lX NFCPINS=0x%08lX\r\n",
             sys_lfclk_is_xtal() ? "LFXO" : "LFRC", (unsigned long)NRF_POWER->DCDCEN, (unsigned long)NRF_POWER->DCDCEN0,
             (unsigned long)NRF_POWER->MAINREGSTATUS, (NRF_POWER->MAINREGSTATUS & 1) ? "yes" : "no",
             (unsigned long)NRF_UICR->REGOUT0, (unsigned long)NRF_UICR->NFCPINS);
  con_printf("SD magic=0x%08lX fwid=0x%04lX bootloader=0x%08lX usbstat=0x%lX\r\n",
             (unsigned long)*(uint32_t*)0x3004, (unsigned long)(*(uint32_t*)0x300C & 0xFFFF),
             (unsigned long)NRF_UICR->NRFFW[0], (unsigned long)NRF_POWER->USBREGSTATUS);
  char ver[17] = {0};
  sx_read_reg(0x0320, (uint8_t*)ver, 16);
  con_printf("radio: '%s' tcxo=%s(%luus) status=0x%02X deverr=0x%04X gain=0x%02X\r\n", ver,
             sx_has_tcxo() ? "yes" : "no", (unsigned long)tcxo_us, sx_status(), sx_device_errors(),
             ({ uint8_t g; sx_read_reg(0x08AC, &g, 1); g; }));
  con_printf("cfg: %lu Hz bw=%d.%d sf=%d cr=4/%d pre=%d, mode=%s duty rx=%lu sleep=%lu\r\n",
             (unsigned long)cfg.freq_hz, (int)cfg.bw_khz, (int)(cfg.bw_khz * 10) % 10, cfg.sf, cfg.cr, cfg.preamble,
             mode ? "duty" : "cont", (unsigned long)duty_rx_us, (unsigned long)duty_sleep_us);
  // re-arm: diagnostics may have woken the radio out of duty-cycle sleep
  start_rx();
}

static void handle_radio(void) {
  if (!dio1_level()) return;
  if (poll_until && (int32_t)(sys_millis() - poll_until) < 0) return;
  poll_until = 0;
  uint64_t now = dio1_last_edge_ticks();
  uint16_t irq = sx_irq_status();
  stats_t* s = &st[mode];

  if (irq & SX_IRQ_PREAMBLE) {
    s->pre++;
    // a false preamble detection that never produced a header must not be paired with a later packet
    if (!t_pre || now - t_pre > 32768u / 4) t_pre = now;
  }
  if (irq & SX_IRQ_HEADER_VALID) { s->hdr++; if (!t_hdr) t_hdr = now; }
  if (irq & SX_IRQ_TIMEOUT) s->timeouts++;

  bool rearm = false;
  if (irq & (SX_IRQ_RX_DONE | SX_IRQ_CRC_ERR | SX_IRQ_HEADER_ERR)) {
    uint64_t t_done = sys_ticks();
    if (irq & SX_IRQ_HEADER_ERR) {
      s->hdr_err++;
      con_printf("[%lu] header error\r\n", (unsigned long)sys_millis());
    } else if (irq & SX_IRQ_CRC_ERR) {
      s->crc++;
      con_printf("[%lu] CRC error\r\n", (unsigned long)sys_millis());
    } else {
      uint8_t buf[256];
      int len = sx_read_packet(buf, sizeof(buf));
      sx_pkt_status_t ps;
      sx_packet_status(&ps);
      s->ok++;
      int32_t pre_hdr = (t_pre && t_hdr) ? (int32_t)(((t_hdr - t_pre) * 1000) >> 15) : -1;
      int32_t hdr_done = t_hdr ? (int32_t)(((t_done - t_hdr) * 1000) >> 15) : -1;
      con_printf("[%lu] RX %s len=%d rssi=%d snr=%d.%02d pre->hdr=%ldms hdr->done=%ldms toa=%lums%s",
                 (unsigned long)sys_millis(), arm_name(), len, ps.rssi_pkt, (int)ps.snr, abs((int)(ps.snr * 100)) % 100,
                 (long)pre_hdr, (long)hdr_done, (unsigned long)(sx_time_on_air_us(&cfg, len) / 1000),
                 print_raw ? " : " : "\r\n");
      if (print_raw) {
        for (int i = 0; i < len; i++) con_printf("%02X", buf[i]);
        con_write("\r\n", 2);
      }
    }
    t_pre = t_hdr = 0;
    rearm = (mode == MODE_DUTY);
  }
  if (mode == MODE_DUTY && duty_pre_irq == 2 && !(irq & (SX_IRQ_RX_DONE | SX_IRQ_CRC_ERR | SX_IRQ_HEADER_ERR | SX_IRQ_TIMEOUT))) {
    // leave preamble/header flags set (DIO1 stays high), poll again in 10 ms
    poll_until = sys_millis() + 10;
    if (t_pre && (sys_ticks() - t_pre) > 32768u / 8 && !t_hdr) { sx_irq_clear(SX_IRQ_ALL); t_pre = 0; start_rx(); poll_until = 0; }   // false preamble
    return;
  }
  sx_irq_clear(irq);
  if (rearm) start_rx();
}

static void handle_cli(void) {
  char* line = con_readline();
  if (!line) return;
  char* arg = strchr(line, ' ');
  if (arg) *arg++ = 0;
  if (!strcmp(line, "stat")) print_stats();
  else if (!strcmp(line, "diag")) print_diag();
  else if (!strcmp(line, "cont")) { ab_period_ms = 0; set_mode(MODE_CONT); con_printf("continuous RX\r\n"); }
  else if (!strcmp(line, "duty") && arg) {
    duty_rx_us = strtoul(arg, &arg, 10); duty_sleep_us = strtoul(arg, NULL, 10);
    ab_period_ms = 0; set_mode(MODE_DUTY);
    con_printf("duty RX rx=%lu sleep=%lu\r\n", (unsigned long)duty_rx_us, (unsigned long)duty_sleep_us);
  }
  else if (!strcmp(line, "ab") && arg) {
    ab_period_ms = strtoul(arg, &arg, 10) * 1000; duty_rx_us = strtoul(arg, &arg, 10); duty_sleep_us = strtoul(arg, NULL, 10);
    ab_next_ms = sys_millis() + ab_period_ms;
    memset(st, 0, sizeof(st)); mode_since_ms = sys_millis();
    set_mode(MODE_CONT);
    con_printf("A/B every %lus, duty rx=%lu sleep=%lu\r\n", (unsigned long)(ab_period_ms / 1000), (unsigned long)duty_rx_us, (unsigned long)duty_sleep_us);
  }
  else if (!strcmp(line, "swin")) {
    if (arg) { sw_win_us = strtoul(arg, &arg, 10); sw_smin_ms = strtoul(arg, &arg, 10); sw_smax_ms = strtoul(arg, NULL, 10); }
    if (sw_smax_ms <= sw_smin_ms) sw_smax_ms = sw_smin_ms + 1;
    ab_period_ms = 0; set_mode(MODE_SWIN);
    con_printf("soft sniff win=%luus sleep=%lu..%lums\r\n", (unsigned long)sw_win_us, (unsigned long)sw_smin_ms, (unsigned long)sw_smax_ms);
  }
  else if (!strcmp(line, "dutyprobe") && arg) {
    // hardware RxDutyCycle timing as seen on BUSY: low = chip awake (RX), high = sleeping/booting
    uint32_t rx = strtoul(arg, &arg, 10), sl = strtoul(arg, &arg, 10);
    bool stp = strtoul(arg, NULL, 10) != 0;
    sx_rx_duty_cycle(rx, sl, 0, stp);
    static uint32_t lo[64], hi[64];
    int nl = 0, nh = 0;
    bool level = sx_busy();
    uint64_t t_edge = sys_ticks(), end = t_edge + 32768 / 2;
    while (sys_ticks() < end && nl < 64 && nh < 64) {
      bool b = sx_busy();
      if (b != level) {
        uint64_t t = sys_ticks();
        uint32_t us = (uint32_t)(((t - t_edge) * 1000000u) >> 15);
        if (level) hi[nh++] = us; else lo[nl++] = us;
        level = b; t_edge = t;
      }
    }
    sys_wdt_feed();
    con_printf("prog rx=%lu sleep=%lu stop_pre=%d\r\nRX(low):", (unsigned long)rx, (unsigned long)sl, stp);
    for (int i = 1; i < nl && i < 12; i++) con_printf(" %lu", (unsigned long)lo[i]);
    con_printf("\r\nSLEEP(high):");
    for (int i = 1; i < nh && i < 12; i++) con_printf(" %lu", (unsigned long)hi[i]);
    con_printf("\r\n");
    sx_standby();
    start_rx();
  }
  else if (!strcmp(line, "swstat")) {
    con_printf("windows=%lu pre=%lu fpre=%lu ok=%lu err=%lu\r\n", (unsigned long)sw_windows, (unsigned long)sw_pre,
               (unsigned long)sw_fpre, (unsigned long)sw_ok, (unsigned long)sw_err);
  }
  else if (!strcmp(line, "clear")) { memset(st, 0, sizeof(st)); mode_since_ms = sys_millis(); }
  else if (!strcmp(line, "tcxo") && arg) {
    tcxo_us = strtoul(arg, NULL, 10);
    bool ok = sx_init(tcxo_us);
    sx_configure(&cfg);
    con_printf("radio init %s, tcxo=%s deverr=0x%04X\r\n", ok ? "ok" : "FAIL", sx_has_tcxo() ? "yes" : "no", sx_device_errors());
    start_rx();
  }
  else if (!strcmp(line, "wtest")) {
    // step by step: continuous RX -> warm sleep -> wake -> RX, report chip state at each step
    uint8_t st; uint16_t e;
    sx_rx_continuous(0); sys_delay_ms(5);
    st = sx_status(); e = sx_device_errors();
    con_printf("rx fresh : status=0x%02X mode=%d err=0x%04X rssi=%d %d %d\r\n", st, (st >> 4) & 7, e, sx_rssi_inst(), sx_rssi_inst(), sx_rssi_inst());
    sx_standby(); sx_clear_device_errors();
    sx_sleep(true); sys_delay_ms(50);
    con_printf("busy while asleep: %d\r\n", sx_busy());
    sx_standby();
    st = sx_status(); e = sx_device_errors();
    con_printf("woken    : status=0x%02X mode=%d err=0x%04X\r\n", st, (st >> 4) & 7, e);
    sx_rx_continuous(0); sys_delay_ms(5);
    st = sx_status(); e = sx_device_errors();
    con_printf("rx again : status=0x%02X mode=%d err=0x%04X rssi=%d %d %d\r\n", st, (st >> 4) & 7, e, sx_rssi_inst(), sx_rssi_inst(), sx_rssi_inst());
    uint8_t sw[2], gain, tcx[1];
    sx_read_reg(0x0740, sw, 2); sx_read_reg(0x08AC, &gain, 1); sx_read_reg(0x0911, tcx, 1);
    con_printf("regs: sync=%02X%02X gain=%02X xta_trim=%02X\r\n", sw[0], sw[1], gain, tcx[0]);
    sx_clear_device_errors();
    start_rx();
  }
  else if (!strcmp(line, "tcxotest")) {
    // warm-sleep -> RX cycles with decreasing TCXO delay, report XOSC start errors and wake time
    static const uint32_t delays[] = { 5000, 1000, 500, 400, 300, 250, 200 };
    for (unsigned i = 0; i < sizeof(delays) / sizeof(delays[0]); i++) {
      sx_init(delays[i]);
      sx_configure(&cfg);
      int errs = 0, stuck = 0; uint16_t last_err = 0; uint32_t wake_max = 0, wake_sum = 0;
      con_printf("tcxo %lu...\r\n", (unsigned long)delays[i]); usb_task();
      for (int k = 0; k < 20; k++) {
        sx_sleep(true);
        sys_delay_ms(20);
        uint64_t t0 = sys_ticks();
        sx_rx_continuous(0);           // wakes chip (NSS), restores context, starts TCXO + RX
        uint64_t lim = sys_ticks() + 33 * 30;
        while (sx_busy() && sys_ticks() < lim) { }
        if (sx_busy()) stuck++;
        uint32_t us = (uint32_t)(((sys_ticks() - t0) * 1000000u) >> 15);
        wake_sum += us; if (us > wake_max) wake_max = us;
        uint16_t e = sx_device_errors();
        if (e) { errs++; last_err = e; sx_clear_device_errors(); }
        sys_wdt_feed();
      }
      con_printf("tcxo delay %4luus: errors %d/20 (0x%04X) stuck %d, wake->rx avg %luus max %luus, rssi %d\r\n", (unsigned long)delays[i], errs, last_err, stuck,
                 (unsigned long)(wake_sum / 20), (unsigned long)wake_max, sx_rssi_inst());
    }
    sx_init(tcxo_us); sx_configure(&cfg); start_rx();
  }
  else if (!strcmp(line, "boost") && arg) { cfg.rx_boosted = atoi(arg); sx_standby(); sx_set_rx_boosted(cfg.rx_boosted); start_rx(); }
  else if (!strcmp(line, "dutypre") && arg) { duty_pre_irq = atoi(arg); con_printf("duty pre irq=%d\r\n", duty_pre_irq); }
  else if (!strcmp(line, "rot") && arg) {
    rot_period_ms = strtoul(arg, &arg, 10) * 1000; duty_rx_us = strtoul(arg, &arg, 10); duty_sleep_us = strtoul(arg, NULL, 10);
    ab_period_ms = 0; rot_arm = 0; rot_next_ms = sys_millis() + rot_period_ms;
    set_mode(MODE_CONT);
    con_printf("ARM %s\r\n", arm_name());
  }
  else if (!strcmp(line, "raw") && arg) print_raw = atoi(arg);
  else if (!strcmp(line, "rssi")) {
    if (mode != MODE_CONT) { con_printf("switch to cont first\r\n"); return; }
    for (int i = 0; i < 8; i++) { con_printf("%d ", sx_rssi_inst()); sys_delay_ms(5); }
    con_write("\r\n", 2);
  }
  else if (!strcmp(line, "boot")) { con_printf("-> bootloader\r\n"); sys_delay_ms(50); sys_reset_to_bootloader(); }
  else if (!strcmp(line, "reset")) sys_reset();
  else if (!strcmp(line, "softreset")) { con_printf("soft reset with WDT running\r\n"); sys_delay_ms(50); NVIC_SystemReset(); }
  else con_printf("cmds: stat diag cont | duty <rx_us> <sleep_us> | ab <secs> <rx_us> <sleep_us> | clear | tcxo <us> | boost 0/1 | raw 0/1 | rssi | boot | reset\r\n");
}

int main(void) {
  sys_init();
  sys_boot_stage(1);
  gpio_output(PIN_LED, false);
  usb_init();
  sys_boot_stage(2);
  sys_wdt_start(30);
  sys_boot_stage(3);

  bool radio_ok = sx_init(tcxo_us);
  sys_boot_stage(4);
  if (radio_ok) {
    sx_configure(&cfg);
    sys_boot_stage(5);
    dio1_init();
    start_rx();
  }
  sys_boot_stage(6);
  mode_since_ms = sys_millis();

  uint32_t next_stat = sys_millis() + 600000;
  bool was_connected = false;

  for (;;) {
    sys_clear_wake();
    sys_wdt_feed();
    usb_task();

    // host tty may still echo for a moment after opening: wait, drop input, then greet
    bool conn = usb_connected_ms() > 300;
    if (conn && !was_connected) {
      usb_discard_input();
      con_printf("\r\n=== MeshCore-LP radio lab (RX only) ===\r\n");
      if (!radio_ok) con_printf("!!! SX1262 not found\r\n");
      print_diag();
    }
    was_connected = conn;

    if (radio_ok) { if (mode == MODE_SWIN) sw_service(); else handle_radio(); }
    if (conn) handle_cli();
    sys_lfclk_maintain();

    uint32_t now = sys_millis();
    if (ab_period_ms && (int32_t)(now - ab_next_ms) >= 0) {
      ab_next_ms = now + ab_period_ms;
      set_mode(mode == MODE_CONT ? MODE_DUTY : MODE_CONT);
    }
    if (rot_arm >= 0 && (int32_t)(now - rot_next_ms) >= 0 && !poll_until && !dio1_level()
        && !(t_hdr && sys_ticks() - t_hdr < 32768u * 3 / 2)) {   // not in the middle of a packet
      rot_arm = (rot_arm + 1) % 4;
      rot_next_ms = now + rot_period_ms;
      if (rot_arm == 0) set_mode(MODE_CONT);
      else { duty_pre_irq = rot_arm - 1; sx_irq_clear(SX_IRQ_ALL); set_mode(MODE_DUTY); }
      con_printf("ARM %s\r\n", arm_name());
    }
    if ((int32_t)(now - next_stat) >= 0) {
      next_stat = now + 600000;
      print_stats();
    }

    if (radio_ok && dio1_level() && !poll_until) continue;   // IRQ still asserted: never sleep on it
    uint32_t deadline = now + 5000;
    if (poll_until) deadline = poll_until;
    if (rot_arm >= 0 && (int32_t)(rot_next_ms - deadline) < 0) deadline = rot_next_ms;
    if (mode == MODE_SWIN) deadline = (sw_state == 0) ? sw_next_open_ms : now + 2;
    if (ab_period_ms && (int32_t)(ab_next_ms - deadline) < 0) deadline = ab_next_ms;
    if (NRF_USBD->ENABLE && (int32_t)(deadline - now) > 1000) deadline = now + 1000;
    if (usb_connected() && !was_connected && (int32_t)(deadline - now) > 50) deadline = now + 50;  // USB is IRQ driven, this is only a safety net
    sys_sleep_until_ms(deadline);
  }
}
