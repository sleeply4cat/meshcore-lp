# Development notes

Internal documentation for working on this firmware: how it is built, how the radio side works,
what was measured and how, and what to check when adapting it to other hardware or another mesh.
User-facing behaviour is in the [README](../README.md).

## 1. Goals and constraints

* Lowest possible average current for an always-on MeshCore repeater on nRF52840 + SX1262, without
  losing packets compared to a continuously receiving stock repeater.
* Protocol behaviour identical to MeshCore 1.17.1; only scheduling, radio access and diagnostics differ.
* One hardware family (Faketec-style ProMicro + HT-RA62-class module); anything not needed for a
  repeater (BLE at run time, sensors, display, bridges) is left out.

## 2. Repository layout

```
Makefile, repeater.mk     build (repeater, lab, release, test, flash-*)
config.mk.example         local build settings (radio defaults, host tool paths) -> config.mk
ld/app.ld                 linker script (app above the S140 SoftDevice, retained RAM block)
src/hal/                  bare-metal drivers: system (clocks, sleep, WDT, RTC), gpio, spi, sx1262,
                          dio1 (GPIOTE), usb (TinyUSB CDC), console, adc, rng, nvmc, syscalls
src/compat/               Arduino/Adafruit API shims MeshCore needs (Print/Stream/Serial, File/FS,
                          RTClib DateTime, CayenneLPP) + flashfs.cpp (A/B-bank flash file store)
src/meshcore/             MeshCore core + helpers (copy, [LP] edits, see MESHCORE_PATCHES.md)
src/repeater/             MyMesh (simple_repeater), LoRaRadio (radio policy), target (board glue), main
src/lab/                  "radio lab" firmware: RX-only experiments (detection latency, TCXO, sniff)
test/                     host unit tests (flash file store with simulated power loss)
tools/                    flashing, serial monitor, test orchestration and analysis scripts
third_party/              nrfx, tinyusb (submodules), cmsis, crypto, ed25519
```

## 3. Build, flash, memory map

* Toolchain: `arm-none-eabi-gcc` (13.2 tested) with newlib and libstdc++ (Debian/Ubuntu:
  `gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib`), GNU make, Python 3 with
  `adafruit-nrfutil`, `pyserial`.
  `make PYTHON=... NRFUTIL=...` or `config.mk` select the host tools.
* `make repeater` -> `build/repeater.{hex,zip}`; `make release` -> clean tree in `build/release`
  (zip, uf2, optional SoftDevice zip, checksums); `make lab`; `make test`; `make flash-<target> PORT=...`.
* Releases: push a tag `v<meshcore version>-lp<n>` (e.g. `v1.17.1-lp2`); CI builds with that version string
  (`FW_VERSION`) and publishes a GitHub Release with the packages and checksums. Branch builds keep the
  packages as a workflow artifact, versioned `<FW_VERSION>-<commit>`.
* Radio defaults for a freshly erased node come from `config.mk` (`LORA_FREQ/BW/SF/CR/TX_POWER`,
  `PATH_HASH_MODE`); they only matter until the node is configured.
* Flash: application at `0x26000` (S140 6.1.1 occupies `0x1000..0x26000`; it is never enabled, but the
  Adafruit bootloader checks for it). File store: two 12 KB banks at `0xED000` and `0xF0000`
  (`compat/flashfs.cpp`, CRC32 + header written last, deferred removal so that remove+rewrite is atomic).
  Bootloader at `0xF4000`.
* RAM: `0x20000000..0x2003BF00` for the application; `0x2003BF00..0x2003C000` is a retained block
  (boot stage, last fault PC, reset reason) placed below the bootloader's stack so it survives resets
  and DFU.
* DFU: `tools/dfu.py` does what PlatformIO does - 1200-baud touch, wait for the bootloader port, then
  `adafruit-nrfutil dfu serial`. Wait a few seconds after the bootloader port appears before starting
  DFU: a host process probing new serial ports (modem managers) can wedge the bootloader, which then
  needs a hardware reset.

## 4. Runtime architecture

* **Main loop** (`repeater/main.cpp`): service USB, run `MyMesh::loop()`, then sleep until the earliest
  deadline (`MyMesh::getNextWakeMillis()` -> `Dispatcher::getNextWakeMillis()` +
  `LoRaRadio::nextDeadline()`) or an interrupt. Never sleeps while DIO1 is asserted.
