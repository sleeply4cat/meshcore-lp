#include "LoRaRadio.h"
#include "hal/dio1.h"
#include "hal/rng.h"
#include "hal/system.h"
#include <Arduino.h>

LoRaRadio radio_driver;

// Only end-of-packet events go to DIO1. PreambleDetected / HeaderValid are latched in the IRQ
// register and only *read* (never cleared mid-packet): clearing them while a packet is being
// received measurably costs packets, in continuous RX and even more in RxDutyCycle.
#define DIO1_RX_MASK  (SX_IRQ_RX_DONE | SX_IRQ_CRC_ERR | SX_IRQ_HEADER_ERR | SX_IRQ_TIMEOUT)
#define PEEK_PERIOD_MS  250   // sniff: look for a receiver stuck on a false preamble

#define NOISE_FLOOR_PERIOD_MS        60000   // plain stats
#define NOISE_FLOOR_PERIOD_LBT_MS    15000   // when the RSSI interference threshold is in use
#define HEALTH_PERIOD_MS            600000

// CAD threshold auto-tuning. A CAD with inverted IQ does not respond to MeshCore's (standard IQ)
// chirps, but sees noise and interference exactly like the real one: its hit rate is the false alarm
// rate of the current detPeak. Bursts of probes every ~minute (a few uA on average); every
// CAD_WINDOW probes the offset from SF+13 moves by one step within +-CAD_ADJ_MAX:
// > 2 % false alarms -> less sensitive, none at all -> more sensitive.
#define CAD_SYMBOLS          2
#define CAD_DET_MIN          10
#define CAD_PROBES_INV       4        // per burst
#define CAD_PROBE_MIN_MS     45000    // burst period 45..75 s (random, not in step with the network)
#define CAD_PROBE_SPAN_MS    30000
#define CAD_WINDOW           120      // inverted probes per tuning decision (~30 min)
#define CAD_FAST_RAISE       6        // this many false alarms before the window ends -> step up now
#define CAD_HOT_MS           3000     // probe counts as 'hot' this soon after a reception
#define CAD_MAX_DEFER        3        // CAD may hold one TX at most this many times (x 200 ms retry)

static inline bool expired(uint32_t now, uint32_t since, uint32_t timeout) {
  return (int32_t)(now - since) > (int32_t)timeout;   // signed: 'since' may be a hair ahead of 'now'
}

static inline uint32_t stamp(uint32_t now) { return now ? now : 1; }   // 0 means "no event"

// ---------------------------------------------------------------- setup
bool LoRaRadio::init(const LpConfig& lp) {
  _lp = lp;
  if (!sx_init(_lp.tcxo_us)) return false;
  dio1_init();
  _cfg.freq_hz = (uint32_t)(LORA_FREQ * 1000000.0);
  _cfg.bw_khz = LORA_BW;
  _cfg.sf = LORA_SF;
  _cfg.cr = LORA_CR;
  _cfg.preamble = _cfg.sf <= 8 ? 32 : 16;       // MeshCore RadioLibWrapper::preambleLengthForSF()
  _cfg.tx_power = LORA_TX_POWER;
  _cfg.rx_boosted = true;
  sx_configure(&_cfg);
  _cad_adj = _lp.cad_auto ? 0 : _lp.cad_adj;
  computeTiming();
  return true;
}

void LoRaRadio::setLpConfig(const LpConfig& lp) {
  bool tcxo_changed = lp.tcxo_us != _lp.tcxo_us;
  if (lp.recal_min != _lp.recal_min) _next_recal_ms = millis() + (uint32_t)lp.recal_min * 60000u;
  _lp = lp;
  if (_lp.cad_adj > CAD_ADJ_MAX) _lp.cad_adj = CAD_ADJ_MAX;
  if (_lp.cad_adj < -CAD_ADJ_MAX) _lp.cad_adj = -CAD_ADJ_MAX;
  if (!_lp.cad_auto) _cad_adj = _lp.cad_adj;
  if (tcxo_changed) {
    sx_init(_lp.tcxo_us);
    sx_configure(&_cfg);
  }
  computeTiming();
  startRx();
}

