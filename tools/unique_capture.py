#!/usr/bin/env python3
"""Per-arm capture of unique packets (any copy) seen by MQTT observers: unique_capture.py MQTT OUR_LOG"""
import bisect
import json
import math
import sys
from collections import defaultdict
from datetime import datetime

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mesh_pkt as b  # noqa: E402


def main(mqtt, log):
    arms, ours = [], defaultdict(list)
    for l in open(log, errors="replace"):
        m = b.arm_re.match(l)
        if m:
            arms.append((datetime.strptime(m[1], "%Y-%m-%d %H:%M:%S").timestamp(), m[2]))
            continue
        if l[20:28] == "# cycle ":   # fw_ab.py: re-flash in progress, nobody listening
            arms.append((datetime.strptime(l[:19], "%Y-%m-%d %H:%M:%S").timestamp(), "gap"))
            continue
        m = b.RX_REPEATER_RE.match(l)
        if m:
            ours[m[4][:16]].append(datetime.strptime(m[1], "%Y-%m-%d %H:%M:%S").timestamp())
            continue
        m = b.rx_re.match(l)
        if not m:
            continue
        try:
            r = b.parse(m[3])
        except (ValueError, IndexError):
            continue
        ours[r[4][:16]].append(datetime.strptime(m[1], "%Y-%m-%d %H:%M:%S").timestamp())
    arm_t = [a[0] for a in arms]
    # the last arm lasts one typical slot at most, and nothing after our log ended counts
    slot = sorted(b - a for a, b in zip(arm_t, arm_t[1:]))[len(arm_t) // 2 - 1] if len(arm_t) > 2 else 60
    t_log_end = max([arm_t[-1] + slot] + [max(v) for v in ours.values()] if ours else [arm_t[-1] + slot])
    t_log_end = min(t_log_end, arm_t[-1] + slot)
    for obs in sorted({json.loads(l.split(" ", 2)[2]).get("origin") for l in open(mqtt, errors="replace")
                       if "/packets " in l and l.split(" ", 2)[2].startswith("{")} - {None}):
        seen, res = set(), defaultdict(lambda: [0, 0])
        for l in open(mqtt, errors="replace"):
            if "/packets " not in l:
                continue
            ts, _, p = l.split(" ", 2)
            try:
                j = json.loads(p)
                if j.get("origin") != obs:
                    continue
                key = b.parse(j["raw"])[4][:16]
            except (ValueError, IndexError, KeyError, json.JSONDecodeError):
                continue
            if key in seen:
                continue
            seen.add(key)
            t = datetime.fromisoformat(ts).timestamp()
            i = bisect.bisect_right(arm_t, t) - 1
            if i < 0 or t - arm_t[i] < 4 or (i + 1 < len(arm_t) and arm_t[i + 1] - t < 4):
                continue
            if t > t_log_end - 4:
                continue
            res[arms[i][1]][0] += 1
            res[arms[i][1]][1] += any(abs(x - t) < 20 for x in ours.get(key, []))
        out = []
        for a, (n, c) in sorted(res.items()):
            if a == "gap":
                continue
            p = c / max(n, 1)
            se = math.sqrt(p * (1 - p) / max(n, 1))
            out.append(f"{a}={c}/{n}={p:.2f}±{se:.2f}")
        print(f"{obs:6s} " + "  ".join(out))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