* **Sleep**: race-free WFI - ISRs call `sys_wake()`, the loop calls `sys_clear_wake()` before checking
  work, and sleeps with PRIMASK set so a wake between check and WFI is not lost. FPU state is cleared
  before sleeping (nRF52 errata 87).
* **Timebase**: RTC2 at 32768 Hz with overflow extension; `millis()` is derived from it.
* **Watchdog**: 60 s, fed by the main loop. Pitfall: the nRF52 WDT keeps running through a soft reset
  and DFU and forces the LF RC oscillator on; LFCLKSTAT then reports a running clock while the RTC gets
  no ticks. Always trigger `LFCLKSTART` at boot and verify that the RTC actually counts.
  A firmware flashed after this one inherits the running WDT (stock gets one reset after ~60 s).
* **USB**: TinyUSB CDC; the banner is printed 300 ms after DTR and pending input is discarded, because
  the host tty may echo for a moment after opening. Writes are non-blocking. A 1200-baud touch reboots
  into the bootloader.
* **Identity / RNG**: hardware RNG for key generation and seeds.

## 5. Radio (`hal/sx1262.c`, `repeater/LoRaRadio.cpp`)

### 5.1 Chip setup
Mirrors RadioLib's SX1262 configuration so behaviour matches stock: private sync word (0x12 ->
registers 0x1424), explicit header, CRC on, preamble 32 at SF <= 8 (16 above), PA table and OCP as
RadioLib, datasheet errata (0x0889 modulation quality, 0x08D8 TX clamp, 0x0736 IQ polarity),
boosted RX gain (0x08AC = 0x96, kept over warm sleep via the register retention list), DC-DC regulator,
DIO2 as RF switch, TCXO on DIO3 with a 1000 us start budget (the TCXO used starts in ~0.3 ms; an
`XOSC_START_ERR` flag after each warm start is expected and harmless).

The module's RXEN input must be driven (high except during TX). On the tested module it feeds the
RF switch control through a resistor and is otherwise only weakly pulled; left floating, sensitivity
dropped by several dB and reception was 7-20 points below stock.

### 5.2 Interrupts and supervision
* DIO1 carries only terminal events: RX_DONE, CRC error, header error, timeout (and TX_DONE / CAD_DONE
  when used). PreambleDetected / HeaderValid are latched in the IRQ register and **read only**
  (`sx_peek_irq`, which never wakes a sleeping chip). Clearing them mid-packet costs packets.
* Supervision in `LoRaRadio::loop()`: a preamble without a header within (longest preamble in use +
  16) symbols is declared false and the receiver restarted; a header without RX_DONE within the
  maximum packet airtime is declared lost.
* **Always refresh the flags right before any decision** (false preamble, noise-floor sample, CAD
  probe, recalibration, health check). An earlier version decided on flags read up to 250 ms before
  and restarted the receiver in the middle of ~30 % of the packets that happened to be observed between
  preamble and header. Metrics that count a packet as received if any copy arrived hid this almost
  completely; it showed up as lost single-copy direct requests.

### 5.3 Sniff mode (RxDutyCycle)
The chip alternates an RX window and warm sleep by itself, with "stop timer on preamble" set, so a
detected preamble keeps the receiver on and the MCU supervises it.

Measured preamble detection latency (lab firmware, `swin` mode, `tools/analyze_swin.py`):
* preamble already on air when the window opens: ~3.6 symbols for strong signals, 5.6-11.6 symbols
  around -5..-10 dB SNR;
* preamble starting inside an open window: 2.5-3.5 symbols;
* a window shorter than ~4 symbols catches almost nothing.

Timing model (`LoRaRadio::computeTiming`), with `P` = shortest preamble to guarantee and `det` = window
length in symbols:

```
window = det * Tsym
sleep  = (P - det - 3.5) * Tsym - (TCXO start + ~1 ms context restore)
```

A preamble starting too late in one window still has `det` symbols left when the next window opens.
`tools/sniff_sim.py` simulates catch probability from measured latencies. With P = 16 at SF7/62.5 kHz:
det 5/6/7 -> radio on ~45/50/56 % -> ~2.2/2.7/3.1 mA, catch 0.93/0.97/0.985 (weak signals 0.965 at
det 7); P = 32 gives ~1.0 catch at much lower duty. Default: P = 16, det = 7.

