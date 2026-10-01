#!/usr/bin/env python3
"""Per-arm result of the local companion's probes (logs/probe.log) against an fw_ab / monitor log.

  probe_report.py PROBE_LOG ARM_LOG
"""
import bisect
import re
import sys
from collections import defaultdict
from datetime import datetime

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mesh_pkt as b  # noqa: E402


def ts(s):
    return datetime.strptime(s, "%Y-%m-%d %H:%M:%S").timestamp()


arms = []
for l in open(sys.argv[2], errors="replace"):
    m = b.arm_re.match(l)
    if m:
        arms.append((ts(m[1]), m[2]))
    elif l[20:28] == "# cycle ":
        arms.append((ts(l[:19]), "gap"))
at = [a[0] for a in arms]
res = defaultdict(lambda: [0, 0, []])
for l in open(sys.argv[1]):
    m = re.match(r"^(\S+ \S+) (status|ver) (ok|LOST) ([\d.]+)", l)
    if not m:
        continue
    t = ts(m[1]) - float(m[4])                   # request time
    i = bisect.bisect_right(at, t) - 1
    if i < 0 or arms[i][1] == "gap" or t > at[-1] + 90:
        continue
    k = i                                        # skip ~2.5 min after a re-flash: the prober has to log in again
    while k > 0 and arms[k][1] != "gap":
        k -= 1
    if arms[k][1] == "gap" and t - at[k] < 150 + 60:
        continue
    r = res[arms[i][1]]
    r[0] += 1
    if m[3] == "ok":
        r[1] += 1
        r[2].append(float(m[4]))
for a, (n, ok, lat) in sorted(res.items()):
    lat.sort()
    print(f"{a:12s} answered {ok}/{n} = {ok / n:.2f}   latency median {lat[len(lat) // 2] if lat else 0:.1f}s "
          f"p90 {lat[int(len(lat) * 0.9)] if lat else 0:.1f}s max {lat[-1] if lat else 0:.1f}s")
