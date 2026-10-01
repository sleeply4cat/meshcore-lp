#!/usr/bin/env python3
"""A MeshCore companion radio on USB as local RAW observer + autonomous request prober for the node.

  companion_probe.py [--interval 20:60] [--tx DBM]   (settings: tools/local.json)

 * every received packet (companion RX log push) -> local_obs_log as '... RAW: <hex>' lines
   (same format as the stock MESH_PACKET_LOGGING observer, so local_obs/node_decode/resp_report work)
 * logs in to the repeater as admin over a fixed ZERO-HOP direct path (no flood packets at all:
   no adverts, no channel messages, path never unknown), then alternates status requests and a CLI
   'ver' at random intervals -> logs/probe.log: time, kind, ok, latency
"""
import asyncio
import local_config as cfg
import os
import random
import sys
import time

from serial.tools import list_ports
from meshcore import MeshCore, EventType

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
NODE_PUB = cfg.need("node_pubkey")
COMPANION_SERIAL = cfg.need("companion_usb_serial")


def stamp():
    return time.strftime("%Y-%m-%d %H:%M:%S")


def find_port():
    for p in list_ports.comports():
        if p.serial_number == COMPANION_SERIAL:
            return p.device
    return None


async def main():
    a = sys.argv[1:]
    lo, hi = (float(x) for x in (a[a.index("--interval") + 1] if "--interval" in a else "20:60").split(":"))
    tx = int(a[a.index("--tx") + 1]) if "--tx" in a else cfg.get("companion_tx_dbm", 10)
    pw = open(cfg.need("admin_password_file")).read().strip()
    obs = open(cfg.get("local_obs_log", "./logs/local_obs.log"), "a", buffering=1)
    probe = open(os.path.join(ROOT, "logs", "probe.log"), "a", buffering=1)

    mc = await MeshCore.create_serial(find_port(), 115200)

    async def on_rx(ev):
        p = ev.payload
        if "payload" in p:
            obs.write(f"{stamp()} RXLOG snr={p.get('snr')} rssi={p.get('rssi')} RAW: {p['payload'].upper()}\n")
    mc.subscribe(EventType.RX_LOG_DATA, on_rx)

    await mc.start_auto_message_fetching()                # CLI replies arrive as queued messages
    info = await mc.commands.send_appstart()
    if info and "public_key" in info.payload:              # for node_decode (our own TX is not in the RX log)
        open(os.path.join(ROOT, "logs", "companion_self.txt"), "w").write(info.payload["public_key"] + "\n")
    rp = cfg.need("radio")
    await mc.commands.set_radio(rp["freq"], rp["bw"], rp["sf"], rp["cr"])
    await mc.commands.set_tx_power(tx)
    await mc.commands.set_time(int(time.time()))
    await mc.commands.set_manual_add_contacts(True)       # do not collect contacts from adverts
    contact = {"public_key": NODE_PUB, "type": 2, "flags": 0, "out_path_len": 0, "out_path": "",
               "out_path_hash_mode": 1, "adv_name": "LPtest", "last_advert": int(time.time()),
               "adv_lat": 0.0, "adv_lon": 0.0}
    r = await mc.commands.add_contact(contact)
    probe.write(f"{stamp()} # add_contact {r.type}\n")
    r = await mc.commands.send_login_sync(NODE_PUB, pw, timeout=10)
    probe.write(f"{stamp()} # login {'ok' if r and r.type == EventType.LOGIN_SUCCESS else 'FAILED'}\n")

    misses = 0
    while True:
        await asyncio.sleep(random.uniform(lo, hi))
        kind = "status" if random.random() < 0.7 else "ver"
        t0 = time.time()
        ok = False
        try:
            if kind == "status":
                r = await mc.commands.req_status_sync(contact, timeout=10)
                ok = r is not None and getattr(r, "type", None) != EventType.ERROR
            else:
                sent = await mc.commands.send_cmd(NODE_PUB, "ver", dst_type=2)
                if sent.type != EventType.ERROR:
                    ev = await mc.wait_for_event(EventType.CONTACT_MSG_RECV, timeout=10)
                    ok = ev is not None
        except Exception as e:                               # keep probing whatever happens
            probe.write(f"{stamp()} # error {e!r}\n")
        probe.write(f"{stamp()} {kind} {'ok' if ok else 'LOST'} {time.time() - t0:.1f}\n")
        misses = 0 if ok else misses + 1
        if misses >= 2 and misses % 3 == 2:                  # node may have rebooted / lost its ACL: log in again
            r = await mc.commands.send_login_sync(NODE_PUB, pw, timeout=10)
            probe.write(f"{stamp()} # re-login {'ok' if r and r.type == EventType.LOGIN_SUCCESS else 'FAILED'}\n")


if __name__ == "__main__":
    asyncio.run(main())
