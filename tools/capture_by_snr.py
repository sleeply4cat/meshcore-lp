#!/usr/bin/env python3
"""Capture ratio of unique observer packets binned by the observer's SNR: capture_by_snr.py MQTT LOG [LOG...]"""
import bisect, json, sys
from collections import defaultdict
from datetime import datetime
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mesh_pkt as b  # noqa: E402

def load(log):
    arms, ours = [], defaultdict(list)
    for l in open(log, errors="replace"):
        m = b.arm_re.match(l)
        if m:
            arms.append((datetime.strptime(m[1], "%Y-%m-%d %H:%M:%S").timestamp(), m[2])); continue
        m = b.RX_REPEATER_RE.match(l)
        if m:
            ours[m[4][:16]].append(datetime.strptime(m[1], "%Y-%m-%d %H:%M:%S").timestamp()); continue
        m = b.rx_re.match(l)
        if m:
            try: ours[b.parse(m[3])[4][:16]].append(datetime.strptime(m[1], "%Y-%m-%d %H:%M:%S").timestamp())
            except (ValueError, IndexError): pass
    return arms, ours

def main(mqtt, logs):
    bins = [(-99, -5), (-5, 0), (0, 5), (5, 10), (10, 99)]
    for log in logs:
        arms, ours = load(log)
        at = [a[0] for a in arms]
        t0, t1 = at[0], at[-1] + 60
        res = defaultdict(lambda: [0, 0]); seen = set()
        for l in open(mqtt, errors="replace"):
            if "/packets " not in l: continue
            ts, _, p = l.split(" ", 2)
            try:
                j = json.loads(p)
                if j.get("origin") != sys.argv[3]: continue
                key = b.parse(j["raw"])[4][:16]; snr = float(j.get("SNR") or 0)
            except Exception: continue
            if key in seen: continue
            seen.add(key)
            t = datetime.fromisoformat(ts).timestamp()
            if t < t0 + 4 or t > t1 - 4: continue
            i = bisect.bisect_right(at, t) - 1
            arm = arms[i][1]
            if arm not in ("cont", "ps_off", "ps_on", "s16d3"): continue
            for lo, hi in bins:
                if lo <= snr < hi:
                    k = (arm, lo)
                    res[k][0] += 1; res[k][1] += any(abs(x - t) < 20 for x in ours.get(key, []))
        print(log)
        for arm in sorted({k[0] for k in res}):
            print("  %-7s " % arm + "  ".join(f"[{lo},{hi}): {res[(arm,lo)][1]}/{res[(arm,lo)][0]}" for lo, hi in bins))

main(sys.argv[1], sys.argv[2:])