void LoRaRadio::computeTiming() {
  uint32_t tsym = sx_symbol_us(&_cfg);
  uint32_t pre = _lp.sniff_preamble ? _lp.sniff_preamble : (_cfg.sf <= 8 ? 32 : 16);
  uint32_t det = _lp.sniff_detect ? _lp.sniff_detect : 7;
  uint32_t overhead = (sx_has_tcxo() ? _lp.tcxo_us : 0) + 1000;   // TCXO start + context restore/PLL

  // Measured on HT-RA62 (lab 'swin'): PreambleDetected comes ~3.6 symbols after a window opens on a
  // preamble already on air (90th percentile ~6, AGC/PLL settling included), ~3.5 symbols after a
  // preamble starts inside an open window. With window W = det symbols (det = cold detection budget):
  //   a preamble starting too late in window k (after W - 3.5) must still have >= det symbols left
  //   when window k+1 opens:   sleep + overhead <= (pre - det - 3.5) * Tsym
  int32_t sleep_us = (int32_t)(((2 * pre - 2 * det - 7) * tsym) / 2) - (int32_t)overhead;
  if (pre <= det + 4 || sleep_us < (int32_t)(tsym / 2)) {
    _sniff_rx_us = _sniff_sleep_us = 0;   // no room for sleeping: sniff degenerates to continuous
  } else {
    _sniff_rx_us = det * tsym;
    _sniff_sleep_us = sleep_us;
  }

  // supervision: preamble detect -> header valid, header -> done (max packet, CR 4/8).
  // Must cover the LONGEST preamble in use (MeshCore sends 32 at SF<=8), not the shortest we guarantee.
  uint32_t pre_max = pre > 32 ? pre : 32;
  _pre_timeout_ms = ((pre_max + 16) * tsym) / 1000 + 5;
  sx_config_t c = _cfg;
  c.cr = 8; c.preamble = 0;
  _payload_timeout_ms = sx_time_on_air_us(&c, 255) / 1000 + 50;
}

void LoRaRadio::begin() {
  _noise_floor = 0;
  _threshold = 0;
  _cad_enabled = false;
  _next_floor_ms = millis() + 5000;
  _next_health_ms = millis() + HEALTH_PERIOD_MS;
  _next_cad_probe_ms = millis() + 30000;
  _next_recal_ms = millis() + (uint32_t)_lp.recal_min * 60000u;
  startRx();
}

void LoRaRadio::startRx() {
  _preamble_at = _header_at = 0;
  _next_peek_ms = millis() + PEEK_PERIOD_MS;
  if (_lp.rx_mode == RX_SNIFF && _sniff_sleep_us) {
    sx_set_rx_boosted(_cfg.rx_boosted);   // (re)arm RX gain retention for warm starts
    sx_rx_duty_cycle(_sniff_rx_us, _sniff_sleep_us, DIO1_RX_MASK, true);
    _sniff_active = true;
  } else {
    sx_rx_continuous(DIO1_RX_MASK);
    _sniff_active = false;
  }
  _state = ST_RX;
  _st.rearms++;
}

// ---------------------------------------------------------------- params
void LoRaRadio::setParams(float freq, float bw, uint8_t sf, uint8_t cr) {
  _cfg.freq_hz = (uint32_t)(freq * 1000000.0f + 0.5f);
  _cfg.bw_khz = bw;
  _cfg.sf = sf;
  _cfg.cr = cr;
  _cfg.preamble = sf <= 8 ? 32 : 16;
  sx_configure(&_cfg);
  computeTiming();
  if (_state != ST_TX) startRx();
}

void LoRaRadio::setTxPower(int8_t dbm) {
  sx_set_tx_power(dbm);
  _cfg.tx_power = sx_config()->tx_power;
  if (_state == ST_RX) startRx();
}

bool LoRaRadio::setRxBoostedGainMode(bool en) {
  _cfg.rx_boosted = en;
  sx_standby();
  sx_set_rx_boosted(en);
  if (_state != ST_TX) startRx();
  return true;
}

