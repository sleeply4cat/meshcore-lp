#!/usr/bin/env python3
"""Tiny non-interactive serial helper for the CDC console.

  term.py [--port P] [--wait S] [--cmd "text"]... [--listen S]
Waits for the device port (app VID/PID), sends each --cmd line, and prints output for --listen seconds.
Never touches 1200 baud, opens with DTR asserted (required for our CDC to talk).
"""
import argparse
import sys
import time

import serial
from serial.tools import list_ports

APP_IDS = {(0x239A, 0x8029)}


def find_port():
    for p in list_ports.comports():
        if (p.vid, p.pid) in APP_IDS:
            return p.device
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--wait", type=float, default=15)
    ap.add_argument("--cmd", action="append", default=[])
    ap.add_argument("--listen", type=float, default=3)
    ap.add_argument("--log")
    a = ap.parse_args()

    port = a.port
    t0 = time.time()
    while not port and time.time() - t0 < a.wait:
        port = find_port()
        if not port:
            time.sleep(0.2)
    if not port:
        sys.exit("device port not found")

    s = serial.Serial(port, 115200, timeout=0.1)
    s.dtr = True
    log = open(a.log, "ab") if a.log else None
    time.sleep(0.3)

    def pump(duration):
        end = time.time() + duration
        while time.time() < end:
            d = s.read(4096)
            if d:
                sys.stdout.write(d.decode("utf-8", "replace"))
                sys.stdout.flush()
                if log:
                    log.write(d)
                    log.flush()

    pump(0.7)
    for c in a.cmd:
        s.write((c + "\r").encode())
        pump(1.0)
    pump(a.listen)
    s.close()


if __name__ == "__main__":
    main()
