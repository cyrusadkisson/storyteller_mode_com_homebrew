#!/usr/bin/env python3
"""Sample the CAN2 battery frames periodically and append a CSV row.

Built for unattended discharge logging — the interesting events (cell
divergence, sag growth) happen over hours, and two snapshots either side tell
you far less than a curve.

    ./tools/battery_log.py                    # every 30s to captures/battery_log.csv
    ./tools/battery_log.py --interval 10
    ./tools/battery_log.py --out somewhere.csv

Requires can0 up on CAN2 (see tools/can_up.sh). Runs until interrupted.

Decode per docs/energy-can2.md. Note the sign convention: the wire reports
positive current as flowing OUT of the pack, so `power_w` here is positive when
discharging — the inverse of what the van's panel displays.
"""
from __future__ import annotations

import csv
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
IFACE = "can0"

CELL_FRAMES = ["18FF938E", "18FF948E", "18FF958E", "18FF968E"]
HEADER = (["utc", "elapsed_s", "soc_pct", "volts", "amps", "power_w", "temp_f",
           "capacity_ah", "time_left_h"]
          + [f"cell{i}" for i in range(1, 17)]
          + ["cell_min", "cell_max", "cell_spread_v", "weakest_cell"])


def sample(dwell: int = 3) -> dict[str, bytes]:
    """Collect the most recent copy of each id seen in a short listen.

    Time-bounded rather than frame-bounded. An earlier version used
    `candump -n 400`, which needs ~8s on this bus (~49 frames/s) and so blew
    past its own timeout and discarded everything. `timeout` exits non-zero on
    expiry but stdout is preserved, which is what we want.
    """
    latest: dict[str, bytes] = {}
    out = subprocess.run(
        ["timeout", str(dwell), "candump", IFACE],
        capture_output=True, text=True).stdout
    for line in out.splitlines():
        # candump default format: "  can0  19FFFD46   [8]  01 78 28 04 ..."
        parts = line.split()
        if len(parts) >= 4 and parts[0] == IFACE:
            try:
                latest[parts[1].upper()] = bytes.fromhex("".join(parts[3:]))
            except ValueError:
                continue
    return latest


def u16(d: bytes, i: int) -> int:
    return d[i] | d[i + 1] << 8


def decode(latest: dict[str, bytes]) -> dict | None:
    d1 = latest.get("19FFFD46")
    if not d1:
        return None
    volts = u16(d1, 2) * 0.05
    amps = (int.from_bytes(d1[4:8], "little") - 2_000_000_000) / 1000
    row = {"volts": round(volts, 2), "amps": round(amps, 2),
           "power_w": round(volts * amps, 1)}

    d2 = latest.get("19FFFC46")
    if d2:
        tr = u16(d2, 5)
        row["soc_pct"] = d2[4] * 0.5
        row["temp_f"] = round((u16(d2, 2) * 0.03125 - 273) * 9 / 5 + 32, 1)
        row["time_left_h"] = "" if tr == 0xFFFF else round(tr / 60, 2)

    d3 = latest.get("19FFFB46")
    if d3:
        row["capacity_ah"] = u16(d3, 3)

    cells = []
    for cid in CELL_FRAMES:
        d = latest.get(cid)
        if d:
            cells += list(d[4:8])
    if len(cells) == 16:
        for i, v in enumerate(cells, 1):
            row[f"cell{i}"] = round(2.00 + v / 100, 2)
        lo, hi = min(cells), max(cells)
        row["cell_min"] = round(2.00 + lo / 100, 2)
        row["cell_max"] = round(2.00 + hi / 100, 2)
        row["cell_spread_v"] = round((hi - lo) / 100, 2)
        # only meaningful once they actually differ
        row["weakest_cell"] = cells.index(lo) + 1 if lo != hi else ""
    return row


def main() -> int:
    args = sys.argv[1:]
    interval = 30.0
    out = ROOT / "captures" / "battery_log.csv"
    for i, a in enumerate(args):
        if a == "--interval" and i + 1 < len(args):
            interval = float(args[i + 1])
        if a == "--out" and i + 1 < len(args):
            out = pathlib.Path(args[i + 1])
    out.parent.mkdir(parents=True, exist_ok=True)

    fresh = not out.exists()
    t0 = time.time()
    with out.open("a", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=HEADER, extrasaction="ignore")
        if fresh:
            w.writeheader()
        print(f"logging to {out} every {interval:.0f}s — Ctrl-C to stop")
        while True:
            row = decode(sample())
            if row:
                row["utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
                row["elapsed_s"] = int(time.time() - t0)
                w.writerow(row)
                fh.flush()          # survive a kill
                print(f"  {row['utc']}  SOC {row.get('soc_pct','?')}%  "
                      f"{row['volts']}V  {row['amps']:+}A  {row['power_w']:+}W  "
                      f"spread {row.get('cell_spread_v','?')}V")
            else:
                print("  (no BMS frames — is can0 up and on CAN2?)")
            time.sleep(interval)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nstopped")
