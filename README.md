# MeshCore LP repeater

A low-power MeshCore **repeater** firmware for the nRF52840 ProMicro + SX1262 "Faketec" class of
boards. It speaks the normal MeshCore protocol (based on MeshCore 1.17.1) and is managed with the
usual tools and CLI, but it is written bare-metal (no Arduino core, no RadioLib, no SoftDevice at
run time) so that it can keep the radio receiver mostly asleep without losing packets.

What you get compared to the stock `ProMicro_repeater` build:

* **Duty-cycled receive ("sniff")**: the SX1262 listens in short windows timed so that any packet
  with a preamble of at least 16 symbols is caught; the MCU sleeps ~99 % of the time.
  Estimated radio current ~3 mA instead of ~5.3 mA in continuous receive (plus MCU/board).
  Reception was measured on par with continuous receive and with stock firmware.
* **Listen-before-talk with CAD** whose threshold tunes itself to the local noise.
* **Periodic full radio recalibration** and supervision of the receiver state.
* **Quiet defaults**: a freshly flashed node does not repeat and does not advertise until configured.
* **USB / BLE firmware update** through the Adafruit bootloader, without a BLE stack in the firmware.

Everything protocol-related (routing, ACL, regions, CLI) is MeshCore's; this README lists only what
differs.

## Supported hardware

* nRF52840 ProMicro / nice!nano-compatible board with the Adafruit nRF52 bootloader and the
  **S140 6.1.1** SoftDevice present (the firmware does not use it, but the bootloader requires it).
* SX1262 module with **1.8 V TCXO on DIO3** and **DIO2 driving the RF switch** (HT-RA62 class),
  wired as on the Faketec v4 PCB (`src/hal/board.h`).
* The module's RXEN input is driven by the firmware (high in receive). Modules that leave it floating
  lose noticeable sensitivity - if you adapt the pin map, keep it driven.

Other wirings need `src/hal/board.h` (pins) and possibly `SX126X_TCXO_VOLTAGE_CODE` adapted.

## Installing

Release packages (`make release`, see *Building*):

| File | Use |
|---|---|
| `repeater.zip` | serial DFU: `adafruit-nrfutil dfu serial -pkg repeater.zip -p <port> -b 115200 --singlebank` (or `make flash-repeater`) |
| `repeater.uf2` | double-tap RESET, copy to the bootloader's USB drive |
| `repeater-sd.zip` | same as `repeater.zip` plus the S140 SoftDevice, for a chip whose SoftDevice was erased |

Settings are kept across updates of this firmware. Stock and this firmware store settings
differently: switching between them (either way) starts the node with default settings, so export
the identity first if you want to keep it (`get prv.key` / `set prv.key`).

### First setup

A new node is **silent**: forwarding off, no adverts. Connect over USB (115200 baud, any terminal)
or log in remotely with the default admin password `password`, then:

```
password <new admin password>
set name <name>
set lat <lat> / set lon <lon>
set radio <freq>,<bw>,<sf>,<cr>      # if your mesh does not use the build default
clock sync                           # or: time <epoch>
set repeat on
set advert.interval 240              # local advert, minutes (0 = off)
set flood.advert.interval 12         # flood advert, hours (0 = off)
advert                               # announce now
```

Adverts on boot are sent only once one of the advert intervals is non-zero.

## Commands that differ from stock

Commands work over USB and over the remote CLI unless noted. Settings set with `set lp.*` take
effect immediately and survive reboots and firmware updates.

### Receiver and power

| Command | Default | Meaning |
|---|---|---|
| `lp` | | current receiver setup: mode, window / sleep length, duty, preamble, window size |
| `set lp.rx sniff\|cont` | `sniff` | duty-cycled or continuous receive |
| `set lp.pre <n>` | `16` | shortest neighbour preamble (symbols) that must still be caught. `16` covers MeshCore firmware from mid-2025 on; `32` (current firmware at SF ≤ 8) roughly halves the radio current again but misses older nodes and companions; `0` = own default for the SF |
| `set lp.det <n>` | `7` | receive window in symbols (4..16). Shorter saves power and misses more weak packets; 7 measured equal to continuous receive |
| `set lp.tcxo <us>` | `1000` | TCXO start-up time budget |
| `set lp.recal <min>` | `60` | full radio recalibration period (0 = off) |
| `lp stats` | | receiver counters, see below |

`powersaving on|off` is accepted for app compatibility but has no effect: power saving is the
receive mode above, and the MCU always sleeps when idle.

