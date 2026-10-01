#!/usr/bin/env python3
"""Detection latency from RX-window opening, measured by the lab 'swin' mode.

For each received packet the preamble start is reconstructed from the HeaderValid time:
  preamble_start = t_hdr - (P + 12.25) * Tsym,   P = 16 for known old-firmware last hops, else 32.
If the preamble was already on air when the window opened, 'pre' is the cold-start detection time D.
"""
import re
import sys
from collections import defaultdict

TSYM = 2048.0  # us, SF7 BW62.5
OLD = set(sys.argv[2].split(",")) if len(sys.argv) > 2 else set()

line_re = re.compile(r"SW wake=(\d+) pre=(-?\d+) hdr=(-?\d+) done=\d+ rssi=(-?\d+) snr=(-?\d+) len=(\d+) (\w+) ([0-9A-F]*)")


def last_hop(raw):
    b = bytes.fromhex(raw)
    if len(b) < 2:
        return None
    hdr = b[0]
    route = hdr & 3
    i = 1 + (4 if route in (0, 3) else 0)
    if i >= len(b):
        return None
    pl = b[i]
    hs = (pl >> 6) + 1
    cnt = pl & 63
    if cnt == 0:
        return "zero"
    end = i + 1 + cnt * hs
    if end > len(b):
        return None
    return b[end - hs:end].hex().upper()


def main(path):
    rows = []
    for l in open(path, errors="replace"):
        m = line_re.search(l)
        if not m:
            continue
        wake, pre, hdr, rssi, snr, ln, st, raw = m.groups()
        pre, hdr, rssi, snr = int(pre), int(hdr), int(rssi), int(snr)
        if pre < 0 or hdr < 0:
            continue
        hop = last_hop(raw) if raw else None
        if hop is None or hop == "zero":
            continue
        P = 16 if hop in OLD else 32
        start = hdr - (P + 12.25) * TSYM
        rows.append((snr, rssi, pre, start, hop, P, st))
    cold = defaultdict(list)
    warm = defaultdict(list)
    for snr, rssi, pre, start, hop, P, st in rows:
        b = "SNR>=5" if snr >= 5 else ("0..4" if snr >= 0 else ("-5..-1" if snr >= -5 else "<-5"))
        if start < -2 * TSYM:            # preamble clearly on air before the window was ready
            cold[b].append(pre / TSYM)
        elif start > 0:
            warm[b].append((pre - start) / TSYM)
    print(f"{len(rows)} packets with known sender preamble")
    for name, d in (("cold start (preamble already on air)", cold), ("preamble started inside window", warm)):
        print(name + ": detection time in symbols")
        for b in ("SNR>=5", "0..4", "-5..-1", "<-5"):
            v = sorted(d.get(b, []))
            if v:
                p90 = v[min(len(v) - 1, int(len(v) * 0.9))]
                print(f"   {b:7s} n={len(v):3d} min {v[0]:5.1f} med {v[len(v)//2]:5.1f} p90 {p90:5.1f} max {v[-1]:5.1f}")


if __name__ == "__main__":
    main(sys.argv[1])
