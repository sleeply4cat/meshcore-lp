// Board glue expected by MeshCore's simple_repeater (replaces variants/promicro/target.h).
#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/SensorManager.h>
#include "LoRaRadio.h"
#include "hal/adc.h"
#include "hal/board.h"
#include "hal/usb.h"

#define ADC_MULTIPLIER_DEFAULT  (1.815f)   // MeshCore PromicroBoard value (same SAADC setup)

class FaketecBoard : public mesh::MainBoard {
  float _adc_mult = ADC_MULTIPLIER_DEFAULT;
public:
  void begin();
  uint16_t getBattMilliVolts() override { return (uint16_t)(_adc_mult * adc_read_raw(VBAT_AIN, 8)); }
  bool setAdcMultiplier(float m) override { _adc_mult = (m == 0.0f) ? ADC_MULTIPLIER_DEFAULT : m; return true; }
  float getAdcMultiplier() const override { return _adc_mult; }
  float getMCUTemperature() override { return adc_mcu_temp_x4() * 0.25f; }
  const char* getManufacturerName() const override { return "Faketec v4 LP"; }
  void reboot() override { sys_reset(); }
  void powerOff() override;
  uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
  bool getBootloaderVersion(char* version, size_t max_len) override;
  bool startOTAUpdate(const char* id, char reply[]) override;
  bool isExternalPowered() override { return usb_vbus(); }
  uint32_t getResetReason() const override { return sys_reset_reason(); }
  const char* getResetReasonString(uint32_t reason) override { return sys_reset_reason_str(); }
};

extern FaketecBoard board;
extern uint32_t g_bootloader_reboot_at;
extern uint8_t  g_bootloader_magic;   // GPREGRET for the bootloader: 0x57 USB DFU, 0xA8 BLE OTA
// 'start ota [usb|ble]': shared by the MeshCore CLI hook and the board callback
bool ota_command(const char* arg, char reply[]);
extern VolatileRTCClock rtc_clock;
extern SensorManager sensors;

#define WRAPPER_CLASS LoRaRadio
