#!/usr/bin/env python3
"""Interleaved comparison: our firmware vs stock MeshCore, same node identity and settings.

  fw_ab.py --log LOG --cycles N --block SECONDS --ours ZIP --stock ZIP --key FILE --pw FILE

Each block: DFU flash -> configure (imported key, password, 10 dBm, repeat off) -> reboot ->
log for BLOCK seconds while rotating receive modes every 60 s ('ARM <name>' lines).
"""
import argparse
import os
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

VENV_BIN = os.path.dirname(sys.executable)
OURS_ID, STOCK_ID, BOOT_ID = (0x239A, 0x8029), (0x239A, 0x00B3), (0x239A, 0x00B3)

ROT = {
    "ours": [("lp_cont", ["set lp.rearm off", "set lp.rx cont"]),
             ("lp_sniff", ["set lp.rearm off", "set lp.rx sniff"])],
    "stock": [("st_psoff", ["powersaving off"]), ("st_pson", ["powersaving on"])],
    "lab": [("lab_cont", [])],
}


def ports(ids):
    return [p.device for p in list_ports.comports() if (p.vid, p.pid) == ids]


# Only the test board: our app (8029), stock app / nice!nano bootloader (00B3). Other Adafruit-VID
# devices on the same host (e.g. a companion radio used as observer) must never be touched.
TEST_IDS = {OURS_ID, STOCK_ID, BOOT_ID, (0x239A, 0x0029)}


def any_device():
    return [p.device for p in list_ports.comports() if (p.vid, p.pid) in TEST_IDS]


def wait_for(pred, timeout):
    t = time.time() + timeout
    while time.time() < t:
        r = pred()
        if r:
            return r
        time.sleep(0.3)
    return None


LOG = None


class Log:
    def __init__(self, path):
        self.f = open(path, "a", buffering=1)

    def w(self, line):
        self.f.write(time.strftime("%Y-%m-%d %H:%M:%S ") + line + "\n")


def flash(zip_path, log):
    dev = any_device()
    if dev:
        try:
            s = serial.Serial(dev[0], 1200)
            s.dtr = False
            time.sleep(0.2)
            s.close()
        except (serial.SerialException, OSError):
            pass
        wait_for(lambda: not any_device(), 8)          # app goes away
    boot = wait_for(lambda: ports(BOOT_ID), 20)
    if not boot:
        log.w("# bootloader not found")
        return False
    for attempt in range(3):
        time.sleep(4)
        boot = ports(BOOT_ID)
        if not boot:
            return False
        r = subprocess.run([os.path.join(VENV_BIN, "adafruit-nrfutil"), "dfu", "serial", "-pkg", zip_path,
                            "-p", boot[0], "-b", "115200", "--singlebank"], capture_output=True, text=True)
        if r.returncode == 0:
            log.w(f"# flashed {os.path.basename(zip_path)}")
            return True
        log.w(f"# dfu attempt {attempt + 1} failed")
    return False


def open_app(ids, timeout=40):
    dev = wait_for(lambda: ports(ids), timeout)
    if not dev:
        return None
    time.sleep(2)
    s = serial.Serial(dev[0], 115200, timeout=0.2)
    s.dtr = True
    time.sleep(1.0)
    banner = s.read(65536).decode("utf-8", "replace")
    for line in banner.splitlines():
        if "reset:" in line:
            LOG.w("# " + line.strip())
    return s


def send(s, cmd, log, wait=1.0, hide=None):
    s.write((cmd + "\r").encode())
    end = time.time() + wait
    buf = b""
    while time.time() < end:
        try:
            buf += s.read(4096)
        except (serial.SerialException, OSError):
            break                      # e.g. 'reboot' drops the port
    txt = buf.decode("utf-8", "replace")
    for secret in (hide or []):
        txt = txt.replace(secret, "<secret>")
        cmd = cmd.replace(secret, "<secret>")
    log.w(f"> {cmd} :: " + " | ".join(x.strip() for x in txt.splitlines() if "->" in x))


