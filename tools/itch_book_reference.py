"""Reference ITCH 5.0 book rebuilder, written for clarity rather than speed.

It decodes the file with `struct` (no shared code with cpp/itch) and keeps
each book as plain dicts and heaps (no shared code with the C engine), then
applies the same documented rules as cpp/replay/replayer.hpp: orders that
aren't a positive whole cent, are above the price cap, or would lock/cross the
book are kept outside it. It writes the top of book whenever it changes, in
the same format as `itch_replay --tob`, and can compare against that file.

    python3 tools/itch_book_reference.py --symbols AAPL,SPY FILE --compare build/tob.csv
"""
from __future__ import annotations

import argparse
import heapq
import mmap
import struct
import sys
from dataclasses import dataclass, field

BOOK_TYPES = b"AFECXDU"


@dataclass
class Order:
    side: str  # "B" or "S"
    price: int  # ITCH units, 4 decimals
    qty: int
    in_book: bool


@dataclass
class Book:
    symbol: str
    max_cents: int
    orders: dict[int, Order] = field(default_factory=dict)
    levels: dict[str, dict[int, int]] = field(default_factory=lambda: {"B": {}, "S": {}})  # cents -> qty
    heaps: dict[str, list[int]] = field(default_factory=lambda: {"B": [], "S": []})  # bids negated
    violations: dict[str, int] = field(default_factory=dict)
    outside: dict[str, int] = field(default_factory=lambda: {"off_grid": 0, "above_cap": 0, "would_cross": 0})
    last_top: tuple[int, int, int, int] = (0, 0, 0, 0)

    def violation(self, kind: str) -> None:
        self.violations[kind] = self.violations.get(kind, 0) + 1

    def best(self, side: str) -> int:
        heap, levels = self.heaps[side], self.levels[side]
        while heap:
            price = -heap[0] if side == "B" else heap[0]
            if levels.get(price, 0) > 0:
                return price
            heapq.heappop(heap)  # stale entry for an emptied level
        return 0

    def top(self) -> tuple[int, int, int, int]:
        bid, ask = self.best("B"), self.best("S")
        return bid, self.levels["B"].get(bid, 0), ask, self.levels["S"].get(ask, 0)

    def add(self, ref: int, side: str, price: int, qty: int) -> None:
        if ref in self.orders:
            return self.violation("duplicate_ref")
        cents = price // 100
        reason = None
        if price % 100 or cents < 1:
            reason = "off_grid"
        elif cents > self.max_cents:
            reason = "above_cap"
        else:
            opposite = self.best("S" if side == "B" else "B")
            if opposite and (cents >= opposite if side == "B" else cents <= opposite):
                reason = "would_cross"
        if reason:
            self.outside[reason] += 1
            self.orders[ref] = Order(side, price, qty, in_book=False)
            return None
        self.orders[ref] = Order(side, price, qty, in_book=True)
        levels = self.levels[side]
        if levels.get(cents, 0) == 0:
            heapq.heappush(self.heaps[side], -cents if side == "B" else cents)
        levels[cents] = levels.get(cents, 0) + qty
        return None

    def reduce(self, ref: int, qty: int, check_best: bool) -> None:
        o = self.orders.get(ref)
        if o is None:
            return self.violation("unknown_ref")
        if qty > o.qty:
            return self.violation("over_reduce")
        if o.in_book:
            cents = o.price // 100
            if check_best and cents != self.best(o.side):
                self.violation("exec_not_at_best")
            self.levels[o.side][cents] -= qty
        o.qty -= qty
        if o.qty == 0:
            del self.orders[ref]
        return None

    def delete(self, ref: int) -> Order | None:
        o = self.orders.pop(ref, None)
        if o is None:
            self.violation("unknown_ref")
        elif o.in_book:
            self.levels[o.side][o.price // 100] -= o.qty
        return o


def replay(buf, symbols: set[str], max_cents: int):
    """Yields (timestamp, book) after every book message for a tracked symbol."""
    books: dict[int, Book] = {}
    off, end = 0, len(buf)
    u16, u32, u64 = struct.Struct(">H"), struct.Struct(">I"), struct.Struct(">Q")
    while off + 2 <= end:
        (length,) = u16.unpack_from(buf, off)
        m = off + 2
        off = m + length
        kind = buf[m]
        if kind == ord("R"):
            symbol = bytes(buf[m + 11 : m + 19]).rstrip(b" ").decode()
            (locate,) = u16.unpack_from(buf, m + 1)
            if symbol in symbols and locate not in books:
                books[locate] = Book(symbol, max_cents)
            continue
        if kind not in BOOK_TYPES:
            continue
        (locate,) = u16.unpack_from(buf, m + 1)
        book = books.get(locate)
        if book is None:
            continue
        ts = int.from_bytes(buf[m + 5 : m + 11], "big")
        (ref,) = u64.unpack_from(buf, m + 11)
        t = chr(kind)
        if t in "AF":
            side = chr(buf[m + 19])
            (shares,) = u32.unpack_from(buf, m + 20)
            (price,) = u32.unpack_from(buf, m + 32)
            book.add(ref, side, price, shares)
        elif t in "ECX":
            (shares,) = u32.unpack_from(buf, m + 19)
            book.reduce(ref, shares, check_best=(t == "E"))
        elif t == "D":
            book.delete(ref)
        elif t == "U":
            (new_ref,) = u64.unpack_from(buf, m + 19)
            (shares,) = u32.unpack_from(buf, m + 27)
            (price,) = u32.unpack_from(buf, m + 31)
            old = book.delete(ref)
            if old is not None:
                book.add(new_ref, old.side, price, shares)
        yield ts, book


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--symbols", required=True)
    ap.add_argument("--max-price", type=int, default=2000, help="dollars, like itch_replay")
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--compare", help="itch_replay --tob output to compare against")
    group.add_argument("--out", help="write this model's top-of-book lines here")
    args = ap.parse_args()

    symbols = set(args.symbols.split(","))
    theirs = open(args.compare) if args.compare else None
    out = open(args.out, "w") if args.out else None
    lines = mismatches = 0
    books: dict[str, Book] = {}

    with open(args.file, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as buf:
        for ts, book in replay(buf, symbols, args.max_price * 100):
            books[book.symbol] = book
            top = book.top()
            if top == book.last_top:
                continue
            book.last_top = top
            line = f"{ts},{book.symbol},{top[0]},{top[1]},{top[2]},{top[3]}\n"
            lines += 1
            if out:
                out.write(line)
            elif theirs:
                other = theirs.readline()
                if other != line:
                    mismatches += 1
                    if mismatches <= 5:
                        print(f"line {lines}: reference {line.strip()!r}  itch_replay {other.strip()!r}")

    for b in sorted(books.values(), key=lambda b: b.symbol):
        print(f"{b.symbol}: {len(b.orders)} resting, outside {b.outside}, violations {b.violations or 0}")
    if theirs:
        extra = sum(1 for _ in theirs)
        print(f"{lines} top-of-book changes, {mismatches} mismatches, {extra} extra lines from itch_replay")
        return 1 if mismatches or extra else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