uint32_t LoRaRadio::getRngSeed() { return rng_u32(); }

void LoRaRadio::powerOff() {
  sx_standby();
  sx_sleep(false);
  _state = ST_IDLE;
}

// ---------------------------------------------------------------- events
bool LoRaRadio::irqPending() const { return sx_dio1(); }

void LoRaRadio::service() {
  if (!sx_dio1()) return;
  uint16_t irq = sx_irq_status();
  uint32_t now = millis();

  if (_state == ST_TX) {
    sx_irq_clear(irq);
    if (irq & (SX_IRQ_TX_DONE | SX_IRQ_TIMEOUT)) _state = ST_TX_DONE;
    return;
  }
  if (_state != ST_RX) { sx_irq_clear(irq); return; }

  if (_debug > 1 && Serial) Serial.printf("[lp] irq 0x%04X at %lu\r\n", irq, (unsigned long)now);
  if (irq & (SX_IRQ_RX_DONE | SX_IRQ_HEADER_ERR)) _last_rx_ms = stamp(now);
  if (irq & SX_IRQ_RX_DONE) {
    if ((irq & SX_IRQ_CRC_ERR) || ((irq & SX_IRQ_HEADER_ERR) && !(irq & SX_IRQ_HEADER_VALID))) {
      n_recv_errors++;
      _st.rx_crc++;
      if (_exp_period_ms) _exp_err[_exp_arm]++;
    } else {
      if (_rx_len > 0) _st.rx_overrun++;   // previous packet never collected (should not happen)
      _rx_len = sx_read_packet(_rx_buf, sizeof(_rx_buf));
      sx_pkt_status_t ps;
      sx_packet_status(&ps);
      _last_rssi = ps.rssi_pkt;
      _last_snr = ps.snr;
      if (_rx_len > 0) { n_recv++; _st.rx_ok++; if (_exp_period_ms) _exp_ok[_exp_arm]++; }
    }
  } else if (irq & SX_IRQ_HEADER_ERR) {
    n_recv_errors++;
    _st.rx_hdr_err++;
  }
  // every DIO1 event is terminal: release all latched flags, continuous RX keeps listening,
  // RxDutyCycle has dropped to STDBY_RC and must be re-armed
  sx_irq_clear(SX_IRQ_ALL);
  _preamble_at = _header_at = 0;
  if (_sniff_active || _lp.rx_mode == RX_SNIFF || _lp.cont_rearm) startRx();
}

// Non-intrusive look at the latched PreambleDetected/HeaderValid flags. Never wakes a sleeping
// (sniffing) chip. Keeps _preamble_at/_header_at as "first seen" times for LBT and supervision.
void LoRaRadio::peek() {
  uint16_t irq;
  uint8_t st;
  if (_state != ST_RX || !sx_peek_irq(&irq, &st)) return;
  uint32_t now = millis();
  uint8_t chip_mode = (st >> 4) & 7;
  if (chip_mode != 5) {             // not in RX: e.g. we raced a window closing and our NSS woke it
    if (_sniff_active || _lp.rx_mode == RX_SNIFF) { _st.peek_rearm++; startRx(); }
    return;
  }
  if ((irq & SX_IRQ_PREAMBLE) && !_preamble_at) _preamble_at = stamp(now);
  if ((irq & SX_IRQ_HEADER_VALID) && !_header_at) {
    _header_at = stamp(now);
    if (_debug > 1 && Serial && _preamble_at) Serial.printf("[lp] header seen %lums after preamble\r\n", (unsigned long)(now - _preamble_at));
  }
  if (!(irq & (SX_IRQ_PREAMBLE | SX_IRQ_HEADER_VALID))) _preamble_at = _header_at = 0;
}

int LoRaRadio::recvRaw(uint8_t* bytes, int sz) {
  service();
  if (_state == ST_IDLE) startRx();
  if (_rx_len <= 0) return 0;
  int len = _rx_len < sz ? _rx_len : sz;
  memcpy(bytes, _rx_buf, len);
  _rx_len = 0;
  return len;
}

