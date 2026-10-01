// SX1262 LoRa driver. Configuration mirrors what MeshCore gets from RadioLib
// (private sync word 0x12 -> 0x1424, explicit header, CRC on, RadioLib's optimized PA table,
// datasheet ch.15 workarounds), so the node is on-air compatible with stock MeshCore.
#include "sx1262.h"
#include "board.h"
#include "gpio.h"
#include "spi.h"
#include "system.h"
#include "nrf.h"
#include <string.h>

// opcodes
#define OP_SET_SLEEP            0x84
#define OP_SET_STANDBY          0x80
#define OP_SET_TX               0x83
#define OP_SET_RX               0x82
#define OP_STOP_TIMER_ON_PREAMB 0x9F
#define OP_SET_RX_DUTY_CYCLE    0x94
#define OP_SET_CAD              0xC5
#define OP_SET_REGULATOR_MODE   0x96
#define OP_CALIBRATE            0x89
#define OP_CALIBRATE_IMAGE      0x98
#define OP_SET_PA_CONFIG        0x95
#define OP_SET_FALLBACK_MODE    0x93
#define OP_WRITE_REGISTER       0x0D
#define OP_READ_REGISTER        0x1D
#define OP_WRITE_BUFFER         0x0E
#define OP_READ_BUFFER          0x1E
#define OP_SET_DIO_IRQ_PARAMS   0x08
#define OP_GET_IRQ_STATUS       0x12
#define OP_CLEAR_IRQ_STATUS     0x02
#define OP_SET_DIO2_RF_SWITCH   0x9D
#define OP_SET_DIO3_TCXO        0x97
#define OP_SET_RF_FREQUENCY     0x86
#define OP_SET_PACKET_TYPE      0x8A
#define OP_SET_TX_PARAMS        0x8E
#define OP_SET_MODULATION       0x8B
#define OP_SET_PACKET_PARAMS    0x8C
#define OP_SET_CAD_PARAMS       0x88
#define OP_SET_BUFFER_BASE      0x8F
#define OP_GET_STATUS           0xC0
#define OP_GET_RX_BUFFER_STATUS 0x13
#define OP_GET_PACKET_STATUS    0x14
#define OP_GET_RSSI_INST        0x15
#define OP_GET_DEVICE_ERRORS    0x17
#define OP_CLEAR_DEVICE_ERRORS  0x07

// registers
#define REG_VERSION_STRING      0x0320
#define REG_RX_GAIN_RETENTION   0x029F
#define REG_IQ_CONFIG           0x0736
#define REG_SYNC_WORD_MSB       0x0740
#define REG_RANDOM_GEN          0x0819
#define REG_SENSITIVITY_CONFIG  0x0889
#define REG_RX_GAIN             0x08AC
#define REG_TX_CLAMP_CONFIG     0x08D8
#define REG_OCP                 0x08E7

static sx_config_t g_cfg;
static bool        g_tcxo;
static uint32_t    g_tcxo_delay_us;
static uint32_t    g_busy_timeouts;
static bool        g_sleeping;

// RadioLib SX1262 paOptimizedTable, index = power + 9
static const struct { uint8_t duty, hpmax; int8_t val; } pa_table[32] = {
  {2,2,-5},{2,1,0},{1,1,3},{1,2,0},{1,1,6},{1,2,3},{2,2,2},{4,1,6},{1,1,11},{2,1,11},
  {1,1,14},{2,1,14},{1,1,20},{1,1,22},{2,2,11},{3,1,21},{1,2,17},{4,2,13},{1,2,20},{1,2,22},
  {2,2,21},{3,2,21},{1,4,19},{1,4,20},{3,3,20},{2,5,19},{1,6,22},{2,5,22},{3,5,22},{3,6,22},
  {4,6,22},{4,7,22}
};

bool sx_busy(void) { return gpio_read(PIN_LORA_BUSY); }
bool sx_dio1(void) { return gpio_read(PIN_LORA_DIO1); }
uint32_t sx_busy_timeouts(void) { return g_busy_timeouts; }
bool sx_has_tcxo(void) { return g_tcxo; }
const sx_config_t* sx_config(void) { return &g_cfg; }

static bool wait_busy(uint32_t timeout_us) {
  uint64_t end = sys_ticks() + (((uint64_t)timeout_us << 15) / 1000000u) + 2;
  while (sx_busy()) {
    if (sys_ticks() > end) { g_busy_timeouts++; return false; }
  }
  return true;
}

