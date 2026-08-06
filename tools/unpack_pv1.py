#!/usr/bin/env python3
"""
unpack_pv1.py — parse a Storyteller/3sigma `.pv1` firmware package and, optionally,
extract the application filesystem it contains.

This operates on YOUR OWN local copy of the firmware. It does not include, embed,
or redistribute any Storyteller/3sigma content — it just unpacks a file you supply.

Container format (little-endian), reconstructed by inspection:
    header magic: d4 c5 67 4e
    section table begins at 0x298; each entry is 32 bytes:
        +0x00 u32 id
        +0x04 u32 flagA
        +0x08 u32 flagB
        +0x0c u64 size     (lo@0x0c, hi@0x10)
        +0x14 u64 offset   (lo@0x14, hi@0x18)
        +0x1c u32 checksum (algorithm TBD; not verified here)
    Sections are stored contiguously (offset == previous offset+size).

Typical sections:
    id 2 : QNX boot image (IFS)
    id 3,4: zero-filled gzip placeholders
    id 5 : gzip -> POSIX tar = the application (app/ config/ os/ nvdata/)
    id 7 : Intel-HEX MCU firmware

Usage:
    python3 unpack_pv1.py FIRMWARE.pv1 --list
    python3 unpack_pv1.py FIRMWARE.pv1 --extract-app --out ./_scratch
    python3 unpack_pv1.py FIRMWARE.pv1 --dump-sections --out ./_scratch
"""
from __future__ import annotations

import argparse
import io
import os
import struct
import sys
import tarfile
import zlib

MAGIC = bytes.fromhex("d4c5674e")
TABLE_OFFSET = 0x298
ENTRY_SIZE = 0x20
GZIP_MAGIC = b"\x1f\x8b"


def parse_sections(data: bytes):
    """Yield dicts for each section table entry that points inside the file."""
    if data[:4] != MAGIC:
        print(f"warning: unexpected header magic {data[:4].hex()} "
              f"(expected {MAGIC.hex()})", file=sys.stderr)
    p = TABLE_OFFSET
    n = len(data)
    while p + ENTRY_SIZE <= n:
        sid, fa, fb, size_lo, size_hi, off_lo, off_hi, chk = \
            struct.unpack_from("<8I", data, p)
        size = size_lo | (size_hi << 32)
        off = off_lo | (off_hi << 32)
        # Stop when the entry no longer describes a real section.
        if sid == 0 or off == 0 or off > n or size == 0 or off + size > n:
            break
        yield {"hdr": p, "id": sid, "flagA": fa, "flagB": fb,
               "offset": off, "size": size, "checksum": chk}
        p += ENTRY_SIZE


def classify(data: bytes, sec) -> str:
    blob = data[sec["offset"]:sec["offset"] + min(sec["size"], 16)]
    if blob[:2] == GZIP_MAGIC:
        return "gzip"
    if blob[:7] == b"imagefs":
        return "qnx-ifs"
    if blob[:1] == b":" and all(c in b"0123456789abcdefABCDEF" for c in blob[1:9]):
        return "intel-hex"
    if not any(data[sec["offset"]:sec["offset"] + min(sec["size"], 4096)]):
        return "zero-filled"
    return "unknown"


def cmd_list(data: bytes):
    print(f"file size: {len(data)} bytes")
    print(f"{'id':>3} {'offset':>12} {'size':>12} {'end':>12}  kind")
    end = 0
    for s in parse_sections(data):
        end = s["offset"] + s["size"]
        print(f"{s['id']:>3} {s['offset']:>12} {s['size']:>12} {end:>12}  "
              f"{classify(data, s)}")
    trailing = len(data) - end
    if trailing:
        print(f"(+{trailing} trailing bytes)")


def gunzip(blob: bytes) -> bytes:
    d = zlib.decompressobj(16 + zlib.MAX_WBITS)
    out = d.decompress(blob)
    out += d.flush()
    return out


def cmd_extract_app(data: bytes, out_dir: str):
    for s in parse_sections(data):
        if classify(data, s) != "gzip":
            continue
        raw = gunzip(data[s["offset"]:s["offset"] + s["size"]])
        try:
            tf = tarfile.open(fileobj=io.BytesIO(raw))
        except tarfile.TarError:
            continue  # gzip section that isn't a tar (e.g. placeholder)
        dest = os.path.join(out_dir, "app_root")
        os.makedirs(dest, exist_ok=True)
        # filter='data' guards against path traversal / unsafe members.
        try:
            tf.extractall(dest, filter="data")
        except TypeError:  # Python < 3.12 has no filter kwarg
            tf.extractall(dest)
        print(f"section {s['id']}: extracted application tree -> {dest}")
        return
    print("no gzip'd tar section found", file=sys.stderr)
    sys.exit(1)


def cmd_dump_sections(data: bytes, out_dir: str):
    os.makedirs(out_dir, exist_ok=True)
    for s in parse_sections(data):
        path = os.path.join(out_dir, f"section{s['id']:02d}.bin")
        with open(path, "wb") as f:
            f.write(data[s["offset"]:s["offset"] + s["size"]])
        print(f"section {s['id']}: {s['size']} bytes -> {path} "
              f"({classify(data, s)})")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("firmware", help="path to the .pv1 file")
    ap.add_argument("--out", default="./_scratch", help="output directory")
    ap.add_argument("--list", action="store_true", help="list sections")
    ap.add_argument("--extract-app", action="store_true",
                    help="extract the application filesystem")
    ap.add_argument("--dump-sections", action="store_true",
                    help="write each raw section to --out")
    args = ap.parse_args()

    with open(args.firmware, "rb") as f:
        data = f.read()

    if not (args.list or args.extract_app or args.dump_sections):
        args.list = True
    if args.list:
        cmd_list(data)
    if args.dump_sections:
        cmd_dump_sections(data, args.out)
    if args.extract_app:
        cmd_extract_app(data, args.out)


if __name__ == "__main__":
    main()
