#!/usr/bin/env python3
"""
parse_can_config.py — extract the CAN message/signal database ("DBC") from a
Storyteller/3sigma `Configuration.bin`.

`Configuration.bin` (found in the firmware under
`config/config0/Configuration.bin`) is gzip'd. Decompressed, it is a custom
typed-node tree. This tool decodes the CAN portion into a flat table of
messages and their signals (bit layout + scaling), which is everything you need
to read/write frames on the bus.

Operates on YOUR OWN local copy. No proprietary content is embedded here.

Binary format (reverse-engineered — little-endian throughout):

  Node type tags (4 bytes) seen:
    08 bc 6d 46   CAN message record
    44 b7 fd 36   signal (SPN) record, fixed 40 bytes
    d9 b7 f8 f2   "container" node (holds the signal-array pointer at +0x10)
    8f 8e ef ea   value node: [tag][u32 len][payload]  (a string, or a u32[] of
                  child offsets when used as an array). Strings are interned and
                  referenced by absolute file offset.

  Message record @o (fields at fixed offsets):
    +0x04 u32  node id
    +0x08 u32  -> value node holding the message NAME string
    +0x0c u32  -> value node holding the FRAME-TYPE string
                  ("Standard/Extended Frame Send/Receive")
    +0x10 u32  CAN id (11-bit if Standard, 29-bit/J1939 if Extended)
    +0x18 u32  DLC (data length, bytes)
    +0x20 u32  -> value node (len 4) whose payload is a ptr to the container node;
                  container +0x10 -> value node that is a u32[] of signal offsets

  Signal record @s (40 bytes):
    +0x04 u32     node id
    +0x08 u32     param id (internal slot; not the DeviceInformation id)
    +0x0c u32     start bit (LSB position within the 64-bit frame)
    +0x10 u32     end bit (inclusive) ; length = end-start+1
    +0x18 f64     scale   (engineering = raw*scale + offset)
    +0x20 f64     offset

Usage:
    python3 parse_can_config.py Configuration.bin                 # print table
    python3 parse_can_config.py Configuration.bin --csv out.csv   # write CSV
    python3 parse_can_config.py Configuration.bin --json out.json
"""
from __future__ import annotations
import argparse, csv, gzip, json, re, struct, sys

STR   = b"\x8f\x8e\xef\xea"
MSG   = b"\x08\xbc\x6d\x46"
SIG   = b"\x44\xb7\xfd\x36"
CHILD = b"\xd9\xb7\xf8\xf2"


def load(path: str) -> bytes:
    raw = open(path, "rb").read()
    if raw[:2] == b"\x1f\x8b":
        raw = gzip.decompress(raw)
    return raw


def parse(data: bytes):
    U = lambda o: struct.unpack_from("<I", data, o)[0]
    F = lambda o: struct.unpack_from("<d", data, o)[0]
    n = len(data)

    def rd_str(o):
        return (data[o + 8:o + 8 + U(o + 4)].split(b"\0")[0].decode("latin1", "replace")
                if 0 < o < n and data[o:o + 4] == STR else None)

    def rd_arr(o):
        return ([U(o + 8 + 4 * k) for k in range(U(o + 4) // 4)]
                if 0 < o < n and data[o:o + 4] == STR else [])

    msgs = []
    for m in re.finditer(re.escape(MSG), data):
        o = m.start()
        wrap = U(o + 0x20)
        child = U(wrap + 8) if 0 < wrap < n and data[wrap:wrap + 4] == STR else 0
        arrp = U(child + 0x10) if 0 < child < n and data[child:child + 4] == CHILD else 0
        sigs = []
        for so in rd_arr(arrp):
            if 0 < so < n and data[so:so + 4] == SIG:
                sb, eb = U(so + 0xc), U(so + 0x10)
                sigs.append({"start_bit": sb, "end_bit": eb, "bits": eb - sb + 1,
                             "scale": F(so + 0x18), "offset": F(so + 0x20)})
        ftype = rd_str(U(o + 0xc)) or ""
        msgs.append({
            "name": rd_str(U(o + 8)), "frame_type": ftype,
            "extended": "Extended" in ftype,
            "direction": "TX" if "Send" in ftype else "RX",
            "can_id": U(o + 0x10), "dlc": U(o + 0x18), "signals": sigs,
        })
    msgs.sort(key=lambda x: (not x["extended"], x["can_id"]))
    return msgs


def j1939(cid: int) -> dict:
    pgn = (cid >> 8) & 0x3FFFF
    pf = (pgn >> 8) & 0xFF
    return {"pgn": pgn, "priority": (cid >> 26) & 7, "src": cid & 0xFF,
            "dst": (pgn & 0xFF) if pf < 240 else None}


def fmt_id(m) -> str:
    return f"0x{m['can_id']:08X}" if m["extended"] else f"0x{m['can_id']:03X}"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("config", help="Configuration.bin (gzip or raw)")
    ap.add_argument("--csv"); ap.add_argument("--json")
    args = ap.parse_args()
    msgs = parse(load(args.config))
    nsig = sum(len(m["signals"]) for m in msgs)
    print(f"{len(msgs)} messages, {nsig} signals", file=sys.stderr)

    if args.json:
        json.dump(msgs, open(args.json, "w"), indent=1)
    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["can_id", "extended", "direction", "dlc", "message",
                        "pgn", "src", "dst", "start_bit", "end_bit", "bits",
                        "scale", "offset"])
            for m in msgs:
                j = j1939(m["can_id"]) if m["extended"] else {"pgn": "", "src": "", "dst": ""}
                for s in m["signals"] or [{}]:
                    w.writerow([fmt_id(m), int(m["extended"]), m["direction"], m["dlc"],
                                m["name"],
                                (f"0x{j['pgn']:05X}" if m["extended"] else ""),
                                (f"0x{j['src']:02X}" if m["extended"] else ""),
                                (f"0x{j['dst']:02X}" if m["extended"] and j["dst"] is not None else ""),
                                s.get("start_bit", ""), s.get("end_bit", ""), s.get("bits", ""),
                                s.get("scale", ""), s.get("offset", "")])
    if not (args.json or args.csv):
        for m in msgs:
            extra = ""
            if m["extended"]:
                j = j1939(m["can_id"])
                extra = f"  [PGN 0x{j['pgn']:05X} SA 0x{j['src']:02X}" + \
                        (f" dst 0x{j['dst']:02X}" if j["dst"] is not None else "") + "]"
            print(f"{fmt_id(m):>10} {m['direction']} dlc{m['dlc']} x{len(m['signals']):<2} {m['name']}{extra}")
            for s in m["signals"]:
                print(f"      bit {s['start_bit']:>2}..{s['end_bit']:<2} ({s['bits']:>2}b) "
                      f"scale {s['scale']:g} offset {s['offset']:g}")


if __name__ == "__main__":
    main()
