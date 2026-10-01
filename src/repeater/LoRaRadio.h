// mesh::Radio implementation on top of the bare SX1262 driver, with a low-power receive mode.
//
// RX modes
//  - RX_CONTINUOUS: classic always-on receiver (what stock MeshCore does).
//  - RX_SNIFF:      SX1262 RxDutyCycle. The radio wakes itself for RX windows of sniff_detect symbols,
//                   spaced so that any preamble of >= sniff_preamble symbols is detected (measured
//                   detection latencies, see computeTiming()). Timer is stopped on preamble detection; the MCU supervises
//                   (false preamble / lost header -> re-arm), so a false detection cannot keep the
//                   receiver on indefinitely.
//
// All radio events arrive on DIO1 (preamble, header, done, errors); the MCU sleeps in between.
#pragma once

#include <Mesh.h>
#include "hal/sx1262.h"

class LoRaRadio : public mesh::Radio {
public:
  enum RxMode : uint8_t { RX_CONTINUOUS = 0, RX_SNIFF = 1 };

  struct LpConfig {
    uint8_t  rx_mode = RX_SNIFF;
    uint8_t  sniff_preamble = 16;  // shortest preamble to guarantee (16: MeshCore fw since Jul 2025; 0 = own SF default)
    uint8_t  sniff_detect = 7;     // RX window in symbols = budget for cold-start preamble detection
    uint16_t tcxo_us = 1000;       // DIO3 TCXO start-up delay (HT-RA62 measured: starts in 250..300 us)
    uint8_t  tx_inhibit = 0;       // test mode: never key the transmitter (sends are faked)
    uint8_t  cont_rearm = 0;       // continuous RX: re-issue SetRx after every packet (as RadioLib/stock does)
    // --- appended in LP02 files (older files keep these defaults)
    uint8_t  cad_auto = 1;         // CAD threshold auto-tuned from inverted-IQ false-alarm probes
    int8_t   cad_adj = 0;          // fixed detPeak offset from SF+13 when cad_auto = 0 (-3..3)
    uint8_t  recal_min = 60;       // full chip recalibration period, minutes (0 = off)
  };
  static const int CAD_ADJ_MAX = 3;

  struct Stats {
    uint32_t rx_ok, rx_crc, rx_hdr_err, rx_false_pre, rx_lost_hdr, rx_overrun;
    uint32_t tx_ok, tx_timeout, tx_inhibited, rearms, health_fixes, peek_rearm;
    uint32_t busy_pre, busy_hdr, busy_chan;   // isReceiving() == true (TX deferred) by reason
    uint32_t cad_skip;                         // CAD ignored after CAD_MAX_DEFER busy answers for one TX
    uint32_t recals;                           // periodic full recalibrations
    uint64_t rx_on_ms_est;   // not measured, informative only
  };

  bool init(const LpConfig& lp);         // chip bring-up, returns false if no SX1262 found
  void setLpConfig(const LpConfig& lp);  // applies RX mode / sniff params
  const LpConfig& lpConfig() const { return _lp; }

  // MeshCore wrapper-compatible API (used by MyMesh / CommonCLI)
  void setParams(float freq, float bw, uint8_t sf, uint8_t cr);
  void setTxPower(int8_t dbm);
  bool setRxBoostedGainMode(bool en);
  bool getRxBoostedGainMode() const { return _cfg.rx_boosted; }
  uint32_t getRngSeed();
  void powerOff();
  uint32_t getPacketsRecv() const { return n_recv; }
  uint32_t getPacketsRecvErrors() const { return n_recv_errors; }
  uint32_t getPacketsSent() const { return n_sent; }
  void resetStats() { n_recv = n_sent = n_recv_errors = 0; memset(&_st, 0, sizeof(_st)); }
  const Stats& lpStats() const { return _st; }

  // sniff timing actually in use (0/0 when continuous)
  uint32_t sniffRxUs() const { return _sniff_rx_us; }
  uint32_t sniffSleepUs() const { return _sniff_sleep_us; }
  bool sniffActive() const { return _sniff_active; }
  bool txInhibited() const { return _lp.tx_inhibit != 0; }
  void setDebug(uint8_t level) { _debug = level; }   // 1: anomalies, 2: every IRQ