void LoRaRadio::loop() {
  service();
  uint32_t now = millis();

  if (_state == ST_IDLE) { startRx(); return; }
  if (_state != ST_RX) return;

  if (_exp_period_ms && (long)(now - _exp_next_ms) >= 0 && !_preamble_at && !_header_at) {
    expAccount();
    expApplyArm((_exp_arm + 1) % EXP_ARMS);
    _exp_next_ms = now + _exp_period_ms;
    return;
  }

  // supervision of partial receptions (flags are only peeked, see DIO1_RX_MASK).
  // Always look at the live flags before deciding anything: a verdict taken on flags read up to
  // PEEK_PERIOD_MS ago ('preamble seen, no header yet') killed receptions whose header had arrived
  // in the meantime - about a third of the packets that happened to be peeked between the two.
  if (_sniff_active && (long)(now - _next_peek_ms) >= 0) _next_peek_ms = now + PEEK_PERIOD_MS;
  peek();
  if (_state != ST_RX) return;
  if (_header_at && expired(now, _header_at, _payload_timeout_ms)) {
    _st.rx_lost_hdr++;
    if (_debug && Serial) Serial.printf("[lp] header without end (%s)\r\n", _sniff_active ? "sniff" : "cont");
    sx_irq_clear(SX_IRQ_ALL);
    startRx();
    return;
  }
  if (_preamble_at && !_header_at && expired(now, _preamble_at, _pre_timeout_ms)) {
    _st.rx_false_pre++;
    if (_debug > 1 && Serial) {
      uint16_t irq = 0; uint8_t st = 0;
      sx_peek_irq(&irq, &st);
      Serial.printf("[lp] false preamble (%s) after %lums, irq 0x%04X status 0x%02X\r\n", _sniff_active ? "sniff" : "cont",
                    (unsigned long)(now - _preamble_at), irq, st);
    }
    sx_irq_clear(SX_IRQ_PREAMBLE);
    _preamble_at = 0;
    if (_sniff_active) startRx();    // timer was stopped on the preamble: release the receiver
    return;
  }
  bool busy = _preamble_at || _header_at;

  // continuous RX was only borrowed (noise floor / LBT): go back to sniffing
  if (!busy && _lp.rx_mode == RX_SNIFF && _sniff_sleep_us && !_sniff_active) { startRx(); return; }

  if (!busy && (long)(now - _next_floor_ms) >= 0) {
    sampleNoiseFloor();
    _next_floor_ms = now + (_threshold ? NOISE_FLOOR_PERIOD_LBT_MS : NOISE_FLOOR_PERIOD_MS);
  }
  if (!busy && (long)(now - _next_health_ms) >= 0) {
    _next_health_ms = now + HEALTH_PERIOD_MS;
    healthCheck();
    return;
  }
  if (!busy && _lp.recal_min && (long)(now - _next_recal_ms) >= 0) {
    _next_recal_ms = now + (uint32_t)_lp.recal_min * 60000u;
    recalibrate();
    return;
  }
  if (!busy && _cad_enabled && (long)(now - _next_cad_probe_ms) >= 0) cadProbe();
}

unsigned long LoRaRadio::nextDeadline(unsigned long max_ms) {
  uint32_t now = millis();
  uint32_t wake = now + max_ms;
  auto earlier = [&](uint32_t t) { if ((long)(t - wake) < 0) wake = t; };
  if (_state == ST_IDLE) return now;
  if (_state == ST_RX) {
    if (_header_at) earlier(_header_at + _payload_timeout_ms + 1);
    else if (_preamble_at) earlier(_preamble_at + _pre_timeout_ms + 1);
    else {
      earlier(_next_floor_ms);
      earlier(_next_health_ms);
      if (_lp.recal_min) earlier(_next_recal_ms);
      if (_cad_enabled) earlier(_next_cad_probe_ms);
      if (_exp_period_ms) earlier(_exp_next_ms);
    }
    if (_sniff_active) earlier(_next_peek_ms);
  }
  if ((long)(wake - now) < 0) wake = now;
  return wake;
}