SETUP = {}      # kind -> list of commands after key/password (default: 10 dBm, repeat off)
NO_ROTATE = False


def configure(kind, key, pw, log):
    ids = STOCK_ID if kind == "stock" else OURS_ID
    if kind == "lab":                  # receive-only lab firmware: nothing to configure
        return wait_for(lambda: ports(ids), 90) is not None
    s = open_app(ids, timeout=90)     # a boot hang would be cleared by the 60 s watchdog
    if not s:
        log.w("# app port not found")
        return False
    extra = SETUP.get(kind, ["set tx 10", "set repeat off"])
    for c in [f"set prv.key {key}", f"password {pw}"] + extra:
        send(s, c, log, hide=[key, pw] + [c.split(" ", 2)[2] for c in extra if c.startswith("set guest.password ")])
    send(s, "reboot", log, wait=0.5)
    try:
        s.close()
    except (serial.SerialException, OSError):
        pass
    wait_for(lambda: not any_device(), 8)
    return True


def run_block(kind, seconds, log):
    ids = STOCK_ID if kind == "stock" else OURS_ID
    s = open_app(ids)
    if not s:
        log.w("# app port not found for block")
        return
    if kind == "ours":
        send(s, "log start", log)
    if kind == "lab":
        for c in ("tcxo 1000", "raw 1", "cont"):
            send(s, c, log)
    else:
        send(s, "get public.key", log)
    rot, i = ([(kind + "_prod", [])] if NO_ROTATE else ROT[kind]), 0
    end = time.time() + seconds
    next_rot = 0
    buf = b""
    while time.time() < end:
        if time.time() >= next_rot:
            name, cmds = rot[i % len(rot)]
            for c in cmds:
                s.write((c + "\r").encode())
                time.sleep(0.3)
            log.w(f"ARM {name}")
            i += 1
            next_rot = time.time() + 60
        try:
            buf += s.read(4096)
        except (serial.SerialException, OSError):
            # e.g. stock fw gets one watchdog reset (our WDT survives the DFU): wait and continue
            log.w("# lost port during block, waiting for the device")
            try:
                s.close()
            except (serial.SerialException, OSError):
                pass
            s = open_app(ids, timeout=90)
            if not s:
                log.w("# device did not come back")
                return
            log.w("# reconnected")
            if kind == "ours":
                send(s, "log start", log)
            continue
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.decode("utf-8", "replace").rstrip("\r")
            if line:
                log.w(line)
    s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--cycles", type=int, default=4)
    ap.add_argument("--block", type=float, default=1200)
    ap.add_argument("--ours", required=True)
    ap.add_argument("--stock", required=True)
    ap.add_argument("--lab")
    ap.add_argument("--key", required=True)
    ap.add_argument("--pw", required=True)
    ap.add_argument("--setup-ours", help="file: CLI commands applied after key/password (replaces 10 dBm + repeat off)")
    ap.add_argument("--setup-stock")
    ap.add_argument("--no-rotate", action="store_true", help="one arm per block (<kind>_prod), no mode rotation")
    a = ap.parse_args()
    key = open(a.key).read().strip()
    pw = open(a.pw).read().strip()
    global LOG, NO_ROTATE
    NO_ROTATE = a.no_rotate
    for kind, f in (("ours", a.setup_ours), ("stock", a.setup_stock)):
        if f:
            SETUP[kind] = [x.strip() for x in open(f) if x.strip() and not x.startswith("#")]
    log = LOG = Log(a.log)
    for c in range(a.cycles):
        kinds = [("ours", a.ours)] + ([("lab", a.lab)] if a.lab else []) + [("stock", a.stock)]
        for kind, z in kinds:
            log.w(f"# cycle {c} {kind}")
            ok = False
            for attempt in range(2):
                if flash(z, log) and configure(kind, key, pw, log):
                    ok = True
                    break
                log.w("# setup failed" + (", retrying" if attempt == 0 else ", skipping block"))
            if not ok:
                continue
            run_block(kind, a.block, log)


if __name__ == "__main__":
    main()