  // mesh::Radio
  void begin() override;
  int recvRaw(uint8_t* bytes, int sz) override;
  uint32_t getEstAirtimeFor(int len_bytes) override;
  float packetScore(float snr, int packet_len) override;
  bool startSendRaw(const uint8_t* bytes, int len) override;
  bool isSendComplete() override;
  void onSendFinished() override;
  void loop() override;
  int getNoiseFloor() const override { return _noise_floor; }
  void triggerNoiseFloorCalibrate(int threshold) override { _threshold = threshold; }
  void setCADEnabled(bool enable) override { _cad_enabled = enable; }
  void setNextTxUrgent(bool urgent) override { _tx_urgent = urgent; }
  void resetAGC() override;
  bool isInRecvMode() const override { return _state == ST_RX; }
  bool isReceiving() override;
  float getLastRSSI() const override { return _last_rssi; }
  int lastPreToHdrMs() const { return -1; }   // not measured any more (flags are only peeked)
  float getLastSNR() const override { return _last_snr; }

  // Receive experiment: rotate through several RX configurations ("arms") every period and count
  // packets per arm (arm 0 = continuous, others = sniff with different preamble/detect settings).
  static const int EXP_ARMS = 4;   // cont, default sniff, one shorter window, default without boosted gain
  void startExperiment(uint32_t period_ms);
  void stopExperiment();
  bool expActive() const { return _exp_period_ms != 0; }
  int  expArm() const { return _exp_arm; }
  uint32_t expOk(int a) const { return _exp_ok[a]; }
  uint32_t expErr(int a) const { return _exp_err[a]; }
  uint64_t expMs(int a);
  static const char* armName(int a);

  // CAD threshold auto-tuning (see cadProbe()): current offset and probe statistics
  struct CadStats {
    uint32_t inv_n, inv_fa;          // inverted-IQ probes: false alarms (MeshCore traffic is invisible to them)
    uint32_t hot_n, hot_fa;          //   of which taken < 3 s after a reception (neighbours retransmitting)
    uint32_t norm_n, norm_busy;      // normal-IQ probes: channel activity as CAD sees it
    uint32_t win_n, win_fa;          // current tuning window
    uint32_t ups, downs;             // threshold steps taken
  };
  int  cadAdj() const { return _cad_adj; }
  uint8_t cadPeak() const { return _cfg.sf + 13 + _cad_adj; }
  const CadStats& cadStats() const { return _cad; }
  bool cadEnabled() const { return _cad_enabled; }

  // low power scheduling: earliest millis() the driver needs loop() again (supervision timers)
  unsigned long nextDeadline(unsigned long max_ms);
  // true if DIO1 is asserted (an event is waiting)
  bool irqPending() const;

private:
  enum State : uint8_t { ST_IDLE, ST_RX, ST_TX, ST_TX_DONE };

  sx_config_t _cfg = {};
  LpConfig _lp;
  State    _state = ST_IDLE;
  bool     _sniff_active = false;
  bool     _fake_tx_done = false;   // tx_inhibit test mode
  uint8_t  _debug = 0;
  uint32_t _sniff_rx_us = 0, _sniff_sleep_us = 0;
  uint32_t _pre_timeout_ms = 100, _payload_timeout_ms = 4000;

  // receive bookkeeping
  uint32_t _preamble_at = 0, _header_at = 0;   // millis(), 0 = none
  uint8_t  _rx_buf[256];
  int      _rx_len = 0;
  float    _last_rssi = 0, _last_snr = 0;
  int      _last_pre_hdr_ms = -1;

  // noise floor (sampled rarely, sniff-friendly)
  int16_t  _noise_floor = 0;
  int      _threshold = 0;
  bool     _cad_enabled = false;
  bool     _tx_urgent = false;
  uint32_t _next_floor_ms = 0;
  uint32_t _next_health_ms = 0;
  uint32_t _next_peek_ms = 0;

  uint32_t n_recv = 0, n_sent = 0, n_recv_errors = 0;
  Stats    _st = {};

  uint32_t _exp_period_ms = 0, _exp_next_ms = 0, _exp_since_ms = 0;
  int      _exp_arm = -1;
  LpConfig _exp_saved;
  bool     _exp_saved_boost = true;
  uint32_t _exp_ok[EXP_ARMS] = {}, _exp_err[EXP_ARMS] = {};
  uint64_t _exp_ms[EXP_ARMS] = {};
  void expAccount();
  void expApplyArm(int a);

  // CAD
  int8_t   _cad_adj = 0;
  uint8_t  _cad_defer_run = 0;       // consecutive CAD 'busy' answers for the pending TX
  uint32_t _next_cad_probe_ms = 0;
  uint32_t _last_rx_ms = 0;
  CadStats _cad = {};
  uint32_t _next_recal_ms = 0;
  bool cadOnce(bool inverted, uint8_t peak);
  void cadProbe();
  void recalibrate();

  void computeTiming();
  void startRx();
  void service();
  void peek();
  void sampleNoiseFloor();
  void healthCheck();
  bool channelBusyCheck();
};

extern LoRaRadio radio_driver;