// ---------------------------------------------------------------- maintenance
void LoRaRadio::sampleNoiseFloor() {
  if (_sniff_active) {
    sx_rx_continuous(DIO1_RX_MASK);
    _sniff_active = false;
    sys_delay_us(1500);   // PLL + AGC settle
  }
  int32_t sum = 0;
  int n = 0;
  for (int i = 0; i < 16 && !sx_dio1(); i++) {
    int rssi = sx_rssi_inst();
    if (_noise_floor == 0 || rssi < _noise_floor + 14) { sum += rssi; n++; }   // same filter as MeshCore
    sys_delay_us(500);
  }
  if (n >= 8) {
    int nf = sum / n;
    _noise_floor = nf < -120 ? -120 : nf;
  }
  if (sx_dio1()) service();                            // something started meanwhile
  if (_lp.rx_mode == RX_SNIFF && !_preamble_at && !_header_at) startRx();
}

void LoRaRadio::healthCheck() {
  uint8_t st = sx_status();
  uint16_t err = sx_device_errors();
  uint8_t mode = (st >> 4) & 7;
  // expected: RX (5) in continuous mode, STDBY_RC (2) right after the NSS wake of a sniffing radio
  bool ok = (st != 0x00 && st != 0xFF) && (mode == 5 || mode == 2 || mode == 3) && !(err & ~SX_ERR_XOSC_START & 0x7F);
  if (!ok) {
    _st.health_fixes++;
    if (_debug && Serial) Serial.printf("[lp] health fix: status 0x%02X errors 0x%04X\r\n", st, err);
    sx_init(_lp.tcxo_us);
    sx_configure(&_cfg);
  } else if (err) {
    sx_clear_device_errors();
  }
  startRx();
}

// ---------------------------------------------------------------- transmit side
bool LoRaRadio::channelBusyCheck() {
  bool busy = false;
  if (_threshold != 0) {
    if (_sniff_active) {
      sx_rx_continuous(DIO1_RX_MASK);
      _sniff_active = false;
      sys_delay_us(1500);
    }
    if (sx_rssi_inst() > _noise_floor + _threshold) busy = true;
  }
  if (!busy && _cad_enabled) {
    if (_tx_urgent) {
      // a reply someone is waiting for: the RX flags (preamble/header) still hold it back, CAD does not
    } else if (_cad_defer_run >= CAD_MAX_DEFER) {
      _st.cad_skip++;                 // CAD already held this TX long enough: rely on the RX flags only
    } else {
      // RadioLib scanChannel() parameters (2 symbols, detPeak = SF+13, detMin = 10), detPeak tuned +-3
      busy = cadOnce(false, cadPeak());
      if (busy) _cad_defer_run++;
      startRx();
    }
  }
  if (busy) startRx();
  return busy;
}

bool LoRaRadio::isReceiving() {
  service();
  peek();
  uint32_t now = millis();
  if (_state == ST_RX) {
    if (_header_at && !expired(now, _header_at, _payload_timeout_ms)) {
      _st.busy_hdr++;
      if (_debug && Serial) Serial.printf("[lp] tx deferred: header %lums ago\r\n", (unsigned long)(now - _header_at));
      return true;
    }
    if (_preamble_at && !_header_at && !expired(now, _preamble_at, _pre_timeout_ms)) {
      _st.busy_pre++;
      if (_debug && Serial) Serial.printf("[lp] tx deferred: preamble %lums ago\r\n", (unsigned long)(now - _preamble_at));
      return true;
    }
  }
  if (channelBusyCheck()) {
    _st.busy_chan++;
    if (_debug && Serial) Serial.printf("[lp] tx deferred: channel busy (lbt/cad)\r\n");
    return true;
  }
  return false;
}

