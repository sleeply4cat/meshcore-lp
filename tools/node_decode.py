#!/usr/bin/env python3
"""Decode the test repeater's own traffic from a local observer's RAW log (stock MESH_PACKET_LOGGING).

  node_decode.py OBSERVER_LOG NODE_PRVKEY_FILE [--node 95] [--follow] [--csv logs/node_status.csv]

With the node's private key, packets between the node and its clients (whose public keys are
learnt from their login packets) are decrypted: requests, status replies (battery, uptime,
counters) and CLI text. Passwords and keys are never printed. Timestamps are the observer log's (MSK).
"""
import hashlib
import local_config as cfg
import hmac
import re
import sys
import time

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import mesh_pkt as b  # noqa: E402

P = 2 ** 255 - 19
RAW_RE = re.compile(r"^(\S+ \S+) .*RAW: ([0-9A-F]+)")
REQ_NAMES = {1: "status", 2: "keepalive", 3: "telemetry", 5: "acl", 6: "neighbours", 7: "owner"}
SECRET_WORDS = ("password", "prv.key", "guest.password")


def ed_to_x25519(pub):
    y = int.from_bytes(pub, "little") & ((1 << 255) - 1)
    u = (1 + y) * pow((1 - y) % P, P - 2, P) % P
    return u.to_bytes(32, "little")


def shared_secret(prv64, pub):
    k = X25519PrivateKey.from_private_bytes(prv64[:32])
    return k.exchange(X25519PublicKey.from_public_bytes(ed_to_x25519(pub)))


def mac_decrypt(secret, data):
    mac, ct = data[:2], data[2:]
    if len(ct) == 0 or len(ct) % 16 or hmac.new(secret, ct, hashlib.sha256).digest()[:2] != mac:
        return None
    d = Cipher(algorithms.AES(secret[:16]), modes.ECB()).decryptor()
    return d.update(ct) + d.finalize()


def mask(text):
    for w in SECRET_WORDS:
        text = re.sub(rf"({re.escape(w)})\s+\S+", r"\1 <hidden>", text)
    return text


def status(data):
    if len(data) < 56:
        return None
    u16 = lambda o: int.from_bytes(data[o:o + 2], "little")
    s16 = lambda o: int.from_bytes(data[o:o + 2], "little", signed=True)
    u32 = lambda o: int.from_bytes(data[o:o + 4], "little")
    return {"batt_mv": u16(0), "queue": u16(2), "noise": s16(4), "last_rssi": s16(6), "recv": u32(8),
            "sent": u32(12), "tx_air_s": u32(16), "uptime_s": u32(20), "err": u16(40), "rx_air_s": u32(48)}


class Decoder:
    def __init__(self, prv64):
        self.prv = prv64
        self.clients = {}      # first byte -> (pubkey, secret)
        self.pending = {}      # request tag -> name

    def learn(self, pub):
        if pub[0] not in self.clients:
            self.clients[pub[0]] = (pub, shared_secret(self.prv, pub))

    def handle(self, ts, raw, me, csv):
        try:
            route, ptype, hs, path, h = b.parse(raw)
        except (ValueError, IndexError):
            return None
        pkt = bytes.fromhex(raw)
        i = 1 + (4 if route in (0, 3) else 0)
        payload = pkt[i + 1 + (pkt[i] & 63) * hs:]
        if ptype == 7 and len(payload) > 35 and payload[0] == me:          # ANON_REQ (login)
            self.learn(payload[1:33])
            return f"{ts} {payload[1]:02X}->{me:02X} login request"
        if ptype not in (0, 1, 2, 8) or len(payload) < 4:
            return None
        dst, src = payload[0], payload[1]
        other = src if dst == me else dst if src == me else None
        if other is None or other not in self.clients:
            return None
        pt = mac_decrypt(self.clients[other][1], payload[2:])
        if pt is None:
            return None
        who = f"{src:02X}->{dst:02X}"
        tag = int.from_bytes(pt[:4], "little")
        if ptype == 0:
            name = REQ_NAMES.get(pt[4], f"type {pt[4]}")
            self.pending[tag] = name
            return f"{ts} {who} request {name}"
        if ptype == 2:
            text = pt[5:].rstrip(b"\0").decode("utf-8", "replace")
            kind = "cli" if (pt[4] >> 2) == 1 else "txt"
            return f"{ts} {who} {kind}: {mask(text)}"
        if ptype == 1:
            name = self.pending.pop(tag, None)
            st = status(pt[4:]) if name == "status" or (name is None and len(pt) >= 60) else None
            if st and name == "status":
                if csv:
                    csv.write(f"{ts},{st['batt_mv']},{st['uptime_s']},{st['recv']},{st['sent']},{st['noise']}\n")
                    csv.flush()
                return (f"{ts} {who} status: batt {st['batt_mv']} mV, up {st['uptime_s'] // 3600}h{st['uptime_s'] % 3600 // 60:02d}m, "
                        f"recv {st['recv']}, sent {st['sent']}, noise {st['noise']}, tx air {st['tx_air_s']} s")
            return f"{ts} {who} response" + (f" to {name}" if name else "") + f" ({len(pt)} bytes)"
        return None


def main():
    args = sys.argv[1:]
    follow = "--follow" in args
    csvp = args[args.index("--csv") + 1] if "--csv" in args else None
    node = args[args.index("--node") + 1] if "--node" in args else None
    cl = args[args.index("--clients") + 1] if "--clients" in args else None
    pos = [a for a in args if not a.startswith("--") and a not in (csvp, node, cl)]
    log, keyf = pos[0], pos[1]
    prv = bytes.fromhex(open(keyf).read().strip())
    dec = Decoder(prv)
    if "--clients" in args:                                  # extra known client public keys, one hex per line
        for x in open(args[args.index("--clients") + 1]):
            if x.strip():
                dec.learn(bytes.fromhex(x.strip()))
    me = int(args[args.index("--node") + 1] if "--node" in args else cfg.need("node_hash"), 16)   # node's hash byte
    csv = open(csvp, "a") if csvp else None
    f = open(log, errors="replace")
    while True:
        l = f.readline()
        if not l:
            if not follow:
                break
            time.sleep(1)
            continue
        m = RAW_RE.match(l)
        if m:
            out = dec.handle(m[1], m[2], me, csv)
            if out:
                print(out, flush=True)


if __name__ == "__main__":
    main()