// Wake the chip from SLEEP (also ends RxDutyCycle sleep phases): NSS falling edge.
static void wakeup(void) {
  if (!sx_busy()) return;
  gpio_clr(PIN_LORA_NSS);
  sys_delay_us(2);
  gpio_set(PIN_LORA_NSS);
  wait_busy(g_tcxo_delay_us + 5000);
}

// Make sure the chip can take a command. A chip that went to sleep on its own (RxDutyCycle)
// keeps BUSY high forever: after a normal busy period has clearly elapsed, wake it with NSS.
static void ready(void) {
  if (g_sleeping) { wakeup(); g_sleeping = false; return; }
  if (!wait_busy(20000)) {
    gpio_clr(PIN_LORA_NSS);
    sys_delay_us(2);
    gpio_set(PIN_LORA_NSS);
    wait_busy(g_tcxo_delay_us + 5000);
  }
}

static void cmd(uint8_t op, const uint8_t* params, uint16_t n) {
  static uint8_t tx[300];
  ready();
  tx[0] = op;
  if (n) memcpy(&tx[1], params, n);
  spi_begin();
  spi_transfer(tx, NULL, 1 + n);
  spi_end();
}

// command with response: opcode, then (skip) status bytes, then rx
static void cmd_read(uint8_t op, const uint8_t* hdr, uint8_t nhdr, uint8_t* out, uint8_t n) {
  static uint8_t tx[300], rx[300];
  ready();
  memset(tx, 0, 1 + nhdr + 1 + n);
  tx[0] = op;
  if (nhdr) memcpy(&tx[1], hdr, nhdr);
  spi_begin();
  spi_transfer(tx, rx, 1 + nhdr + 1 + n);
  spi_end();
  memcpy(out, &rx[1 + nhdr + 1], n);
}

void sx_write_reg(uint16_t addr, const uint8_t* data, uint8_t n) {
  uint8_t p[34];
  p[0] = addr >> 8; p[1] = addr & 0xFF;
  memcpy(&p[2], data, n);
  cmd(OP_WRITE_REGISTER, p, 2 + n);
}

void sx_read_reg(uint16_t addr, uint8_t* data, uint8_t n) {
  uint8_t h[2] = { addr >> 8, addr & 0xFF };
  cmd_read(OP_READ_REGISTER, h, 2, data, n);
}

static void write_reg1(uint16_t addr, uint8_t v) { sx_write_reg(addr, &v, 1); }
static uint8_t read_reg1(uint16_t addr) { uint8_t v; sx_read_reg(addr, &v, 1); return v; }

uint8_t sx_status(void) {
  static uint8_t tx[1] = { OP_GET_STATUS }, rx[1];
  ready();
  spi_begin();
  tx[0] = OP_GET_STATUS;
  spi_transfer(tx, rx, 1);
  spi_end();
  return rx[0];
}

uint16_t sx_device_errors(void) {
  uint8_t r[2];
  cmd_read(OP_GET_DEVICE_ERRORS, NULL, 0, r, 2);
  return ((uint16_t)r[0] << 8) | r[1];
}

void sx_clear_device_errors(void) {
  uint8_t p[2] = { 0, 0 };
  cmd(OP_CLEAR_DEVICE_ERRORS, p, 2);
}

uint16_t sx_irq_status(void) {
  uint8_t r[2];
  cmd_read(OP_GET_IRQ_STATUS, NULL, 0, r, 2);
  return ((uint16_t)r[0] << 8) | r[1];
}

bool sx_peek_irq(uint16_t* irq, uint8_t* status) {
  static uint8_t tx[4] = { OP_GET_IRQ_STATUS, 0, 0, 0 }, rx[4];
  if (sx_busy()) return false;          // asleep (or busy): a NSS edge would wake it, do not touch
  tx[0] = OP_GET_IRQ_STATUS;
  spi_begin();
  spi_transfer(tx, rx, 4);
  spi_end();
  *status = rx[1];
  *irq = ((uint16_t)rx[2] << 8) | rx[3];
  return true;
}

void sx_irq_clear(uint16_t mask) {
  uint8_t p[2] = { mask >> 8, mask & 0xFF };
  cmd(OP_CLEAR_IRQ_STATUS, p, 2);
}

