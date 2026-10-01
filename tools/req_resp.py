#!/usr/bin/env python3
"""Zero-hop request/response audit of a node from an MQTT observer dump.

  req_resp.py MQTT_DUMP NODE_HASH_HEX OBSERVER [SINCE_ISO] [UNTIL_ISO]

Requests: direct zero-hop REQ/TXT/ANON_REQ with dest hash = first byte of NODE. Each response
(RESPONSE/TXT from that byte, zero-hop) is paired with the latest preceding request (<= 4 s).
"""
import collections
import json
import sys
from datetime import datetime

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from mesh_pkt import parse  # noqa: E402


def main():
    mqtt, node = sys.argv[1], int(sys.argv[2][:2], 16)
    obs = sys.argv[3]
    since = datetime.fromisoformat(sys.argv[4]).timestamp() if len(sys.argv) > 4 else 0
    until = datetime.fromisoformat(sys.argv[5]).timestamp() if len(sys.argv) > 5 else 1e12
    reqs, rsps = [], []
    for l in open(mqtt, errors="replace"):
        if "/packets " not in l:
            continue
        try:
            j = json.loads(l.split(" ", 2)[2])
            if j.get("origin") != obs or "raw" not in j:
                continue
            route, ptype, hs, path, h = parse(j["raw"])
            t = datetime.fromisoformat(j["timestamp"]).timestamp()
        except (ValueError, IndexError, KeyError):
            continue
        if not since <= t <= until or route != 2 or path:
            continue
        p = bytes.fromhex(j["raw"])[2:]
        if len(p) < 3:
            continue
        if p[0] == node and ptype in (0, 2, 7):
            reqs.append((t, ptype, p[1]))
        elif p[1] == node and ptype in (1, 2):
            rsps.append((t, p[0]))
    answered = set()
    for t, dst in rsps:
        cand = [i for i, r in enumerate(reqs) if t - 4 <= r[0] < t and (r[1] == 7 or r[2] == dst)]
        if cand:
            answered.add(max(cand, key=lambda i: reqs[i][0]))
    st = collections.defaultdict(lambda: [0, 0])
    for i, (t, ptype, _) in enumerate(reqs):
        for k in ("all", f"type {ptype}", datetime.fromtimestamp(t).strftime("%d %H:00")):
            st[k][0] += 1
            st[k][1] += i in answered
    for k in sorted(st):
        n, ok = st[k]
        print(f"{k:10s} requests {n:4d} answered {ok:4d}  {ok / n:.2f}")


if __name__ == "__main__":
    main()