On-air checks (node rotating receive configurations, counted by the node itself over ~4 h each):
det 7 received the same number of packets as continuous receive; det 6 and boosted gain off were a
few points lower in retransmission coverage.

### 5.4 Channel access
`isReceiving()` (asked by the Dispatcher before each TX) returns busy if a header was seen and the
packet is not finished, or a preamble was seen recently; otherwise it runs the optional RSSI threshold
check and CAD.

* **CAD threshold auto-tuning**: every 45-75 s (random) the node runs 4 CADs with inverted IQ (which do
  not respond to standard-IQ MeshCore chirps, but do respond to noise and interference) and 1 normal
  CAD. The inverted hit rate is the false-alarm rate of the current detPeak. Every 120 inverted probes
  (~30 min), or as soon as 6 false alarms accumulate, detPeak moves by one step within SF+13 +-3:
  > 2 % -> less sensitive, none -> more sensitive. After each inverted probe the IQ setting and the
  0x0736 errata bit are restored (getting this wrong silently degrades reception). Validation: the
  false-alarm rate of probes taken within 3 s after a reception ("hot") must match the rest; if it
  does not, inverted CAD is seeing traffic and the regulator would chase it.
* A transmission is held by CAD at most 3 times; retries are random 120..360 ms.
* Direct-routed packets (replies to requests, routed traffic) skip CAD: someone is waiting for them,
  and CAD on a busy mesh added seconds to reply latency. They still wait for a reception in progress.
* Measured: with CAD the share of our transmissions heard by a nearby observer was 0-10 points higher
  depending on traffic; reply latency p90 ~1.3 s.

### 5.5 Maintenance
* Noise floor: 16 RSSI samples every 60 s (15 s with an interference threshold), MeshCore's filter.
* Health check every 10 min: chip status/mode and device errors; re-initialises the chip if wrong
  (`lp stats` `fix`; seen about once a day, cause not yet identified - the event is logged with status
  and error bits at `lp debug 1`).
