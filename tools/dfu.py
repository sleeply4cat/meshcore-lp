#!/usr/bin/env python3
"""Flash a DFU zip through the Adafruit nRF52 bootloader (serial DFU).

Same procedure as PlatformIO's nordicnrf52 'nrfutil' upload:
  1200 baud touch (if the application is running) -> wait for bootloader port -> adafruit-nrfutil dfu serial.
"""
import os
import shutil
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

BOOT_IDS = {(0x239A, 0x00B3)}                    # nice!nano / ProMicro bootloader
APP_IDS = {(0x239A, 0x8029), (0x239A, 0x0029)}   # our firmware / Arduino app


def find(ids):
    for p in list_ports.comports():
        if (p.vid, p.pid) in ids:
            return p.device
    return None


def main():
    port, pkg = sys.argv[1], sys.argv[2]
    boot = find(BOOT_IDS)
    if not boot:
        app = find(APP_IDS) or port
        print(f"touching {app} at 1200 baud")
        try:
            s = serial.Serial(app, 1200)
            s.dtr = False
            time.sleep(0.1)
            s.close()
        except (serial.SerialException, OSError) as e:
            print("touch:", e)
        for _ in range(100):
            time.sleep(0.1)
            boot = find(BOOT_IDS)
            if boot:
                break
    if not boot:
        sys.exit("bootloader port not found")
    nrfutil = os.path.join(os.path.dirname(sys.executable), "adafruit-nrfutil")
    if not os.path.exists(nrfutil):                 # not in a venv next to this Python: use PATH
        nrfutil = shutil.which("adafruit-nrfutil") or "adafruit-nrfutil"
    for attempt in range(3):
        # give ModemManager & co. time to finish probing the freshly enumerated port
        time.sleep(4)
        boot = find(BOOT_IDS)
        if not boot:
            sys.exit("bootloader port disappeared")
        print(f"bootloader on {boot}, flashing {pkg} (attempt {attempt + 1})")
        r = subprocess.run([nrfutil, "dfu", "serial", "-pkg", pkg, "-p", boot, "-b", "115200", "--singlebank"])
        if r.returncode == 0:
            sys.exit(0)
    sys.exit(1)


if __name__ == "__main__":
    main()
