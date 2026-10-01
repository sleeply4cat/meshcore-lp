#!/usr/bin/env python3
"""Capture ratio per receive configuration, using MeshCore MQTT observers as ground truth.

  mqtt_compare.py MQTT_DUMP OUR_LOG [--observer NAME]...

MQTT dump lines:  <iso-time> meshcore/<iata>/<id>/packets {json with "origin", "raw", "SNR", ...}
Our log (monitor.py): '<date> <time> ARM <name>' switch lines and '... RX <arm> ... : <RAWHEX>' lines.
A transmission counts as caught if our log has the identical raw packet within +-WINDOW s.
Observer packets within GUARD s of an arm switch are ignored (latency of the MQTT path).
"""
import bisect
import json
import re
import sys
from collections import defaultdict
from datetime import datetime

WINDOW = 20.0
GUARD = 4.0

arm_re = re.compile(r"^(\S+ \S+) ARM (\w+)")
rx_re = re.compile(r"^(\S+ \S+) .*RX (\w+) len=\d+ rssi=(-?\d+) snr=(-?\d+)\S* .* : ([0-9A-F]+)")


def ts_local(s):
    return datetime.strptime(s, "%Y-%m-%d %H:%M:%S").timestamp()


def main():
    mqtt_path, log_path = sys.argv[1], sys.argv[2]
    observers = set(a.split("=", 1)[1] for a in sys.argv[3:] if a.startswith("--observer=")) or None

    arms = []          # (t, name)
    ours = defaultdict(list)   # raw -> [(t, arm, rssi, snr)]
    for l in open(log_path, errors="replace"):
        m = arm_re.match(l)
        if m:
            arms.append((ts_local(m[1]), m[2]))
            continue
        m = rx_re.match(l)
        if m:
            ours[m[5]].append((ts_local(m[1]), m[2], int(m[3]), int(m[4])))
    if not arms:
        sys.exit("no ARM lines in our log")
    arm_t = [a[0] for a in arms]
    t_first, t_last = arms[0][0], max(max((x[0] for v in ours.values() for x in v), default=0), arms[-1][0])

    stats = defaultdict(lambda: defaultdict(lambda: [0, 0]))   # observer -> arm -> [seen, caught]
    by_snr = defaultdict(lambda: defaultdict(lambda: [0, 0]))
    for l in open(mqtt_path, errors="replace"):
        if "/packets " not in l:
            continue
        ts, topic, payload = l.split(" ", 2)
        try:
            j = json.loads(payload)
        except json.JSONDecodeError:
            continue
        if j.get("direction", "rx") != "rx" or "raw" not in j:
            continue
        origin = j.get("origin", "?")
        if observers and origin not in observers:
            continue
        t = datetime.fromisoformat(ts).timestamp()
        if t < t_first + GUARD or t > t_last:
            continue
        i = bisect.bisect_right(arm_t, t) - 1
        if i < 0:
            continue
        # skip packets close to an arm switch (MQTT latency could put them in the wrong slot)
        if t - arm_t[i] < GUARD or (i + 1 < len(arm_t) and arm_t[i + 1] - t < GUARD):
            continue
        arm = arms[i][1]
        raw = j["raw"].upper()
        caught = any(abs(x[0] - t) <= WINDOW for x in ours.get(raw, []))
        stats[origin][arm][0] += 1
        stats[origin][arm][1] += caught
        try:
            snr = float(j.get("SNR") or 0)
        except ValueError:
            snr = 0.0
        sb = "obsSNR>=5" if snr >= 5 else ("obsSNR 0..5" if snr >= 0 else "obsSNR<0")
        by_snr[sb][arm][0] += 1
        by_snr[sb][arm][1] += caught

    for origin, a in stats.items():
        print(f"observer {origin}:")
        for arm in sorted(a):
            seen, caught = a[arm]
            print(f"   {arm:8s} seen {seen:4d} caught {caught:4d}  ratio {caught / max(seen, 1):.3f}")
    print("by observer SNR (all observers):")
    for sb in sorted(by_snr):
        print("   " + sb + ": " + "  ".join(f"{arm}={c}/{s}" for arm, (s, c) in sorted(by_snr[sb].items())))


if __name__ == "__main__":
    main()
