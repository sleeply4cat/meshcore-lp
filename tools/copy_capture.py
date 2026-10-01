#!/usr/bin/env python3
"""Per-arm capture of individual packet copies seen by MQTT observers.

  copy_capture.py MQTT OUR_LOG [MIN_OBSERVER_SNR] [NODE_HASH_HEX]

Fair comparison: copies transmitted by the node itself (last hop = NODE) are skipped, and so are
copies from senders the node never heard in any arm (out of its range, not a firmware matter).

A copy is (packet hash, path): every retransmission is a separate reception chance. Unlike
unique_capture.py (any copy counts), losing a single copy is visible here - which is what a
direct packet (one copy only) experiences. Works with our log format and stock RAW: lines.
"""
import bisect
import json
import math
import re
import sys
from collections import defaultdict
from datetime import datetime

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mesh_pkt as b  # noqa: E402

RAW_RE = re.compile(r"^(\S+ \S+) .*RAW: ([0-9A-F]+)")
OURS_RE = re.compile(r"^(\S+ \S+) .*RX, len=.* path=([0-9A-F]*) hash=([0-9A-F]{16})")


def ts(s):
    return datetime.strptime(s, "%Y-%m-%d %H:%M:%S").timestamp()


def main(mqtt, log, min_snr, node=""):
    arms, ours = [], defaultdict(list)
    for l in open(log, errors="replace"):
        m = b.arm_re.match(l)
        if m:
            arms.append((ts(m[1]), m[2]))
            continue
        if l[20:28] == "# cycle ":
            arms.append((ts(l[:19]), "gap"))
            continue
        m = OURS_RE.match(l)
        if m:
            ours[(m[3][:16], m[2])].append(ts(m[1]))
            continue
        m = RAW_RE.match(l)
        if m:
            try:
                route, ptype, hs, path, h = b.parse(m[2])
            except (ValueError, IndexError):
                continue
            ours[(h[:16], path.hex().upper())].append(ts(m[1]))
    arm_t = [a[0] for a in arms]
    slot = sorted(y - x for x, y in zip(arm_t, arm_t[1:]))[len(arm_t) // 2 - 1] if len(arm_t) > 2 else 60
    t_end = arm_t[-1] + slot
    rows = []
    seen = set()
    for l in open(mqtt, errors="replace"):
        if "/packets " not in l:
            continue
        try:
            j = json.loads(l.split(" ", 2)[2])
            obs = j.get("origin")
            if obs is None or "raw" not in j or float(j.get("SNR", -99)) < min_snr:
                continue
            route, ptype, hs, path, h = b.parse(j["raw"])
            t = datetime.fromisoformat(j["timestamp"]).timestamp()
        except (ValueError, IndexError, KeyError, json.JSONDecodeError):
            continue
        key = (h[:16], path.hex().upper())
        if (obs, key) in seen:
            continue
        seen.add((obs, key))
        i = bisect.bisect_right(arm_t, t) - 1
        if i < 0 or t - arm_t[i] < 4 or (i + 1 < len(arm_t) and arm_t[i + 1] - t < 4) or t > t_end - 4:
            continue
        a = arms[i][1]
        if a == "gap":
            continue
        last = path[-hs:].hex().upper() if path else "-"
        if node and last == node[:2 * hs]:
            continue
        rows.append((obs, a, last, any(abs(x - t) < 20 for x in ours.get(key, []))))
    heard = {(o, last) for o, a, last, got in rows if got}
    res = defaultdict(lambda: [0, 0])
    for o, a, last, got in rows:
        if (o, last) in heard:
            res[(o, a)][0] += 1
            res[(o, a)][1] += got
    for obs in sorted({o for o, _ in res}):
        out = []
        for (o, a), (n, c) in sorted(res.items()):
            if o != obs:
                continue
            p = c / max(n, 1)
            out.append(f"{a}={c}/{n}={p:.2f}±{math.sqrt(p * (1 - p) / max(n, 1)):.2f}")
        print(f"{obs:6s} copies(SNR>={min_snr:g}) " + "  ".join(out))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else -99,
         sys.argv[4].upper() if len(sys.argv) > 4 else "")
