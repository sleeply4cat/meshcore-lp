#!/usr/bin/env python3
"""Compare our node with a local observer that logs RAW packets over USB (stock MESH_PACKET_LOGGING).

  local_obs.py OBSERVER_LOG FW_AB_LOG NODE_HASH_HEX

Per arm of FW_AB_LOG:
 copies   copies (hash, path) the observer received that our node also received (own transmissions and
          senders our node never heard in any arm are skipped)
 fwd      unique flood packets the observer received (not already via us) for which it later heard our
          retransmission (a copy whose path contains our hop)
Both logs carry host timestamps (MSK).
"""
import bisect
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


def main(obs_log, log, node):
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
                r = b.parse(m[2])
            except (ValueError, IndexError):
                continue
            ours[(r[4][:16], r[3].hex().upper())].append(ts(m[1]))
    at = [a[0] for a in arms]
    end = at[-1] + 60

    def arm(t):
        i = bisect.bisect_right(at, t) - 1
        if i < 0 or t - at[i] < 5 or t > end or (i + 1 < len(at) and at[i + 1] - t < 5) or arms[i][1] == "gap":
            return None
        return arms[i][1]

    rows, first, ourhop = [], {}, set()
    for l in open(obs_log, errors="replace"):
        m = RAW_RE.match(l)
        if not m:
            continue
        try:
            route, ptype, hs, path, h = b.parse(m[2])
        except (ValueError, IndexError):
            continue
        t = ts(m[1])
        hops = [path[i:i + hs].hex().upper() for i in range(0, len(path), hs)]
        mine = node[:2 * hs] in hops
        if route in (0, 1):
            first.setdefault(h[:16], (t, arm(t), mine, len(hops), ptype))
            if mine:
                ourhop.add(h[:16])
        a = arm(t)
        if not a:
            continue
        last = hops[-1] if hops else "-"
        if last == node[:2 * hs]:
            continue
        pkt = bytes.fromhex(m[2])
        pl = pkt[1 + (4 if route in (0, 3) else 0) + 1 + len(path):]
        if not path and ptype in (0, 1, 2, 8) and len(pl) > 1 and pl[1] == int(node[:2], 16):
            continue                      # zero-hop packet sent by the node itself (e.g. a reply)
        got = any(abs(x - t) < 5 for x in ours.get((h[:16], path.hex().upper()), []))
        rows.append((a, last, got))
    heard = {last for a, last, got in rows if got}
    cop, fwd = defaultdict(lambda: [0, 0]), defaultdict(lambda: [0, 0])
    for a, last, got in rows:
        if last in heard:
            cop[a][0] += 1
            cop[a][1] += got
    for key, (t, a, via_us, nh, ptype) in first.items():
        if a and not via_us and not (ptype == 4 and nh >= 8):
            fwd[a][0] += 1
            fwd[a][1] += key in ourhop
    for a in sorted(set(cop) | set(fwd)):
        (n, c), (fn, fc) = cop[a], fwd[a]
        print(f"{a:12s} copies {c}/{n}={c / max(n, 1):.2f}   fwd {fc}/{fn}={fc / max(fn, 1):.2f}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3].upper())