bool LoRaRadio::startSendRaw(const uint8_t* bytes, int len) {
  if (len <= 0 || len > 255) return false;
  if (_lp.tx_inhibit) {          // test mode: pretend the packet went out, the receiver keeps running
    _st.tx_inhibited++;
    _fake_tx_done = true;
    sys_wake();                   // no TX_DONE interrupt will come: let the main loop run again
    return true;
  }
  _preamble_at = _header_at = 0;
  _sniff_active = false;
  _cad_defer_run = 0;
  _state = ST_TX;
  sx_tx(bytes, (uint8_t)len);
  return true;
}

bool LoRaRadio::isSendComplete() {
  service();
  if (_fake_tx_done) return true;
  if (_state == ST_TX_DONE) {
    n_sent++;
    _st.tx_ok++;
    return true;
  }
  return false;
}

void LoRaRadio::onSendFinished() {
  if (_fake_tx_done) { _fake_tx_done = false; return; }   // radio never left RX
  if (_state == ST_TX) _st.tx_timeout++;    // Dispatcher gave up waiting
  sx_standby();
  _state = ST_IDLE;
  startRx();
}

// One CAD run (blocking, ~2 symbols + TCXO start). Leaves the chip in STDBY_RC: caller restarts RX.
bool LoRaRadio::cadOnce(bool inverted, uint8_t peak) {
  if (inverted) sx_set_iq_inverted(true);
  sx_cad(CAD_SYMBOLS, peak, CAD_DET_MIN, false, 0, SX_IRQ_CAD_DONE);
  _sniff_active = false;
  uint32_t start = millis();
  while (!sx_dio1() && millis() - start < 50) { }
  uint16_t irq = sx_irq_status();
  sx_irq_clear(SX_IRQ_ALL);
  if (inverted) sx_set_iq_inverted(false);
  return (irq & SX_IRQ_CAD_DETECTED) != 0;
}

void LoRaRadio::cadProbe() {
  uint32_t now = millis();
  service();
  peek();
  if (_state != ST_RX || _preamble_at || _header_at) { _next_cad_probe_ms = now + 5000; return; }
  bool hot = _last_rx_ms && (now - _last_rx_ms) < CAD_HOT_MS;
  uint8_t peak = cadPeak();
  int fa = 0;
  for (int i = 0; i < CAD_PROBES_INV; i++) fa += cadOnce(true, peak);
  bool act = cadOnce(false, peak);
  startRx();

  _cad.inv_n += CAD_PROBES_INV;  _cad.inv_fa += fa;
  if (hot) { _cad.hot_n += CAD_PROBES_INV; _cad.hot_fa += fa; }
  _cad.norm_n++;  _cad.norm_busy += act;
  _cad.win_n += CAD_PROBES_INV;  _cad.win_fa += fa;
  if (_debug && Serial) Serial.printf("[lp] cad probe peak=%u fa=%d/%d act=%d%s\r\n", peak, fa, CAD_PROBES_INV, act, hot ? " hot" : "");
  if (_cad.win_n >= CAD_WINDOW || _cad.win_fa >= CAD_FAST_RAISE) {   // sudden interference: don't wait 30 min
    if (_lp.cad_auto) {
      if (_cad.win_fa * 50 > _cad.win_n && _cad_adj < CAD_ADJ_MAX) { _cad_adj++; _cad.ups++; }
      else if (_cad.win_fa == 0 && _cad_adj > -CAD_ADJ_MAX) { _cad_adj--; _cad.downs++; }
    }
    if (_debug && Serial) Serial.printf("[lp] cad window fa=%lu/%lu -> adj %d\r\n", (unsigned long)_cad.win_fa, (unsigned long)_cad.win_n, _cad_adj);
    _cad.win_n = _cad.win_fa = 0;
  }
  _next_cad_probe_ms = millis() + CAD_PROBE_MIN_MS + rng_u32() % CAD_PROBE_SPAN_MS;
}

void LoRaRadio::recalibrate() {
  sx_recalibrate();
  sx_configure(&_cfg);
  _st.recals++;
  if (_debug && Serial) Serial.printf("[lp] recalibrated\r\n");
  startRx();
}