* Full recalibration every `lp.recal` minutes (default 60): warm sleep, Calibrate(0x7F), chip setup,
  full reconfiguration (like stock's AGC reset).
* AGC reset (`agc.reset.interval`): warm sleep + restart, continuous mode only, skipped during a
  reception (flags re-read first).

## 6. MeshCore integration

* `src/compat` provides just enough Arduino / Adafruit API for the unmodified MeshCore sources.
* LP settings live in `/lp_prefs` (magic + `LoRaRadio::LpConfig`). Fields are only ever appended;
  shorter files from older builds load their prefix and keep defaults for the rest.
* Remote CLI replies must fit 160 characters (the reply buffer is 166 bytes with a 5-byte header);
  use `snprintf(reply, 160, ...)`.
* Battery: SAADC, internal 0.6 V reference, gain 1/6, 12 bit, stock-compatible multiplier semantics;
  offset calibration at boot. `lp bat` compares 3/10/40 us acquisition times (a high-impedance divider
  without capacitor reads low at 3 us) and shows VDDH/5 and VDD.
* OTA: no BLE stack in the firmware. `start ota ble` sets GPREGRET = 0xA8 (bootloader BLE OTA, no
  timeout, the bootloader keeps feeding our WDT); `start ota usb` uses 0x57.

## 7. Power

Expected: radio ~3.1 mA in sniff (P 16, det 7), ~5.3 mA continuous, MCU asleep ~99 %, plus board
losses (regulator, divider, module). Not yet confirmed by a direct current measurement. Measure on
battery with USB disconnected (USB keeps the USB peripheral and charger path active). `lp stats`
`mcusleep` and the sniff duty in `lp` are the first things to check if the current is higher.

## 8. Test methodology

Roles (any comparable equipment works):

| Role | What it is | Used for |
|---|---|---|
| DUT | the node under test, on USB to the test host | node-side log (`log start`, `lp debug`), flashing, CLI |
| Local companion | a MeshCore companion radio on USB to the same host, near the DUT | (a) observer: the companion RX-log push gives every received packet raw; (b) prober: logs in to the DUT over a fixed zero-hop path and sends status / CLI requests at random intervals |
| Remote observers | repeaters publishing received packets to MQTT | wider view of forwarding and reception |

Metrics:
* **Per copy** (`copy_capture.py`, `local_obs.py`): a copy is (packet hash, path); the DUT should receive
  each copy an observer received from a sender the DUT can hear. This is the metric that exposes
  single-copy losses. Exclude the DUT's own transmissions and senders it never hears.
* **Any copy** (`unique_capture.py`): packet counted as received if any copy arrived. Hides most
  receiver bugs on a mesh with many repeaters; use only as a secondary metric.
* **Forwarding** (`local_obs.py`, `prod_report.py`): floods an observer heard, later heard with the
  DUT's hop in the path. Biased low when the observer is itself busy (half duplex, collisions).
* **Responsiveness** (`companion_probe.py` + `probe_report.py`): share of requests answered within
  10 s and latency, by test arm. This is the closest to user experience.
* **Node counters**: `lp stats` deltas within an arm; `lp exp` keeps exact per-arm counts on the node.

Running comparisons:
* Same firmware, different settings: rotate settings over USB (`monitor.py --rotate "name=cmd;cmd"`),
  10-minute arms, a few hours. Arm names are written to the log as `ARM <name>` lines.
* Different firmware: `fw_ab.py` (flash, configure, log, repeat) with **long** blocks. Reflashing
  resets the DUT's client list (ACL), so session-based requests fail until clients log in again -
  the prober re-logs in after two misses, and the first minutes after each flash are excluded.
* Interleave arms in time; traffic changes a lot between hours, absolute numbers from different
  runs are not comparable.
* Arm schedules reconstructed from the command time drift (the node postpones arm switches while
  receiving); prefer the node's own per-arm counters or ARM lines in the log.
* Request losses were almost entirely collisions with bursts of retransmissions of the same flood by
  several repeaters; they depend on site and traffic far more than on receive mode.

Safety and privacy in tooling:
* Test scripts only touch serial devices with the DUT's USB VID:PID (`fw_ab.py TEST_IDS`) or the
  configured companion serial number. Never select "any Adafruit device": other boards on the host
  would be reset into their bootloader.
* Site data (keys, passwords, device serials, observer names, radio parameters) lives in
  `tools/local.json`, `.secrets/` and `config.mk`, all ignored by git. Private keys are never printed;
  `node_decode.py` masks password and key arguments.

Tools (all take `-h`-style usage from their docstring):

| Script | Purpose |
|---|---|
| `dfu.py`, `term.py` | flash, interactive terminal |
| `monitor.py` | timestamped node log with init / periodic / rotating commands |
| `fw_ab.py` | interleaved firmware comparison (flash, configure, log) |
| `companion_probe.py` | local companion as observer + autonomous prober |
| `node_decode.py` | decrypt the DUT's own client traffic from an observer log with the DUT's key |
| `mk_status_req.py` | build fresh encrypted status / CLI request packets (for raw-packet tooling) |
| `copy_capture.py`, `unique_capture.py`, `local_obs.py`, `prod_report.py`, `resp_report.py`, `probe_report.py`, `req_resp.py` | metrics above |
| `sniff_sim.py`, `analyze_swin.py` | sniff timing model / detection latency analysis (lab firmware logs) |
| `mesh_pkt.py` | shared packet parsing (MeshCore packet hash, path) |

## 9. Adapting

Checklist for another board, module or mesh:
1. `src/hal/board.h`: SPI, BUSY, DIO1, RESET, RXEN/TXEN, power-enable pins, LED, battery input.
2. TCXO voltage code and start-up time (`SX126X_TCXO_VOLTAGE_CODE`, `set lp.tcxo`); modules without
   TCXO are detected at init and fall back to the crystal.
3. RF switch: DIO2 control and any external enable lines - drive them, do not leave them floating.
4. Radio parameters: `config.mk`. At other SF/BW, re-derive the sniff timing (symbol time changes) and
   re-run `sniff_sim.py`; at SF > 8 MeshCore uses 16-symbol preambles.
5. Neighbour firmware mix: if nodes with 8-symbol preambles remain, sniff cannot guarantee them.
6. Validate with per-copy capture and request probes against continuous receive before trusting the
   defaults.

## 10. Open items

* Direct battery current measurement (sniff vs continuous) and VDDH-based battery reading.
* Root cause of the rare health-check re-initialisation.
* Optional listen-before-talk in sniff before every TX (closes the ~10-25 ms blind window before a
  transmission); not needed so far given the measurements.
* Low-voltage shutdown (SYSTEMOFF + comparator wake) and a clock that survives soft resets.
