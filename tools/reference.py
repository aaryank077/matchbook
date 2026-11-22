"""Reference model of the matching engine, written for clarity rather than speed.

It implements the same rules as the C engine (price-time priority, trades at
the maker's price, market orders discard their remainder, same reject codes in
the same order) using plain dicts and deques. tools/fuzz.py runs random order
flow through both and requires byte-identical output.
"""
from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field


@dataclass
class Book:
    max_price: int
    max_orders: int
    # price -> deque of [order_id, remaining_qty], oldest first
    bids: dict[int, deque] = field(default_factory=dict)
    asks: dict[int, deque] = field(default_factory=dict)
    # order_id -> (side, price) for resting orders
    resting: dict[int, tuple[str, int]] = field(default_factory=dict)

    def _match(self, taker_id: int, side: str, limit: int | None, qty: int, out: list[str]) -> int:
        book = self.asks if side == "B" else self.bids
        while qty > 0 and book:
            price = min(book) if side == "B" else max(book)
            if limit is not None and (price > limit if side == "B" else price < limit):
                break
            queue = book[price]
            while qty > 0 and queue:
                maker = queue[0]
                fill = min(maker[1], qty)
                maker[1] -= fill
                qty -= fill
                out.append(f"T {maker[0]} {taker_id} {price} {fill}")
                if maker[1] == 0:
                    queue.popleft()
                    del self.resting[maker[0]]
            if not queue:
                del book[price]
        return qty

    def limit(self, oid: int, side: str, price: int, qty: int, out: list[str]) -> None:
        if oid == 0:
            return out.append(f"R {oid} BAD_ID")
        if qty == 0:
            return out.append(f"R {oid} BAD_QTY")
        if not 1 <= price <= self.max_price:
            return out.append(f"R {oid} BAD_PRICE")
        if oid in self.resting:
            return out.append(f"R {oid} DUP_ID")
        remaining = self._match(oid, side, price, qty, out)
        if remaining == 0:
            return None
        if len(self.resting) >= self.max_orders:
            return out.append(f"R {oid} FULL")
        book = self.bids if side == "B" else self.asks
        book.setdefault(price, deque()).append([oid, remaining])
        self.resting[oid] = (side, price)
        return None

    def market(self, oid: int, side: str, qty: int, out: list[str]) -> None:
        if oid == 0:
            return out.append(f"R {oid} BAD_ID")
        if qty == 0:
            return out.append(f"R {oid} BAD_QTY")
        if oid in self.resting:
            return out.append(f"R {oid} DUP_ID")
        self._match(oid, side, None, qty, out)
        return None

    def cancel(self, oid: int, out: list[str]) -> None:
        if oid == 0:
            return out.append(f"R {oid} BAD_ID")
        if oid not in self.resting:
            return out.append(f"R {oid} UNKNOWN_ID")
        side, price = self.resting.pop(oid)
        book = self.bids if side == "B" else self.asks
        queue = book[price]
        for i, (rid, _) in enumerate(queue):
            if rid == oid:
                del queue[i]
                break
        if not queue:
            del book[price]
        return None

    def top(self, out: list[str]) -> None:
        bid = max(self.bids) if self.bids else 0
        ask = min(self.asks) if self.asks else 0
        bid_qty = sum(q for _, q in self.bids[bid]) if bid else 0
        ask_qty = sum(q for _, q in self.asks[ask]) if ask else 0
        out.append(f"BOOK {bid} {bid_qty} {ask} {ask_qty}")


def run(commands: list[str], max_price: int, max_orders: int) -> list[list[str]]:
    """Applies each command and returns the output lines it produced, one list per command."""
    book = Book(max_price, max_orders)
    per_command: list[list[str]] = []
    for line in commands:
        out: list[str] = []
        parts = line.split()
        kind = parts[0]
        if kind == "L":
            book.limit(int(parts[1]), parts[2], int(parts[3]), int(parts[4]), out)
        elif kind == "M":
            book.market(int(parts[1]), parts[2], int(parts[3]), out)
        elif kind == "C":
            book.cancel(int(parts[1]), out)
        elif kind == "P":
            book.top(out)
        per_command.append(out)
    return per_command