static void set_dio_irq(uint16_t irq_mask, uint16_t dio1_mask) {
  uint8_t p[8] = { irq_mask >> 8, irq_mask & 0xFF, dio1_mask >> 8, dio1_mask & 0xFF, 0, 0, 0, 0 };
  cmd(OP_SET_DIO_IRQ_PARAMS, p, 8);
}

void sx_standby(void) {
  uint8_t p = 0x00;   // STDBY_RC
  cmd(OP_SET_STANDBY, &p, 1);
}

void sx_sleep(bool warm) {
  uint8_t p = warm ? 0x04 : 0x00;
  cmd(OP_SET_SLEEP, &p, 1);
  g_sleeping = true;
  sys_delay_us(600);   // datasheet: no SPI for ~500us after SetSleep
}

static uint8_t bw_code(float bw) {
  if (bw < 9.0f)   return 0x00;   // 7.8
  if (bw < 12.0f)  return 0x08;   // 10.4
  if (bw < 18.0f)  return 0x01;   // 15.6
  if (bw < 26.0f)  return 0x09;   // 20.8
  if (bw < 36.0f)  return 0x02;   // 31.25
  if (bw < 50.0f)  return 0x0A;   // 41.7
  if (bw < 90.0f)  return 0x03;   // 62.5
  if (bw < 180.0f) return 0x04;   // 125
  if (bw < 350.0f) return 0x05;   // 250
  return 0x06;                    // 500
}

uint32_t sx_symbol_us(const sx_config_t* c) {
  return ((uint32_t)(1000 * 10) << c->sf) / (uint32_t)(c->bw_khz * 10);
}

static bool ldro(const sx_config_t* c) { return sx_symbol_us(c) >= 16000; }

uint32_t sx_time_on_air_us(const sx_config_t* c, int len) {
  // identical to RadioLib SX126x::calculateTimeOnAir() (explicit header, CRC on)
  uint32_t sym = sx_symbol_us(c);
  uint8_t sf1_x4 = 17, sf2 = 8;
  if (c->sf == 5 || c->sf == 6) { sf1_x4 = 25; sf2 = 0; }
  uint8_t div = 4 * c->sf;
  if (ldro(c)) div = 4 * (c->sf - 2);
  int16_t bits = (int16_t)(8 * len) + 16 - 4 * c->sf + sf2 + 20;
  if (bits < 0) bits = 0;
  uint16_t nsym = (bits + div - 1) / div;
  uint32_t n_x4 = (c->preamble + 8) * 4 + sf1_x4 + nsym * c->cr * 4;
  return (sym * n_x4) / 4;
}

static bool g_iq_inverted = false;   // only for CAD false-alarm probes, never for RX/TX

static void set_packet_params(uint8_t payload_len) {
  uint8_t p[6] = { g_cfg.preamble >> 8, g_cfg.preamble & 0xFF, 0x00 /*explicit*/, payload_len, 0x01 /*CRC on*/,
                   g_iq_inverted ? 0x01 : 0x00 };
  cmd(OP_SET_PACKET_PARAMS, p, 6);
}

void sx_set_iq_inverted(bool inv) {
  sx_standby();
  g_iq_inverted = inv;
  set_packet_params(0xFF);
  // datasheet 15.4: IQ polarity fix, bit2 of 0x0736 = 1 for standard IQ, 0 for inverted
  uint8_t r = read_reg1(REG_IQ_CONFIG);
  write_reg1(REG_IQ_CONFIG, inv ? (r & ~0x04) : (r | 0x04));
}

void sx_set_tx_power(int8_t dbm) {
  if (dbm < -9) dbm = -9;
  if (dbm > 22) dbm = 22;
  g_cfg.tx_power = dbm;
  uint8_t ocp = read_reg1(REG_OCP);
  uint8_t pa[4] = { pa_table[dbm + 9].duty, pa_table[dbm + 9].hpmax, 0x00 /*SX1262*/, 0x01 };
  cmd(OP_SET_PA_CONFIG, pa, 4);
  uint8_t tp[2] = { (uint8_t)pa_table[dbm + 9].val, 0x04 /*200us ramp*/ };
  cmd(OP_SET_TX_PARAMS, tp, 2);
  write_reg1(REG_OCP, ocp);
}

