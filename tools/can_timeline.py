#!/usr/bin/env python3
"""Show PDM command-frame state changes over time, with channel names.

For captures where a control is operated *during* the recording, a before/after
diff loses the sequence. This prints one row per state change, so you can see
exactly what moved and when.

    ./tools/can_timeline.py captures/master_toggle.log
    ./tools/can_timeline.py captures/dimsweep.log --all      # every sample
    ./tools/can_timeline.py captures/x.log --id 0x14EF1E11   # one PDM

Channel names come from data/pdm_channels.csv, so the map stays the single
source of truth — update the CSV and this output follows.
"""
from __future__ import annotations

import csv
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LINE = re.compile(r"\((?P<ts>\d+\.\d+)\)\s+\S+\s+(?P<id>[0-9A-Fa-f]+)#(?P<data>[0-9A-Fa-f]*)")


def load_names():
    """-> {(cmd_id, mux): {byte: label}}"""
    names = {}
    p = ROOT / "data" / "pdm_channels.csv"
    if not p.exists():
        return names
    for r in csv.DictReader(p.open()):
        try:
            key = (int(r["cmd_can_id"], 16), int(r["cmd_mux"], 16))
            names.setdefault(key, {})[int(r["cmd_byte"])] = (
                f"{r['do']} {r['load_name']}" if r["load_name"] else f"{r['do']} ?")
        except (ValueError, KeyError):
            continue
    return names


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    if not args:
        print(__doc__)
        return 2

    path = pathlib.Path(args[0])
    if not path.is_absolute():
        path = ROOT / path
    show_all = "--all" in flags
    only = None
    for f in flags:
        if f.startswith("--id="):
            only = int(f.split("=", 1)[1], 16)

    names = load_names()
    frames = []
    for line in path.read_text().splitlines():
        m = LINE.match(line.strip())
        if not m:
            continue
        cid = int(m.group("id"), 16)
        data = bytes.fromhex(m.group("data"))
        if (cid, data[0] if data else None) in names:
            frames.append((float(m.group("ts")), cid, data))

    if not frames:
        print(f"No known PDM command frames in {path.name}.")
        return 1

    t0 = frames[0][0]
    print(f"{path.name} — {len(frames)} PDM command frames, "
          f"{frames[-1][0] - t0:.1f}s\n")

    for key in sorted(names):
        cid, mux = key
        if only is not None and cid != only:
            continue
        seq = [(t - t0, tuple(d[1:7])) for t, c, d in frames
               if c == cid and d[0] == mux]
        if not seq:
            continue
        labels = names[key]
        hdr = "  ".join(f"{labels.get(i, f'byte{i}'):>16}" for i in range(1, 7))

        states, prev = [], None
        for t, v in seq:
            if show_all or v != prev:
                states.append((t, v, prev))
                prev = v

        tag = f"0x{cid:08X}[{mux:02X}]"
        if len(states) <= 1 and not show_all:
            vals = " ".join(f"{b:02X}" for b in states[0][1]) if states else "-"
            print(f"{tag}   no change   {vals}")
            continue

        print(f"{tag}   {len(states)} state(s)")
        print(f"  {'t':>6}  {hdr}")
        for t, v, prev_v in states:
            cells = []
            for i, b in enumerate(v):
                mark = "*" if prev_v is not None and prev_v[i] != b else " "
                cells.append(f"{mark}0x{b:02X}".rjust(16))
            print(f"  {t:6.2f}  " + "  ".join(cells))
        print("  (* = byte changed from the previous state)\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
