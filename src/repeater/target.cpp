#include "target.h"
#include "hal/gpio.h"
#include "hal/sx1262.h"
#include "nrf.h"

FaketecBoard board;
VolatileRTCClock rtc_clock;
SensorManager sensors;

void FaketecBoard::begin() {
  gpio_output(PIN_LED, false);
  // MOSFET outputs have PCB pull-downs; leave the pins disconnected (lowest leakage).
  gpio_input(PIN_BUTTON, GPIO_PIN_CNF_PULL_Disabled);
  gpio_disconnect(PIN_BUTTON);
  gpio_input(PIN_VBAT, GPIO_PIN_CNF_PULL_Disabled);
  gpio_disconnect(PIN_VBAT);   // SAADC input does not need the digital buffer
  adc_calibrate_offset();      // Nordic: calibrate the SAADC offset once at start (a few LSB)
}

void FaketecBoard::powerOff() {
  radio_driver.powerOff();
  NRF_POWER->SYSTEMOFF = 1;    // wake only by reset / VBUS
  for (;;) { __WFE(); }
}

bool FaketecBoard::getBootloaderVersion(char* out, size_t max_len) {
  static const char marker[] = "UF2 Bootloader ";
  const uint8_t* flash = (const uint8_t*)0x000FB000;
  for (uint32_t i = 0; i < 0x3000 - (sizeof(marker) - 1); i++) {
    if (memcmp(&flash[i], marker, sizeof(marker) - 1) == 0) {
      const char* ver = (const char*)&flash[i + sizeof(marker) - 1];
      size_t len = 0;
      while (len < max_len - 1 && ver[len] && ver[len] != ' ' && ver[len] != '\n' && ver[len] != '\r') {
        out[len] = ver[len];
        len++;
      }
      out[len] = 0;
      return len > 0;
    }
  }
  return false;
}

uint32_t g_bootloader_reboot_at = 0;   // polled by the main loop
uint8_t  g_bootloader_magic = 0x57;

static void schedule_bootloader(uint8_t magic, uint32_t delay_ms) {
  g_bootloader_magic = magic;
  g_bootloader_reboot_at = millis() + delay_ms;   // delayed, so the CLI reply still goes out
  if (!g_bootloader_reboot_at) g_bootloader_reboot_at = 1;
}

// This firmware has no BLE stack of its own. BLE OTA is done by the Adafruit bootloader: GPREGRET
// 0xA8 (DFU_MAGIC_OTA_RESET) makes it start the SoftDevice and advertise its DFU service. It then
// waits with no timeout (and keeps feeding our watchdog) until an update completes or the board is
// reset / power cycled - the node is off the mesh meanwhile.
bool ota_command(const char* arg, char reply[]) {
  while (*arg == ' ') arg++;
  if (strcmp(arg, "usb") == 0) {
    if (!usb_vbus()) { strcpy(reply, "Err - no USB power"); return true; }
    schedule_bootloader(0x57, 1500);
    strcpy(reply, "OK - rebooting to USB DFU bootloader");
  } else if (strcmp(arg, "ble") == 0) {
    schedule_bootloader(0xA8, 3000);
    strcpy(reply, "OK - rebooting to BLE DFU bootloader; off the mesh until updated or RESET/power cycle");
  } else {
    strcpy(reply, "start ota usb - USB DFU (serial/UF2); start ota ble - BLE DFU, waits until updated or RESET/power cycle");
  }
  return true;
}

bool FaketecBoard::startOTAUpdate(const char* id, char reply[]) {
  return ota_command("", reply);
}
