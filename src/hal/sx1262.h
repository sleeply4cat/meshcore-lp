// Minimal SX1262 driver (LoRa only), register level, no RadioLib.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// IRQ bits
#define SX_IRQ_TX_DONE        (1u << 0)
#define SX_IRQ_RX_DONE        (1u << 1)
#define SX_IRQ_PREAMBLE       (1u << 2)
#define SX_IRQ_SYNCWORD       (1u << 3)
#define SX_IRQ_HEADER_VALID   (1u << 4)
#define SX_IRQ_HEADER_ERR     (1u << 5)
#define SX_IRQ_CRC_ERR        (1u << 6)
#define SX_IRQ_CAD_DONE       (1u << 7)
#define SX_IRQ_CAD_DETECTED   (1u << 8)
#define SX_IRQ_TIMEOUT        (1u << 9)
#define SX_IRQ_ALL            0x03FFu

// device errors (GetDeviceErrors)
#define SX_ERR_XOSC_START     (1u << 5)

typedef struct {
  uint32_t freq_hz;
  float    bw_khz;       // 7.8 .. 500
  uint8_t  sf;           // 5..12
  uint8_t  cr;           // 5..8  (4/5 .. 4/8)
  uint16_t preamble;     // symbols
  int8_t   tx_power;     // dBm, -9..22
  bool     rx_boosted;
} sx_config_t;

typedef struct {
  int16_t rssi_pkt;      // dBm
  int16_t signal_rssi;   // dBm
  float   snr;           // dB
} sx_pkt_status_t;

bool     sx_init(uint32_t tcxo_delay_us);  // power, reset, detect chip, TCXO, calibrate. true if chip found
bool     sx_has_tcxo(void);
void     sx_configure(const sx_config_t* cfg);
const sx_config_t* sx_config(void);
void     sx_set_tx_power(int8_t dbm);
void     sx_set_rx_boosted(bool on);

void     sx_standby(void);                 // STDBY_RC (wakes the chip if sleeping)
void     sx_sleep(bool warm);              // warm = keep config (needed for RxDutyCycle-like use)
void     sx_rx_continuous(uint16_t irq_mask_dio1);
void     sx_rx_single(uint32_t timeout_us, uint16_t irq_mask_dio1, bool stop_on_preamble);   // RX with timeout
void     sx_rx_duty_cycle(uint32_t rx_us, uint32_t sleep_us, uint16_t irq_mask_dio1, bool stop_on_preamble);
void     sx_cad(uint8_t symbols, uint8_t det_peak, uint8_t det_min, bool exit_to_rx, uint32_t rx_timeout_us, uint16_t irq_mask_dio1);
bool     sx_tx(const uint8_t* data, uint8_t len);   // starts TX, DIO1 on TX_DONE|TIMEOUT
void     sx_set_iq_inverted(bool inv);        // CAD false-alarm probes only; restore before RX/TX
void     sx_recalibrate(void);                // Calibrate(0x7F) + chip setup; follow with sx_configure()

uint16_t sx_irq_status(void);
// Read IRQ flags only if the chip is awake (BUSY low); never wakes a sleeping/sniffing chip.
// Returns false (nothing read) if BUSY is high. *status receives the GetStatus byte (chip mode in bits 6:4).
bool     sx_peek_irq(uint16_t* irq, uint8_t* status);
void     sx_irq_clear(uint16_t mask);
int      sx_read_packet(uint8_t* buf, int max_len);  // returns length (reads RX buffer)
void     sx_packet_status(sx_pkt_status_t* st);
int16_t  sx_rssi_inst(void);
uint16_t sx_device_errors(void);
void     sx_clear_device_errors(void);
uint8_t  sx_status(void);                  // GetStatus byte
uint32_t sx_random32(void);                // needs RX mode, uses RandomNumberGen register
bool     sx_busy(void);
bool     sx_dio1(void);

void     sx_write_reg(uint16_t addr, const uint8_t* data, uint8_t n);
void     sx_read_reg(uint16_t addr, uint8_t* data, uint8_t n);

uint32_t sx_time_on_air_us(const sx_config_t* cfg, int len);   // RadioLib-compatible formula
uint32_t sx_symbol_us(const sx_config_t* cfg);

// diagnostics
uint32_t sx_busy_timeouts(void);

#ifdef __cplusplus
}
#endif
