#!/usr/bin/env python3
"""Long-running console logger for the repeater.

  monitor.py --log FILE [--init "cmd"]... [--every SECONDS --periodic "cmd"]... [--duration SECONDS]

Keeps the CDC port open (reconnects if the device reboots), prefixes every line with host time,
sends --init commands after each (re)connect and --periodic commands every --every seconds.
"""
import argparse
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
    ap.add_argument("--log", required=True)
    ap.add_argument("--init", action="append", default=[])
    ap.add_argument("--periodic", action="append", default=[])
    ap.add_argument("--every", type=float, default=600)
    ap.add_argument("--duration", type=float, default=0)
    ap.add_argument("--vidpid", help="hex VID:PID of the device, default 239A:8029")
    ap.add_argument("--rotate", action="append", default=[], help="NAME=command, cycled every --rotate-every s")
    ap.add_argument("--rotate-every", type=float, default=60)
    a = ap.parse_args()
    if a.vidpid:
        v, p = a.vidpid.split(":")
        APP_IDS.clear()
        APP_IDS.add((int(v, 16), int(p, 16)))
    rot = [r.split("=", 1) for r in a.rotate]
    rot_i, next_rot = 0, 0.0

    t_end = time.time() + a.duration if a.duration else None
    log = open(a.log, "a", buffering=1)

    def w(line):
        log.write(time.strftime("%Y-%m-%d %H:%M:%S ") + line + "\n")

    while t_end is None or time.time() < t_end:
        port = find_port()
        if not port:
            time.sleep(1)
            continue
        try:
            s = serial.Serial(port, 115200, timeout=0.2)
            s.dtr = True
            w(f"# connected {port}")
            time.sleep(1.0)
            buf = b""
            sent_init = False
            next_periodic = time.time() + a.every
            while t_end is None or time.time() < t_end:
                d = s.read(4096)
                if d:
                    buf += d
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        line = line.decode("utf-8", "replace").rstrip("\r")
                        if line:
                            w(line)
                if not sent_init and time.time() > 0:
                    time.sleep(0.5)
                    for c in a.init:
                        s.write((c + "\r").encode())
                        time.sleep(0.5)
                    sent_init = True
                if rot and sent_init and time.time() >= next_rot:
                    name, cmd = rot[rot_i % len(rot)]
                    for c in cmd.split(";"):            # several commands per arm: 'a;b'
                        s.write((c.strip() + "\r").encode())
                        time.sleep(0.4)
                    w(f"ARM {name}")
                    rot_i += 1
                    next_rot = time.time() + a.rotate_every
                if a.periodic and time.time() >= next_periodic:
                    for c in a.periodic:
                        s.write((c + "\r").encode())
                        time.sleep(0.5)
                    next_periodic = time.time() + a.every
        except (serial.SerialException, OSError) as e:
            w(f"# disconnected: {e}")
            time.sleep(2)


if __name__ == "__main__":
    main()