void sx_set_rx_boosted(bool on) {
  g_cfg.rx_boosted = on;
  write_reg1(REG_RX_GAIN, on ? 0x96 : 0x94);
  // keep RX gain across warm sleep (mandatory for RxDutyCycle), datasheet 9.6
  uint8_t ret[3] = { 0x01, REG_RX_GAIN >> 8, REG_RX_GAIN & 0xFF };
  sx_write_reg(REG_RX_GAIN_RETENTION, ret, 3);
}

void sx_configure(const sx_config_t* c) {
  g_cfg = *c;
  sx_standby();

  // frequency + image calibration for the band
  uint8_t img[2] = { 0xD7, 0xDB };                 // 863-870 MHz
  uint32_t mhz = c->freq_hz / 1000000u;
  if (mhz >= 902 && mhz <= 928)      { img[0] = 0xE1; img[1] = 0xE9; }
  else if (mhz >= 779 && mhz <= 787) { img[0] = 0xC1; img[1] = 0xC5; }
  else if (mhz >= 470 && mhz <= 510) { img[0] = 0x75; img[1] = 0x81; }
  else if (mhz >= 430 && mhz <= 440) { img[0] = 0x6B; img[1] = 0x6F; }
  cmd(OP_CALIBRATE_IMAGE, img, 2);

  uint32_t frf = (uint32_t)(((uint64_t)c->freq_hz << 25) / 32000000u);
  uint8_t f[4] = { frf >> 24, frf >> 16, frf >> 8, frf };
  cmd(OP_SET_RF_FREQUENCY, f, 4);

  uint8_t m[4] = { c->sf, bw_code(c->bw_khz), (uint8_t)(c->cr - 4), ldro(c) ? 1 : 0 };
  cmd(OP_SET_MODULATION, m, 4);
  set_packet_params(0xFF);

  // datasheet 15.4: IQ polarity fix (standard IQ -> bit2 set)
  g_iq_inverted = false;
  set_packet_params(0xFF);
  write_reg1(REG_IQ_CONFIG, read_reg1(REG_IQ_CONFIG) | 0x04);
  // private sync word 0x12 (RadioLib control bits 0x44)
  uint8_t sw[2] = { 0x14, 0x24 };
  sx_write_reg(REG_SYNC_WORD_MSB, sw, 2);

  // datasheet 15.2: PA clamping
  write_reg1(REG_TX_CLAMP_CONFIG, read_reg1(REG_TX_CLAMP_CONFIG) | 0x1E);
  sx_set_tx_power(c->tx_power);
  write_reg1(REG_OCP, 0x38);                         // 140 mA
  sx_set_rx_boosted(c->rx_boosted);

  // datasheet 15.1: modulation quality, bit2 of 0x0889 = 0 only for BW500
  uint8_t sens = read_reg1(REG_SENSITIVITY_CONFIG);
  sens = (c->bw_khz > 400.0f) ? (sens & 0xFB) : (sens | 0x04);
  write_reg1(REG_SENSITIVITY_CONFIG, sens);

  sx_irq_clear(SX_IRQ_ALL);
}

static void chip_setup(void) {
  uint8_t reg = 0x01;  // DC-DC
  cmd(OP_SET_REGULATOR_MODE, &reg, 1);
  uint8_t dio2 = 0x01; // DIO2 drives the RF switch (HT-RA62)
  cmd(OP_SET_DIO2_RF_SWITCH, &dio2, 1);
  uint8_t base[2] = { 0, 0 };
  cmd(OP_SET_BUFFER_BASE, base, 2);
}

