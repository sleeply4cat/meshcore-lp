// Low-power MeshCore repeater for Faketec v4 (nRF52840 + SX1262), bare metal.
// Based on MeshCore examples/simple_repeater/main.cpp; the main loop sleeps (WFI) until the next
// timed deadline or an interrupt (radio DIO1, USB, RTC).
#include <Arduino.h>
#include <Mesh.h>
#include "MyMesh.h"
#include "hal/usb.h"
#include "hal/rng.h"
#include "hal/gpio.h"

#ifndef LP_WATCHDOG_S
  #define LP_WATCHDOG_S     60
#endif
#define MAX_SLEEP_MS        30000   // housekeeping: watchdog, RTC clock accumulation

StdRNG fast_rng;
SimpleMeshTables tables;
MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

static char command[160];

class HwRNG : public mesh::RNG {
public:
  void random(uint8_t* dest, size_t sz) override { rng_fill(dest, sz); }
};

static void halt_blink(void) {
  for (;;) {
    sys_wdt_feed();
    gpio_write(PIN_LED, true);  sys_delay_ms(100);
    gpio_write(PIN_LED, false); sys_delay_ms(900);
    usb_task();
  }
}

static void handle_serial_cli(void) {
  int c;
  int len = strlen(command);
  while ((c = usb_read()) >= 0) {
    if (c == '\n') continue;
    if (c == '\r') {
      Serial.print("\r\n");
      if (len == 0) continue;
      char reply[192];
      reply[0] = 0;
      the_mesh.handleCommand(0, command, reply);   // no sender_timestamp via serial
      if (reply[0]) { Serial.print("  -> "); Serial.print(reply); Serial.print("\r\n"); }
      command[0] = 0;
      len = 0;
      continue;
    }
    if ((c == 8 || c == 127)) {
      if (len > 0) { command[--len] = 0; Serial.print("\b \b"); }
      continue;
    }
    if (len < (int)sizeof(command) - 1 && c >= 32) {
      command[len++] = (char)c;
      command[len] = 0;
      Serial.print((char)c);
    }
  }
}

int main(void) {
  sys_init();
  sys_boot_stage(1);
  board.begin();
  usb_init();
  sys_wdt_start(LP_WATCHDOG_S);
  sys_boot_stage(2);

  InternalFS.begin();
  sys_boot_stage(21);

  // low power radio settings live in their own small file, read them before bringing up the radio
  LoRaRadio::LpConfig lp;
#ifdef LP_DEFAULT_TX_INHIBIT
  lp.tx_inhibit = LP_DEFAULT_TX_INHIBIT;
#endif
  {
    File f = InternalFS.open("/lp_prefs");
    if (f) {
      lpPrefsRead(f, lp);
      f.close();
    }
  }
  sys_boot_stage(22);
  if (!radio_driver.init(lp)) halt_blink();
  sys_boot_stage(3);

  fast_rng.begin(radio_driver.getRngSeed());

  IdentityStore store(InternalFS, "");
  if (!store.load("_main", the_mesh.self_id)) {
    HwRNG hw_rng;
    the_mesh.self_id = mesh::LocalIdentity(&hw_rng);
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = mesh::LocalIdentity(&hw_rng);
      count++;
    }
    store.save("_main", the_mesh.self_id);
  }
  sys_boot_stage(31);

  command[0] = 0;
  sensors.begin();
  sys_boot_stage(32);
  the_mesh.begin(&InternalFS);
  sys_boot_stage(4);

#if ENABLE_ADVERT_ON_BOOT == 1
  // [LP] only once adverts are enabled (a fresh node with LP_QUIET_DEFAULTS stays silent)
  if (the_mesh.getNodePrefs()->advert_interval || the_mesh.getNodePrefs()->flood_advert_interval)
    the_mesh.sendSelfAdvertisement(16000, false);
#endif
  board.onBootComplete();
  sys_boot_stage(5);

  bool was_connected = false;
  for (;;) {
    sys_clear_wake();
    sys_wdt_feed();
    usb_task();

    bool conn = usb_connected_ms() > 300;   // host tty may echo for a moment after open
    if (conn && !was_connected) {
      usb_discard_input();
      Serial.print("\r\nMeshCore-LP repeater ");
      Serial.print(FIRMWARE_VERSION);
      Serial.print("  ID: ");
      mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE);
      Serial.printf("\r\nreset: %s, last fault pc: 0x%08lX, boot stage: %lu\r\n", sys_reset_reason_str(),
                    (unsigned long)sys_last_fault_pc(), (unsigned long)sys_prev_boot_stage());
    }
    was_connected = conn;
    if (conn) handle_serial_cli();

    the_mesh.loop();
    InternalFS.sync();          // flush lone file removals (normally merged with the next write)
    rtc_clock.tick();
    sys_lfclk_maintain();

    if (g_bootloader_reboot_at && (long)(millis() - g_bootloader_reboot_at) >= 0) sys_reset_to_bootloader_mode(g_bootloader_magic);

    // never sleep with a radio event pending: DIO1 is level-sensitive and must be serviced
    if (radio_driver.irqPending()) continue;

    unsigned long deadline = the_mesh.getNextWakeMillis(MAX_SLEEP_MS);
    if (usb_connected() && !was_connected && (long)(deadline - millis()) > 50) deadline = millis() + 50;
    if (g_bootloader_reboot_at && (long)(g_bootloader_reboot_at - deadline) < 0) deadline = g_bootloader_reboot_at;
    sys_sleep_until_ms(deadline);
  }
}