void LoRaRadio::resetAGC() {
  if (_state != ST_RX || _sniff_active) return;   // sniffing re-inits AGC every window
  service();
  peek();                                          // continuous RX: nothing refreshes the flags otherwise
  if (_state != ST_RX || _preamble_at || _header_at) return;
  sx_standby();
  sx_sleep(true);
  startRx();
}

// ---------------------------------------------------------------- scoring (RadioLibWrapper)
uint32_t LoRaRadio::getEstAirtimeFor(int len_bytes) {
  return sx_time_on_air_us(&_cfg, len_bytes) / 1000;
}

static const float snr_threshold[] = { -7.5, -10, -12.5, -15, -17.5, -20 };

float LoRaRadio::packetScore(float snr, int packet_len) {
  int sf = _cfg.sf;
  if (sf < 7) return 0.0f;
  if (snr < snr_threshold[sf - 7]) return 0.0f;
  float success = (snr - snr_threshold[sf - 7]) / 10.0f;
  float collision = 1.0f - (packet_len / 256.0f);
  float s = success * collision;
  return s < 0 ? 0 : (s > 1 ? 1 : s);
}

// ---------------------------------------------------------------- receive experiment
struct ExpArm { const char* name; uint8_t mode, pre, det, boost; };
static const ExpArm exp_arms[LoRaRadio::EXP_ARMS] = {
  { "cont",     LoRaRadio::RX_CONTINUOUS, 0,  0, 1 },
  { "s16d7",    LoRaRadio::RX_SNIFF,      16, 7, 1 },
  { "s16d6",    LoRaRadio::RX_SNIFF,      16, 6, 1 },
  { "s16d7nb",  LoRaRadio::RX_SNIFF,      16, 7, 0 },
};

const char* LoRaRadio::armName(int a) { return (a >= 0 && a < EXP_ARMS) ? exp_arms[a].name : "-"; }

void LoRaRadio::expAccount() {
  uint32_t now = millis();
  if (_exp_arm >= 0) _exp_ms[_exp_arm] += now - _exp_since_ms;
  _exp_since_ms = now;
}

uint64_t LoRaRadio::expMs(int a) {
  if (_exp_period_ms) expAccount();
  return _exp_ms[a];
}

void LoRaRadio::expApplyArm(int a) {
  _exp_arm = a;
  if (Serial) Serial.printf("ARM %s\r\n", exp_arms[a].name);
  LpConfig lp = _lp;               // only the receive fields change, everything else stays live
  lp.rx_mode = exp_arms[a].mode;
  if (exp_arms[a].mode == RX_SNIFF) { lp.sniff_preamble = exp_arms[a].pre; lp.sniff_detect = exp_arms[a].det; }
  _lp = lp;
  if (_cfg.rx_boosted != (bool)exp_arms[a].boost) {
    _cfg.rx_boosted = exp_arms[a].boost;
    sx_standby();
    sx_set_rx_boosted(_cfg.rx_boosted);
  }
  computeTiming();
  startRx();
}

void LoRaRadio::startExperiment(uint32_t period_ms) {
  if (!_exp_period_ms) { _exp_saved = _lp; _exp_saved_boost = _cfg.rx_boosted; }
  memset(_exp_ok, 0, sizeof(_exp_ok));
  memset(_exp_err, 0, sizeof(_exp_err));
  memset(_exp_ms, 0, sizeof(_exp_ms));
  _exp_period_ms = period_ms;
  _exp_since_ms = millis();
  _exp_next_ms = _exp_since_ms + period_ms;
  expApplyArm(0);
}

void LoRaRadio::stopExperiment() {
  if (!_exp_period_ms) return;
  expAccount();
  _exp_period_ms = 0;
  _exp_arm = -1;
  _lp.rx_mode = _exp_saved.rx_mode;
  _lp.sniff_preamble = _exp_saved.sniff_preamble;
  _lp.sniff_detect = _exp_saved.sniff_detect;
  _cfg.rx_boosted = _exp_saved_boost;
  sx_standby();
  sx_set_rx_boosted(_cfg.rx_boosted);
  computeTiming();
  startRx();
}