bool sx_init(uint32_t tcxo_delay_us) {
  gpio_output(PIN_VCC_EN, true);
  gpio_output(PIN_LORA_RXEN, true);
  gpio_input(PIN_LORA_BUSY, GPIO_PIN_CNF_PULL_Disabled);
  gpio_input(PIN_LORA_DIO1, GPIO_PIN_CNF_PULL_Disabled);
  spi_init();
  g_sleeping = false;

  // hardware reset
  gpio_output(PIN_LORA_RESET, true);
  sys_delay_ms(10);            // module power settle after VCC enable
  gpio_clr(PIN_LORA_RESET);
  sys_delay_us(200);
  gpio_set(PIN_LORA_RESET);
  sys_delay_ms(5);
  wait_busy(20000);

  char ver[17] = { 0 };
  sx_read_reg(REG_VERSION_STRING, (uint8_t*)ver, 16);
  if (strncmp(ver, "SX126", 5) != 0) return false;

  sx_standby();
  // TCXO on DIO3; if the oscillator does not start, fall back to crystal (RA-01SH style module)
  g_tcxo_delay_us = tcxo_delay_us;
  uint32_t d = (uint32_t)(tcxo_delay_us / 15.625f);
  uint8_t t[4] = { SX126X_TCXO_VOLTAGE_CODE, d >> 16, d >> 8, d };
  cmd(OP_SET_DIO3_TCXO, t, 4);
  sx_clear_device_errors();

  uint8_t pt = 0x01;   // LoRa
  cmd(OP_SET_PACKET_TYPE, &pt, 1);
  uint8_t fb = 0x20;   // fallback STDBY_RC
  cmd(OP_SET_FALLBACK_MODE, &fb, 1);
  set_dio_irq(0, 0);
  sx_irq_clear(SX_IRQ_ALL);

  uint8_t cal = 0x7F;
  cmd(OP_CALIBRATE, &cal, 1);
  sys_delay_ms(5);
  wait_busy(50000);

  g_tcxo = true;
  if (sx_device_errors() & SX_ERR_XOSC_START) {
    // no TCXO: reset and run on XTAL
    g_tcxo = false;
    g_tcxo_delay_us = 0;
    gpio_clr(PIN_LORA_RESET); sys_delay_us(200); gpio_set(PIN_LORA_RESET);
    sys_delay_ms(5); wait_busy(20000);
    sx_standby();
    cmd(OP_SET_PACKET_TYPE, &pt, 1);
    cmd(OP_SET_FALLBACK_MODE, &fb, 1);
    cmd(OP_CALIBRATE, &cal, 1);
    sys_delay_ms(5); wait_busy(50000);
    sx_clear_device_errors();
  }

  chip_setup();
  return true;
}

// Full receiver reset (as stock MeshCore sx126xResetAGC): warm sleep, Calibrate(0x7F) of all blocks,
// then everything calibration may touch is set again. Caller re-applies the radio config (sx_configure).
void sx_recalibrate(void) {
  sx_standby();
  sx_sleep(true);
  sx_standby();
  uint8_t cal = 0x7F;
  cmd(OP_CALIBRATE, &cal, 1);
  sys_delay_ms(5);
  wait_busy(50000);
  sx_clear_device_errors();   // XOSC_START_ERR after a warm start is expected (TCXO), see sx_init
  uint8_t pt = 0x01;
  cmd(OP_SET_PACKET_TYPE, &pt, 1);
  uint8_t fb = 0x20;
  cmd(OP_SET_FALLBACK_MODE, &fb, 1);
  chip_setup();
  g_iq_inverted = false;
}

void sx_rx_continuous(uint16_t irq_mask_dio1) {
  sx_standby();
  gpio_set(PIN_LORA_RXEN);
  uint8_t stp = 0x00;
  cmd(OP_STOP_TIMER_ON_PREAMB, &stp, 1);
  set_packet_params(0xFF);
  sx_irq_clear(SX_IRQ_ALL);
  set_dio_irq(SX_IRQ_ALL, irq_mask_dio1);
  uint8_t t[3] = { 0xFF, 0xFF, 0xFF };   // continuous
  cmd(OP_SET_RX, t, 3);
}

void sx_rx_single(uint32_t timeout_us, uint16_t irq_mask_dio1, bool stop_on_preamble) {
  sx_standby();
  gpio_set(PIN_LORA_RXEN);
  uint8_t stp = stop_on_preamble ? 0x01 : 0x00;
  cmd(OP_STOP_TIMER_ON_PREAMB, &stp, 1);
  set_packet_params(0xFF);
  sx_irq_clear(SX_IRQ_ALL);
  set_dio_irq(SX_IRQ_ALL, irq_mask_dio1);
  uint32_t t = (uint32_t)(timeout_us / 15.625f);
  if (t == 0) t = 1;
  uint8_t p[3] = { t >> 16, t >> 8, t };
  cmd(OP_SET_RX, p, 3);
}

