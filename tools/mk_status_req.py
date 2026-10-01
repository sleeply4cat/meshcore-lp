#!/usr/bin/env python3
"""Build a fresh raw 'get status' request to the test repeater, as if sent by a logged-in client.

  mk_status_req.py [--client PUBKEY_HEX | --client-prefix F0] [--count N] [--cli "lp stats"]

With --cli the packet is a console command (TXT_MSG, CLI type) instead of a status request; the
client must be logged in as admin for the node to answer it.

The request is encrypted with the client<->node shared secret, which we can compute from the node's
private key (node_prvkey_file in tools/local.json) and the client's public key (default: taken from its
last login packet in the local observer log). Every packet carries the current UNIX time and 4 random bytes, so it is
unique: the node rejects a timestamp not newer than the client's last one (replay protection) and the
mesh drops exact duplicates - generate a new one for each send, never replay an old hex.
Output: one hex line per packet (zero-hop direct REQ, 22 bytes).
"""
import hashlib
import hmac
import os
import re
import sys
import time

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mesh_pkt as b          # noqa: E402
import node_decode as d          # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
import local_config as cfg  # noqa: E402
NODE_PRV = cfg.need("node_prvkey_file")
NODE_HASH = int(cfg.need("node_hash"), 16)


def client_from_log(path, prefix=0xF0):
    pub = None
    for l in open(path, errors="replace"):
        m = d.RAW_RE.match(l)
        if not m:
            continue
        route, ptype, hs, pth, h = b.parse(m[2])
        pkt = bytes.fromhex(m[2])
        i = 1 + (4 if route in (0, 3) else 0)
        pl = pkt[i + 1 + (pkt[i] & 63) * hs:]
        if ptype == 7 and len(pl) > 33 and pl[0] == NODE_HASH and pl[1] == prefix:
            pub = pl[1:33]
    return pub


def build(secret, client_hash, ts, cli=None):
    if cli:
        pt = ts.to_bytes(4, "little") + bytes([1 << 2]) + cli.encode()      # TXT_TYPE_CLI_DATA, attempt 0
        hdr = 0x0A                                                        # TXT_MSG | direct
    else:
        pt = ts.to_bytes(4, "little") + b"\x01" + b"\0\0\0\0" + os.urandom(4)   # REQ get status
        hdr = 0x02                                                        # REQ | direct
    pt += b"\0" * (-len(pt) % 16)
    e = Cipher(algorithms.AES(secret[:16]), modes.ECB()).encryptor()
    ct = e.update(pt) + e.finalize()
    mac = hmac.new(secret, ct, hashlib.sha256).digest()[:2]
    return bytes([hdr, 0x00, NODE_HASH, client_hash]) + mac + ct     # no path (zero hop)


def main():
    a = sys.argv[1:]
    prefix = int(a[a.index("--client-prefix") + 1], 16) if "--client-prefix" in a else 0xF0
    client = bytes.fromhex(a[a.index("--client") + 1]) if "--client" in a else \
        client_from_log(cfg.get("local_obs_log", "./logs/local_obs.log"), prefix)
    n = int(a[a.index("--count") + 1]) if "--count" in a else 1
    cli = a[a.index("--cli") + 1] if "--cli" in a else None
    if not client:
        sys.exit("no client public key (log in once, or pass --client)")
    secret = d.shared_secret(bytes.fromhex(open(NODE_PRV).read().strip()), client)
    ts = int(time.time())
    for k in range(n):
        print(build(secret, client[0], ts + k, cli).hex().upper())


if __name__ == "__main__":
    main()