`lp stats` fields: `ok`/`crc`/`hdr` packets received / CRC errors / header errors, `fpre` preambles
that never produced a header, `lost` receptions that stalled after the header (should stay 0),
`tx`, `txto` transmit timeouts, `fix` receiver re-initialisations by the health check, `busy=a/b/c`
transmissions deferred because a preamble / a header was being received / the channel check said
busy, `mcusleep` share of time the MCU slept.

### Channel access (CAD)

| Command | Default | Meaning |
|---|---|---|
| `set cad on\|off` | **on** | hardware channel activity check before transmitting (stock default: off) |
| `get cad` | | as stock, plus the current threshold and measured false-alarm rate |
| `set lp.cad auto\|<-3..3>` | `auto` | CAD detection threshold: automatic, or a fixed offset from the radio's default |
| `lp cad` | | CAD details: threshold, false alarms (`fa`), probes right after traffic (`hot`), activity seen (`act`), threshold steps, `skip`, recalibrations |

How it behaves:

* About once a minute the node runs a few CAD probes with *inverted IQ*, which do not respond to
  MeshCore traffic but do respond to noise and interference. Their hit rate is the CAD false-alarm
  rate; every ~30 minutes the threshold moves one step (within ±3) to keep it around 1-2 %.
* One transmission is held back by CAD at most three times (random 120-360 ms each); after that only
  a packet actually being received holds it.
* Replies and other direct-routed packets skip the CAD hold-off (they still wait for a reception in
  progress), so a busy channel does not delay answers to your requests.

`set int.thresh`, `set agc.reset.interval` work as in stock. AGC reset (default **8 s** here) only
matters in continuous receive; in sniff mode the receiver restarts every window anyway.

### Firmware update

| Command | Meaning |
|---|---|
| `start ota` | shows the options |
| `start ota usb` | reboot into the USB bootloader (needs USB power): serial DFU / UF2 |
| `start ota ble` | reboot into the bootloader's BLE DFU mode, update with nRF Connect / a MeshCore web flasher |

BLE DFU is run by the bootloader itself and is **slow**. The bootloader waits until an update completes
with **no timeout**: the node is off the mesh until it is updated or reset / power-cycled.

### Battery measurement

| Command | Meaning |
|---|---|
| `lp bat` | battery voltage, multiplier and raw ADC readings, VDDH/VDD, USB present |
| `set lp.bat <mV>` | one-point calibration: enter the voltage measured with a meter (on battery, USB unplugged); sets `adc.multiplier` |

The default multiplier is stock's (1.815). Divider resistors and ADC gain vary between boards: a
one-point calibration typically moves readings by a few percent, which matters for state of charge.

### Diagnostics

| Command | Meaning |
|---|---|
| `log start` / `log stop` | packet log is printed to the USB console (stock writes it to a flash file); `log` (dump) is not supported |
| `lp debug 0\|1\|2` | radio diagnostics on the USB console (1: anomalies and deferrals, 2: every radio interrupt) |
| `lp exp <min>` / `lp exp` / `lp exp 0` | rotate receive configurations every `<min>` minutes and count packets per configuration / show / stop |
| `set lp.txinhibit on\|off` | test mode: never transmit (sends are faked) |
| `set lp.rearm on\|off` | continuous receive only: restart RX after every packet, as RadioLib does |

## Differences and limitations

* No sensors, GPS, display or bridges; no BLE at run time (only BLE DFU via the bootloader).
* The watchdog started by this firmware survives a firmware update; stock firmware flashed afterwards
  gets one watchdog reset about a minute after boot. Harmless.
* Sniff mode cannot guarantee reception of nodes that send short (8-symbol, pre-mid-2025 MeshCore)
  preambles; use `set lp.rx cont` in such a mesh.
* The clock restarts at a fixed date after power loss or reboot until synced (as stock without an RTC
  chip).

## Building

```
git clone --recursive <this repo>
cd <repo>
cp config.mk.example config.mk     # radio defaults for your mesh, host tool paths
make release                       # build/release: repeater.zip, repeater.uf2, (repeater-sd.zip), SHA256SUMS
make test                          # host unit tests
```

Needs `arm-none-eabi-gcc` with newlib and its C++ library (tested with 13.2; Debian/Ubuntu packages
`gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib`), GNU make, Python 3 with
`adafruit-nrfutil` and `pyserial`. `repeater-sd.zip` is built only if `third_party/softdevice/s140_nrf52_6.1.1_softdevice.hex`
exists (not redistributed here; it is in the Adafruit nRF52 bootloader repository).

Developer documentation: [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md), MeshCore changes:
[docs/MESHCORE_PATCHES.md](docs/MESHCORE_PATCHES.md).

## License

MIT, see [LICENSE](LICENSE). Third-party components: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
