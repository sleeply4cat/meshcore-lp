#!/usr/bin/env python3
"""Per-arm responsiveness of the node, seen by a local RAW observer (companion).

  resp_report.py OBSERVER_LOG FW_AB_LOG NODE_HASH CLIENT_HASH

 local    zero-hop requests from CLIENT heard by the observer: answered within 8 s, reply latency
 replies  all replies from the node to any client per hour (remote automation load: compare rates)
"""
import bisect
import re
import sys
from collections import defaultdict
from datetime import datetime

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mesh_pkt as b  # noqa: E402

RAW_RE = re.compile(r"^(\S+ \S+) .*RAW: ([0-9A-F]+)")


def ts(s):
    return datetime.strptime(s, "%Y-%m-%d %H:%M:%S").timestamp()


def main(obs, log, node, client):
    arms = []
    for l in open(log, errors="replace"):
        m = b.arm_re.match(l)
        if m:
            arms.append((ts(m[1]), m[2]))
        elif l[20:28] == "# cycle ":
            arms.append((ts(l[:19]), "gap"))
    at = [a[0] for a in arms]
    end = at[-1] + 60

    def arm(t):
        i = bisect.bisect_right(at, t) - 1
        return None if i < 0 or t > end or arms[i][1] == "gap" else arms[i][1]

    reqs, reps, allrep = [], [], defaultdict(int)
    for l in open(obs, errors="replace"):
        m = RAW_RE.match(l)
        if not m:
            continue
        try:
            route, ptype, hs, path, h = b.parse(m[2])
        except (ValueError, IndexError):
            continue
        t = ts(m[1])
        pkt = bytes.fromhex(m[2])
        i = 1 + (4 if route in (0, 3) else 0)
        pl = pkt[i + 1 + (pkt[i] & 63) * hs:]
        if len(pl) < 2:
            continue
        a = arm(t)
        if route == 2 and not path and ptype in (0, 2, 7) and pl[0] == node and pl[1] == client:
            reqs.append((t, a))
        if ptype in (1, 2) and pl[1] == node:
            if route == 2 and not path and pl[0] == client:
                reps.append(t)
            if a and not (path and route in (0, 1)):     # count each reply once (our own transmission)
                allrep[a] += 1
    span = defaultdict(float)
    for x, y in zip(arms, arms[1:] + [(end, "")]):
        if x[1] != "gap":
            span[x[1]] += (y[0] - x[0]) / 3600
    res = defaultdict(lambda: [0, 0, []])
    for t, a in reqs:
        if not a:
            continue
        r = [x for x in reps if t < x <= t + 8]
        res[a][0] += 1
        if r:
            res[a][1] += 1
            res[a][2].append(r[0] - t)
    for a in sorted(set(res) | set(allrep)):
        n, ok, lat = res[a]
        lat.sort()
        l50 = f"{lat[len(lat) // 2]:.0f}" if lat else "-"
        l90 = f"{lat[int(len(lat) * 0.9)]:.0f}" if lat else "-"
        print(f"{a:12s} local requests {ok}/{n}" + (f"={ok / n:.2f}" if n else "") +
              f"  latency median {l50}s p90 {l90}s   replies to all clients {allrep[a] / max(span[a], 1e-9):.0f}/h")


if __name__ == "__main__":
    a = sys.argv
    main(a[1], a[2], int(a[3], 16), int(a[4], 16))
