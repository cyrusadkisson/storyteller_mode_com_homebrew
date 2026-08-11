#!/usr/bin/env python3
"""Diff two candump logs to find the frame that controls a load.

This is the tool that closes the last gap in docs/pdm-control.md: the PDM
output command frames (lights, pumps, awning) could not be recovered by static
analysis, so we crack them by observation.

    ./tools/can_capture.sh galley_OFF 15     # load off
    (flip the load ON from the stock screen)
    ./tools/can_capture.sh galley_ON 15      # load on
    ./tools/can_diff.py captures/galley_OFF.log captures/galley_ON.log

MULTIPLEXED FRAMES: the van's PDM traffic reuses one CAN id for several
different messages, selected by byte 0 (e.g. 0x14EF1E11 alternates `FC …` and
`FD …`). Comparing those as one message smears two unrelated payloads together
and hides clean transitions. So an id whose byte 0 takes a small number of
distinct values is split into sub-messages, reported as `0x14EF1E11[FC]`.
Use --no-mux to disable, or --mux N to select on a different byte.

Output is ranked: bytes that hold ONE value in the OFF capture and a DIFFERENT
single value in the ON capture are the strongest candidates. Noisy bytes
(voltages, temperatures, counters) are ranked down automatically.
"""
from __future__ import annotations

import json
import pathlib
import re
import sys
from collections import defaultdict

ROOT = pathlib.Path(__file__).resolve().parent.parent
LINE = re.compile(r"\((?P<ts>\d+\.\d+)\)\s+\S+\s+(?P<id>[0-9A-Fa-f]+)#(?P<data>[0-9A-Fa-f]*)")

MUX_MAX_VALUES = 16   # more distinct values than this -> it's data, not a selector
MUX_MIN_DLC = 3       # need at least a selector + some payload


def load_db():
    p = ROOT / "data" / "can_messages.json"
    if not p.exists():
        return {}
    return {m["can_id"]: m for m in json.loads(p.read_text())}


def parse(path: pathlib.Path):
    """-> list of (can_id, payload bytes)"""
    out = []
    for line in path.read_text().splitlines():
        m = LINE.match(line.strip())
        if not m:
            continue
        out.append((int(m.group("id"), 16), bytes.fromhex(m.group("data"))))
    return out


def find_mux_ids(*record_lists, mux_byte: int):
    """Ids where `mux_byte` looks like a message selector rather than data.

    "Few distinct values" is not enough — a slowly drifting *data* byte also has
    few values in a short capture, and splitting on it invents phantom
    sub-messages (this bit us on the Rixen status frame 0x724, whose byte 0 is
    a reading, not a selector).

    The distinguishing property is that a real multiplexer **cycles**: it
    revisits its values over and over as the sender rotates through
    sub-messages. A drifting reading changes monotonically and rarely returns.
    So we also require the value to recur far more often than it has values.
    """
    seen = defaultdict(set)
    transitions = defaultdict(int)
    revisits = defaultdict(int)
    min_dlc = {}
    last = {}
    for records in record_lists:
        for cid, data in records:
            min_dlc[cid] = min(min_dlc.get(cid, 99), len(data))
            if len(data) <= mux_byte:
                continue
            v = data[mux_byte]
            if cid in last and last[cid] != v:
                transitions[cid] += 1
                if v in seen[cid]:
                    revisits[cid] += 1      # came back to a value seen before
            seen[cid].add(v)
            last[cid] = v

    out = set()
    for cid, vals in seen.items():
        if not (2 <= len(vals) <= MUX_MAX_VALUES):
            continue
        if min_dlc.get(cid, 0) < MUX_MIN_DLC:
            continue
        # a selector cycles: many transitions, and it keeps coming back
        if transitions[cid] >= 2 * len(vals) and revisits[cid] >= len(vals):
            out.add(cid)
    return out


def tabulate(records, mux_ids, mux_byte: int):
    """-> {key: {byte_index: set(values)}}, {key: count}   key = (can_id, mux|None)"""
    vals = defaultdict(lambda: defaultdict(set))
    count = defaultdict(int)
    for cid, data in records:
        key = (cid, data[mux_byte] if cid in mux_ids and len(data) > mux_byte else None)
        count[key] += 1
        for i, b in enumerate(data):
            vals[key][i].add(b)
    return vals, count


def bits_changed(a: int, b: int) -> str:
    x = a ^ b
    return ",".join(str(i) for i in range(8) if x >> i & 1)


