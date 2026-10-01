#!/usr/bin/env python3
"""Summarise an A/B receive log produced by monitor.py + 'log start' + 'lp ab N'.

Per mode: packets, rate/hour, SNR distribution; per last-hop repeater: counts per mode;
for continuous-mode packets: estimated preamble symbols seen before detection
(d = P + 12.25 - p2h/Tsym), i.e. how fast the SX1262 flags a preamble.
"""
import re
import sys
from collections import defaultdict

TSYM_MS = 2.048   # SF7 / BW62.5
PRE = 32

rx_re = re.compile(r"RX, len=(\d+) \(type=(\d+), route=(\w), payload_len=(\d+)\) SNR=(-?\d+) RSSI=(-?\d+) "
                   r"score=(-?\d+) mode=(\w+) p2h=(-?\d+) path=([0-9A-F]*)")
ab_re = re.compile(r"ab=\w+ cont: (\d+)min ok=(\d+) err=(\d+) \| sniff: (\d+)min ok=(\d+) err=(\d+)")
exp_re = re.compile(r"> exp=\S+((?: \w+:\d+/\d+/\d+m)+)")


def main(path, hash_size=2):
    per_mode = defaultdict(list)
    per_hop = defaultdict(lambda: defaultdict(int))
    hop_p2h = defaultdict(list)
    last_ab = None
    last_exp = None
    for line in open(path, errors="replace"):
        m = rx_re.search(line)
        if m:
            ln, typ, route, plen, snr, rssi, score, mode, p2h, path_hex = m.groups()
            snr, rssi, p2h = int(snr), int(rssi), int(p2h)
            per_mode[mode].append((snr, rssi, p2h, int(typ), route))
            hop = path_hex[-2 * hash_size:] if path_hex else "direct"
            per_hop[hop][mode] += 1
            if mode == "cont" and p2h > 0:
                hop_p2h[hop].append(p2h)
            continue
        m = ab_re.search(line)
        if m:
            last_ab = tuple(int(x) for x in m.groups())
        m = exp_re.search(line)
        if m:
            last_exp = m.group(1)

    if last_ab:
        cm, cok, cerr, sm, sok, serr = last_ab
        print(f"device counters: cont {cm} min ok={cok} err={cerr} ({cok * 60 / max(cm, 1):.1f}/h) | "
              f"sniff {sm} min ok={sok} err={serr} ({sok * 60 / max(sm, 1):.1f}/h)")
        if cok and cm and sm:
            ratio = (sok / sm) / (cok / cm)
            print(f"sniff/cont rate ratio: {ratio:.3f}")
    if last_exp:
        arms = [x.split(":") for x in last_exp.split()]
        base = None
        print("device counters (ok/err/minutes, rate per hour, relative to cont):")
        for name, v in arms:
            ok, err, mins = v.rstrip("m").split("/")
            ok, err, mins = int(ok), int(err), int(mins)
            rate = ok * 60 / mins if mins else 0
            if name == "cont":
                base = rate
            rel = f"{rate / base:.2f}" if base else "-"
            print(f"   {name:6s} ok={ok:4d} err={err:3d} {mins:4d} min  {rate:6.1f}/h  rel={rel}")
    for mode, pk in per_mode.items():
        snrs = sorted(p[0] for p in pk)
        print(f"{mode}: {len(pk)} logged pkts, SNR min/med/max {snrs[0]}/{snrs[len(snrs)//2]}/{snrs[-1]}")
        weak = [p for p in pk if p[0] < 0]
        print(f"   weak (SNR<0): {len(weak)}")
        d = [PRE + 12.25 - p[2] / TSYM_MS for p in pk if p[2] > 0]
        if mode != "cont" and d:
            print(f"   sniff: preamble symbol at which the window caught it: min {min(d):.1f} max {max(d):.1f}")
        if mode == "cont" and d:
            d.sort()
            print(f"   p2h-derived detect symbols: min {d[0]:.1f} med {d[len(d)//2]:.1f} max {d[-1]:.1f}  (n={len(d)})")
            short = [p for p in pk if 0 < p[2] < (16 + 10) * TSYM_MS]
            print(f"   packets with p2h typical of a 16-symbol preamble: {len(short)}")
    print("last hop -> counts per mode, sender preamble estimate from continuous-mode p2h:")
    for hop, c in sorted(per_hop.items(), key=lambda kv: -sum(kv[1].values())):
        est = ""
        if hop_p2h[hop]:
            # detection normally happens ~2-3 symbols into the preamble
            pre = sorted(round(x / TSYM_MS - 12.25 + 2.5) for x in hop_p2h[hop])
            est = f" preamble~{pre}"
        cols = " ".join(f"{k}={v}" for k, v in sorted(c.items()))
        print(f"   {hop:>8}: {cols}{est}")


if __name__ == "__main__":
    main(sys.argv[1])
