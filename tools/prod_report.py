#!/usr/bin/env python3
"""Per-arm repeater behaviour as seen by an MQTT observer: forwarding and replies.

  prod_report.py MQTT FW_AB_LOG NODE_HASH_HEX OBSERVER

 fwd      unique flood packets the observer heard (not via us, hop limit not reached) for which it
          later heard a copy carrying our hop - i.e. we received it AND our retransmission got through
 direct   how many of our copies it heard directly (last hop = us) per hour
 req      zero-hop requests to us, answered within 4 s (reply paired to the latest request)
"""
import bisect
import json
import sys
from collections import defaultdict
from datetime import datetime

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mesh_pkt as b  # noqa: E402


def main(mqtt, log, node, obs):
    me = node.upper()
    arms = []
    for l in open(log, errors="replace"):
        m = b.arm_re.match(l)
        if m:
            arms.append((datetime.strptime(m[1], "%Y-%m-%d %H:%M:%S").timestamp(), m[2]))
        elif l[20:28] == "# cycle ":
            arms.append((datetime.strptime(l[:19], "%Y-%m-%d %H:%M:%S").timestamp(), "gap"))
    at = [a[0] for a in arms]
    end = at[-1] + (sorted(y - x for x, y in zip(at, at[1:]))[len(at) // 2] if len(at) > 2 else 1200)

    def arm(t):
        i = bisect.bisect_right(at, t) - 1
        if i < 0 or t - at[i] < 5 or t > end - 5 or (i + 1 < len(at) and at[i + 1] - t < 5):
            return None
        return None if arms[i][1] == "gap" else arms[i][1]

    first, ourhop, span = {}, set(), defaultdict(float)
    direct = defaultdict(int)
    reqs, rsps = [], []
    for l in open(mqtt, errors="replace"):
        if "/packets " not in l:
            continue
        try:
            j = json.loads(l.split(" ", 2)[2])
            if j.get("origin") != obs or "raw" not in j:
                continue
            route, ptype, hs, path, h = b.parse(j["raw"])
            t = datetime.fromisoformat(j["timestamp"]).timestamp()
        except (ValueError, IndexError, KeyError, json.JSONDecodeError):
            continue
        hops = [path[i:i + hs].hex().upper() for i in range(0, len(path), hs)]
        mine = [x for x in hops if x == me[:2 * hs]]
        a = arm(t)
        if route in (0, 1):
            key = h[:16]
            if key not in first:
                first[key] = (t, a, bool(mine), len(hops), ptype)
            if mine:
                ourhop.add(key)
            if hops and hops[-1] == me[:2 * hs] and a:
                direct[a] += 1
        p = bytes.fromhex(j["raw"])[2:]
        if route == 2 and not path and len(p) > 2 and a:
            if p[0] == int(me[:2], 16) and ptype in (0, 2, 7):
                reqs.append((t, a, ptype, p[1]))
            elif p[1] == int(me[:2], 16) and ptype in (1, 2):
                rsps.append((t, p[0]))
    fwd = defaultdict(lambda: [0, 0])
    for key, (t, a, via_us, nh, ptype) in first.items():
        if not a or via_us or (ptype == 4 and nh >= 8):
            continue
        fwd[a][0] += 1
        fwd[a][1] += key in ourhop
    answered = set()
    for t, dst in rsps:
        c = [i for i, r in enumerate(reqs) if t - 4 <= r[0] < t and (r[2] == 7 or r[3] == dst)]
        if c:
            answered.add(max(c, key=lambda i: reqs[i][0]))
    req = defaultdict(lambda: [0, 0])
    for i, r in enumerate(reqs):
        req[r[1]][0] += 1
        req[r[1]][1] += i in answered
    for x, y in zip(arms, arms[1:] + [(end, "")]):
        if x[1] != "gap":
            span[x[1]] += (y[0] - x[0]) / 3600
    for a in sorted(fwd):
        n, c = fwd[a]
        rn, rc = req[a]
        print(f"{a:12s} fwd {c}/{n}={c / max(n, 1):.2f}   direct {direct[a] / max(span[a], 1e-9):.0f}/h"
              f"   req {rc}/{rn}" + (f"={rc / rn:.2f}" if rn else ""))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4])
