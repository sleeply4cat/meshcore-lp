#!/usr/bin/env python3
"""Ground truth from a nearby repeater's own retransmissions.

For every flood packet the observer (a repeater with an MQTT uplink) reports as received,
it normally retransmits it with its own hash appended to the path. Those retransmissions reach
us at high SNR, so we should hear (nearly) all of them. Per arm: fraction of such packet hashes
for which our log contains a copy whose last path hop is the observer.

  mesh_pkt.py MQTT_DUMP OUR_LOG OBSERVER_NAME OBSERVER_HASH_HEX
"""
import bisect
import hashlib
import json
import re
import sys
from collections import defaultdict
from datetime import datetime

arm_re = re.compile(r"^(\S+ \S+) ARM (\w+)")
rx_re = re.compile(r"^(\S+ \S+) .*RX (\w+) len=\d+ .* : ([0-9A-F]+)")
RX_REPEATER_RE = re.compile(r"^(\S+ \S+) .*RX, len=\d+ .*?(?:mode=(\w+) .*?path=([0-9A-F]*) )?.*hash=([0-9A-F]{16})")


def parse(raw_hex):
    b = bytes.fromhex(raw_hex)
    hdr = b[0]
    route = hdr & 3
    ptype = (hdr >> 2) & 15
    i = 1 + (4 if route in (0, 3) else 0)
    pl = b[i]
    hs = (pl >> 6) + 1
    cnt = pl & 63
    path = b[i + 1:i + 1 + cnt * hs]
    payload = b[i + 1 + cnt * hs:]
    h = hashlib.sha256(bytes([ptype]) + (bytes([pl]) if ptype == 9 else b"") + payload).hexdigest().upper()
    return route, ptype, hs, path, h


def main():
    mqtt_path, log_path = sys.argv[1], sys.argv[2]
    obs_name = sys.argv[3]
    obs_hash = bytes.fromhex(sys.argv[4])

    arms, ours = [], defaultdict(list)   # packet hash -> [(t, arm, last_hop)]
    for l in open(log_path, errors="replace"):
        m = arm_re.match(l)
        if m:
            arms.append((datetime.strptime(m[1], "%Y-%m-%d %H:%M:%S").timestamp(), m[2]))
            continue
        m = rx_re.match(l)
        if m:
            try:
                route, ptype, hs, path, h = parse(m[3])
            except (IndexError, ValueError):
                continue
            t = datetime.strptime(m[1], "%Y-%m-%d %H:%M:%S").timestamp()
            ours[h[:16]].append((t, m[2], path[-hs:] if path else b""))
    arm_t = [a[0] for a in arms]

    seen_hash = set()
    res = defaultdict(lambda: [0, 0])
    for l in open(mqtt_path, errors="replace"):
        if "/packets " not in l:
            continue
        ts, topic, payload = l.split(" ", 2)
        try:
            j = json.loads(payload)
            if j.get("origin") != obs_name or "raw" not in j:
                continue
            route, ptype, hs, path, h = parse(j["raw"])
        except (ValueError, IndexError, json.JSONDecodeError):
            continue
        if route not in (1, 2) or route != 1 and route != 2:
            pass
        if route not in (0, 1):          # flood routes only (transport flood / flood)
            continue
        key = h[:16]
        if key in seen_hash:              # repeater forwards only the first copy
            continue
        seen_hash.add(key)
        if obs_hash[:hs] in [path[k:k + hs] for k in range(0, len(path), hs)]:
            continue                      # already went through the observer: it will not forward again
        t = datetime.fromisoformat(ts).timestamp()
        i = bisect.bisect_right(arm_t, t) - 1
        if i < 0 or t - arm_t[i] < 4 or (i + 1 < len(arm_t) and arm_t[i + 1] - t < 4):
            continue
        arm = arms[i][1]
        caught = any(abs(x[0] - t) < 20 and x[2] == obs_hash[:hs] for x in ours.get(key, []))
        res[arm][0] += 1
        res[arm][1] += caught
    for arm in sorted(res):
        n, c = res[arm]
        print(f"{arm:8s} observer-forwarded floods {n:4d}, heard its retransmission {c:4d}  ratio {c / max(n, 1):.3f}")


if __name__ == "__main__":
    main()