def fmt_key(key) -> str:
    cid, mux = key
    ident = f"0x{cid:X}"
    return ident if mux is None else f"{ident}[{mux:02X}]"


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    if len(args) < 2:
        print(__doc__)
        return 2

    mux_byte = 0
    for f in flags:
        if f.startswith("--mux="):
            mux_byte = int(f.split("=", 1)[1])
        elif f == "--mux" and flags.index(f) + 1 < len(flags):
            pass
    use_mux = "--no-mux" not in flags

    paths = []
    for arg in args[:2]:
        p = pathlib.Path(arg)
        paths.append(p if p.is_absolute() else ROOT / p)
    off_p, on_p = paths

    db = load_db()
    off_rec, on_rec = parse(off_p), parse(on_p)
    mux_ids = find_mux_ids(off_rec, on_rec, mux_byte=mux_byte) if use_mux else set()

    off, off_n = tabulate(off_rec, mux_ids, mux_byte)
    on, on_n = tabulate(on_rec, mux_ids, mux_byte)

    print(f"A (baseline) : {off_p.name}  {len(off_rec)} frames, "
          f"{len({k[0] for k in off_n})} ids")
    print(f"B (changed)  : {on_p.name}  {len(on_rec)} frames, "
          f"{len({k[0] for k in on_n})} ids")
    if mux_ids:
        print(f"demultiplexed on byte {mux_byte}: "
              + ", ".join(f"0x{c:X}" for c in sorted(mux_ids)))
    print()

    # --- sub-messages that appeared or vanished entirely ------------------
    new_keys = set(on_n) - set(off_n)
    gone_keys = set(off_n) - set(on_n)
    for label, keys in (("ONLY in B (appeared)", new_keys),
                        ("ONLY in A (disappeared)", gone_keys)):
        if keys:
            print(f"== {label} ==")
            for key in sorted(keys):
                name = db[key[0]]["name"] if key[0] in db else ""
                n = (on_n if keys is new_keys else off_n)[key]
                print(f"   {fmt_key(key)}  ({n} frames)  {name}")
            print()

    # --- byte-level differences, ranked -----------------------------------
    findings = []
    for key in sorted(set(off) & set(on)):
        for i in sorted(set(off[key]) | set(on[key])):
            if key[1] is not None and i == mux_byte:
                continue                          # the selector itself
            a, b = off[key].get(i, set()), on[key].get(i, set())
            if not a or not b or a == b:
                continue

            a_stable, b_stable = len(a) == 1, len(b) == 1
            if a_stable and b_stable:
                score, kind = 0, "CLEAN"          # one value -> another value
            elif a_stable or b_stable:
                score, kind = 1, "semi"
            elif not (a & b):
                score, kind = 2, "disjoint"
            else:
                score, kind = 3, "noisy"
            findings.append((score, key, i, a, b, kind))

    if not findings:
        print("No byte-level differences on shared messages.")
        print("Try a longer capture, or check that the load actually toggled.")
        return 0

    findings.sort(key=lambda f: (f[0], f[1], f[2]))
    print("== byte differences (best candidates first) ==")
    print(f"{'':2}{'MESSAGE':<16} {'byte':>4}  {'A (off)':<18} {'B (on)':<18} bits  name")
    print("-" * 96)
    shown = 0
    for score, key, i, a, b, kind in findings:
        if score >= 3 and shown > 40:
            continue
        shown += 1
        fa = ",".join(f"{v:02X}" for v in sorted(a)[:4]) + ("…" if len(a) > 4 else "")
        fb = ",".join(f"{v:02X}" for v in sorted(b)[:4]) + ("…" if len(b) > 4 else "")
        bit = bits_changed(sorted(a)[0], sorted(b)[0]) if kind == "CLEAN" else ""
        name = db[key[0]]["name"] if key[0] in db else ""
        mark = "**" if kind == "CLEAN" else "  "
        print(f"{mark}{fmt_key(key):<16} {i:>4}  {fa:<18} {fb:<18} {bit:<5} {name}")

    print("-" * 96)
    clean = sum(1 for f in findings if f[0] == 0)
    print(f"{clean} clean transitions (**). Those are your load-control bits.")
    print("\nConfirm before believing it: repeat the OFF->ON->OFF cycle and check the")
    print("same byte tracks the load. One capture can coincide with anything.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
