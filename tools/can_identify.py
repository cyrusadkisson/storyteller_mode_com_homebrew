#!/usr/bin/env python3
"""Summarise a candump log and cross-reference it against our extracted CAN DB.

This is the *rig validation* step: if the known Rixen heater frames (0x724-0x789)
and the J1939 PGNs from Configuration.bin show up here, we are reading the right
bus correctly and everything in docs/can-map.md applies.

    ./tools/can_identify.py captures/baseline.log

Reads data/can_messages.json (from tools/parse_can_config.py).
"""
from __future__ import annotations

import json
import pathlib
import re
import sys
from collections import defaultdict

ROOT = pathlib.Path(__file__).resolve().parent.parent

# candump -L format:  (1699999999.123456) can0 18FEE500#0102030405060708
LINE = re.compile(r"\((?P<ts>\d+\.\d+)\)\s+(?P<if>\S+)\s+(?P<id>[0-9A-Fa-f]+)#(?P<data>[0-9A-Fa-f]*)")


def load_db():
    p = ROOT / "data" / "can_messages.json"
    if not p.exists():
        return {}, {}
    msgs = json.loads(p.read_text())
    by_id = {}
    by_pgn = {}
    for m in msgs:
        by_id[m["can_id"]] = m
        if m.get("extended"):
            # J1939: PGN sits in bits 8-25 of the 29-bit id
            by_pgn[(m["can_id"] >> 8) & 0x3FFFF] = m
    return by_id, by_pgn


def j1939(can_id: int):
    """Split a 29-bit id into J1939 fields."""
    return {
        "pri": (can_id >> 26) & 0x7,
        "pgn": (can_id >> 8) & 0x3FFFF,
        "pf": (can_id >> 16) & 0xFF,
        "ps": (can_id >> 8) & 0xFF,
        "sa": can_id & 0xFF,
    }


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    path = pathlib.Path(sys.argv[1])
    if not path.is_absolute():
        path = ROOT / path

    by_id, by_pgn = load_db()

    stats = defaultdict(lambda: {"n": 0, "ext": False, "dlc": 0,
                                 "first": None, "last": None,
                                 "payloads": set(), "bytes": defaultdict(set)})
    total = 0
    for line in path.read_text().splitlines():
        m = LINE.match(line.strip())
        if not m:
            continue
        total += 1
        raw = m.group("id")
        cid = int(raw, 16)
        data = bytes.fromhex(m.group("data"))
        ts = float(m.group("ts"))

        s = stats[cid]
        s["n"] += 1
        s["ext"] = len(raw) > 3
        s["dlc"] = len(data)
        s["first"] = ts if s["first"] is None else s["first"]
        s["last"] = ts
        if len(s["payloads"]) < 64:
            s["payloads"].add(data.hex())
        for i, b in enumerate(data):
            s["bytes"][i].add(b)

    if not total:
        print(f"No frames in {path}")
        return 1

    span = max(s["last"] for s in stats.values()) - min(s["first"] for s in stats.values())
    print(f"{path.name}: {total} frames, {len(stats)} ids, {span:.1f}s\n")

    known = 0
    print(f"{'CAN ID':<12} {'typ':<4} {'n':>6} {'Hz':>7}  {'chg':<10} known message")
    print("-" * 84)
    for cid, s in sorted(stats.items(), key=lambda kv: -kv[1]["n"]):
        hz = s["n"] / span if span > 0 else 0
        # which byte positions ever change -> where the live data is
        chg = "".join(str(i) if len(s["bytes"][i]) > 1 else "." for i in range(s["dlc"]))

        hit = by_id.get(cid)
        label = ""
        if hit:
            label = hit["name"]
            known += 1
        elif s["ext"]:
            f = j1939(cid)
            p = by_pgn.get(f["pgn"])
            if p:
                label = f"~{p['name']} (PGN {f['pgn']:05X}, SA {f['sa']:02X})"
                known += 1
            else:
                label = f"PGN {f['pgn']:05X}  SA {f['sa']:02X}  pri {f['pri']}"

        star = "*" if hit else " "
        ident = f"0x{cid:08X}" if s["ext"] else f"0x{cid:03X}"
        print(f"{star}{ident:<11} {'ext' if s['ext'] else 'std':<4} "
              f"{s['n']:>6} {hz:>7.1f}  {chg:<10} {label}")

    print("-" * 84)
    print(f"{known}/{len(stats)} ids matched the extracted DB  (* = exact id match)")

    # --- rig validation ----------------------------------------------------
    rixen = [c for c in stats if 0x724 <= c <= 0x789]
    print()
    if rixen:
        print("RIG VALIDATED: Rixen heater frames present -> " +
              ", ".join(f"0x{c:03X}" for c in sorted(rixen)))
    else:
        print("No Rixen frames (0x724-0x789) seen. Either the heater is asleep, or")
        print("this is the other CAN pair. Not fatal, but check the J1939 matches above.")

    print("\n'chg' column = byte positions that CHANGED during the capture "
          "(. = constant).\nThose are the live data bytes worth diffing.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
