#!/usr/bin/env python3
"""Monte-Carlo catch probability of SX1262 RxDutyCycle sniffing, using detection latencies
measured with the lab 'swin' mode (tools/analyze_swin.py log).

Model: windows of W symbols every T = W + S + O. A preamble of P symbols starts at a random phase.
 - already on air when a window opens -> detected D_cold symbols after the window opens
 - starts inside a window            -> detected D_warm symbols after it starts
Detection must happen inside the window and before the preamble ends (then the chip stops its timer).
Average radio current: I_rx*W/T + overhead.
"""
import random
import re
import sys

TSYM = 2.048  # ms
line_re = re.compile(r"SW wake=\d+ pre=(-?\d+) hdr=(-?\d+) done=\d+ rssi=(-?\d+) snr=(-?\d+) len=\d+ \w+ ([0-9A-F]*)")

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from analyze_swin import last_hop, OLD  # noqa: E402


def load(path, weak_only):
    cold, warm = [], []
    for l in open(path, errors="replace"):
        m = line_re.search(l)
        if not m:
            continue
        pre, hdr, rssi, snr, raw = int(m[1]), int(m[2]), int(m[3]), int(m[4]), m[5]
        if pre < 0 or hdr < 0 or not raw or (weak_only and snr >= 0):
            continue
        hop = last_hop(raw)
        if hop in (None, "zero"):
            continue
        P = 16 if hop in OLD else 32
        start = hdr / 1000.0 - (P + 12.25) * TSYM
        pre_ms = pre / 1000.0
        if start < -2 * TSYM:
            cold.append(pre_ms / TSYM)
        elif start > 0 and pre_ms - start > 0:
            warm.append((pre_ms - start) / TSYM)
    return cold, warm


def catch_prob(P, W, S_ms, O_ms, cold, warm, n=20000):
    T = W * TSYM + S_ms + O_ms
    ok = 0
    for _ in range(n):
        start = random.uniform(0, T)            # preamble start relative to window 0 opening
        end = start + P * TSYM
        caught = False
        k = 0
        while k * T < end and not caught:
            wo, wc = k * T, k * T + W * TSYM
            if start <= wo:                      # on air at opening
                det = wo + random.choice(cold) * TSYM
            elif start < wc:
                det = start + random.choice(warm) * TSYM
            else:
                k += 1
                continue
            if det <= wc and det <= end:
                caught = True
            k += 1
        ok += caught
    return ok / n


def current_ma(W, S_ms, O_ms, i_rx=5.3, i_oh=1.5):
    T = W * TSYM + S_ms + O_ms
    return (W * TSYM * i_rx + O_ms * i_oh) / T


if __name__ == "__main__":
    cold, warm = load(sys.argv[1], weak_only=len(sys.argv) > 2 and sys.argv[2] == "weak")
    print(f"samples: cold {len(cold)}, warm {len(warm)}")
    O = 2.0
    for P in (16, 32):
        print(f"--- sender preamble {P} symbols")
        best = []
        for W in (4, 5, 6, 7, 8, 9, 10, 12):
            for S in (2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 25, 30, 35, 40, 45, 50):
                p = catch_prob(P, W, S, O, cold, warm, 4000)
                best.append((p, current_ma(W, S, O), W, S))
        for target in (0.95, 0.99, 0.995):
            cand = [b for b in best if b[0] >= target]
            if cand:
                p, i, W, S = min(cand, key=lambda b: b[1])
                print(f"   catch>={target}: W={W} sym S={S} ms -> p={p:.4f}  I_radio~{i:.2f} mA")
            else:
                print(f"   catch>={target}: not reachable")