void sx_rx_duty_cycle(uint32_t rx_us, uint32_t sleep_us, uint16_t irq_mask_dio1, bool stop_on_preamble) {
  sx_standby();
  gpio_set(PIN_LORA_RXEN);
  set_packet_params(0xFF);
  sx_irq_clear(SX_IRQ_ALL);
  set_dio_irq(SX_IRQ_ALL, irq_mask_dio1);
  // 1: stop the window timer as soon as a preamble is seen (the host supervises false detections)
  uint8_t stp = stop_on_preamble ? 0x01 : 0x00;
  cmd(OP_STOP_TIMER_ON_PREAMB, &stp, 1);
  uint32_t r = (uint32_t)(rx_us / 15.625f), s = (uint32_t)(sleep_us / 15.625f);
  if (r == 0) r = 1;
  if (s == 0) s = 1;
  uint8_t p[6] = { r >> 16, r >> 8, r, s >> 16, s >> 8, s };
  cmd(OP_SET_RX_DUTY_CYCLE, p, 6);
  // the chip goes to sleep by itself between windows: SPI access needs an NSS wakeup
  g_sleeping = true;
}

void sx_cad(uint8_t symbols, uint8_t det_peak, uint8_t det_min, bool exit_to_rx, uint32_t rx_timeout_us, uint16_t irq_mask_dio1) {
  sx_standby();
  gpio_set(PIN_LORA_RXEN);
  set_packet_params(0xFF);
  sx_irq_clear(SX_IRQ_ALL);
  set_dio_irq(SX_IRQ_ALL, irq_mask_dio1);
  uint8_t code = 0;
  if (symbols >= 16) code = 4; else if (symbols >= 8) code = 3; else if (symbols >= 4) code = 2; else if (symbols >= 2) code = 1;
  uint32_t to = (uint32_t)(rx_timeout_us / 15.625f);
  uint8_t p[7] = { code, det_peak, det_min, exit_to_rx ? 1 : 0, to >> 16, to >> 8, to };
  cmd(OP_SET_CAD_PARAMS, p, 7);
  cmd(OP_SET_CAD, NULL, 0);
}

bool sx_tx(const uint8_t* data, uint8_t len) {
  static uint8_t buf[260];
  sx_standby();
  set_packet_params(len);
  // datasheet ch.15 fixes + OCP: re-applied before each TX, registers are not all kept over warm sleep
  uint8_t sens = read_reg1(REG_SENSITIVITY_CONFIG);
  sens = (g_cfg.bw_khz > 400.0f) ? (sens & 0xFB) : (sens | 0x04);
  write_reg1(REG_SENSITIVITY_CONFIG, sens);
  write_reg1(REG_TX_CLAMP_CONFIG, read_reg1(REG_TX_CLAMP_CONFIG) | 0x1E);
  write_reg1(REG_OCP, 0x38);

  buf[0] = 0x00;   // buffer offset
  memcpy(&buf[1], data, len);
  cmd(OP_WRITE_BUFFER, buf, 1 + len);
  sx_irq_clear(SX_IRQ_ALL);
  set_dio_irq(SX_IRQ_TX_DONE | SX_IRQ_TIMEOUT, SX_IRQ_TX_DONE | SX_IRQ_TIMEOUT);
  uint8_t t[3] = { 0, 0, 0 };   // no timeout, we supervise from the MCU
  gpio_clr(PIN_LORA_RXEN);      // RF switch: receive branch off while transmitting
  cmd(OP_SET_TX, t, 3);
  return true;
}

int sx_read_packet(uint8_t* out, int max_len) {
  uint8_t st[2];
  cmd_read(OP_GET_RX_BUFFER_STATUS, NULL, 0, st, 2);
  int len = st[0];
  if (len > max_len) len = max_len;
  if (len <= 0) return 0;
  static uint8_t tmp[256];
  uint8_t off = st[1];
  cmd_read(OP_READ_BUFFER, &off, 1, tmp, (uint8_t)len);
  memcpy(out, tmp, len);
  return len;
}

void sx_packet_status(sx_pkt_status_t* ps) {
  uint8_t r[3];
  cmd_read(OP_GET_PACKET_STATUS, NULL, 0, r, 3);
  ps->rssi_pkt = -(int16_t)r[0] / 2;
  ps->snr = (float)(int8_t)r[1] / 4.0f;
  ps->signal_rssi = -(int16_t)r[2] / 2;
}

int16_t sx_rssi_inst(void) {
  uint8_t r;
  cmd_read(OP_GET_RSSI_INST, NULL, 0, &r, 1);
  return -(int16_t)r / 2;
}

uint32_t sx_random32(void) {
  uint32_t v;
  sx_read_reg(REG_RANDOM_GEN, (uint8_t*)&v, 4);
  return v;
}
