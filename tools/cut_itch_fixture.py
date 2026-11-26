"""Cuts a small ITCH 5.0 fixture for CI out of a full trading day.

Keeps system events, the stock directory entries for the chosen symbols, and
every book message (add, execute, cancel, delete, replace) for them, in their
original order and byte for byte. The result replays exactly like those
symbols did in the full file.

    python3 tools/cut_itch_fixture.py data/day.NASDAQ_ITCH50 --symbols TWTR,GILD -o fixture.itch
"""
from __future__ import annotations

import argparse
import mmap
import struct
import sys

KEEP_FOR_SYMBOL = b"AFECXDU"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--symbols", required=True)
    ap.add_argument("-o", "--out", required=True)
    args = ap.parse_args()
    symbols = {s.encode() for s in args.symbols.split(",")}

    u16 = struct.Struct(">H")
    locates: set[int] = set()
    kept = 0
    with open(args.file, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as buf, open(
        args.out, "wb"
    ) as out:
        off, end = 0, len(buf)
        while off + 2 <= end:
            (length,) = u16.unpack_from(buf, off)
            m = off + 2
            kind = buf[m]
            keep = kind == ord("S")
            if kind == ord("R") and bytes(buf[m + 11 : m + 19]).rstrip(b" ") in symbols:
                locates.add(u16.unpack_from(buf, m + 1)[0])
                keep = True
            elif kind in KEEP_FOR_SYMBOL and u16.unpack_from(buf, m + 1)[0] in locates:
                keep = True
            if keep:
                out.write(buf[off : m + length])
                kept += 1
            off = m + length
    print(f"kept {kept} messages for {len(locates)} symbols in {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
