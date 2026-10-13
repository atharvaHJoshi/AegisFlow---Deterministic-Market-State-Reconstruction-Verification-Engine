#!/usr/bin/env python3
"""Independent reference model for MarketState.

This is intentionally implemented in a different style from the C++ engine:
price levels are recomputed from the raw order set at serialization time rather
than maintained incrementally. Feeding the same authoritative event history to
both implementations and comparing canonical states is the project's
correctness oracle.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
MASK64 = (1 << 64) - 1

ORDER_ADD, ORDER_CANCEL, ORDER_MODIFY, TRADE, MARKET_UPDATE = range(5)
BID, ASK = 0, 1
NO_PRICE = -1
NO_QUANTITY = -1

PAYLOAD_TOKENS = {
    ORDER_ADD: 4,
    ORDER_CANCEL: 2,
    ORDER_MODIFY: 3,
    TRADE: 4,
    MARKET_UPDATE: 2,
}


def fnv1a(value: str) -> int:
    h = FNV_OFFSET
    for byte in value.encode("ascii"):
        h ^= byte
        h = (h * FNV_PRIME) & MASK64
    return h


@dataclass
class Event:
    event_id: int
    stream_id: int
    seq: int
    symbol: str
    type: int
    timestamp: int
    payload: Tuple[int, ...]
    checksum: int
    valid: bool = True
    error: str = ""


def encode_event(ev: Event) -> str:
    header = f"{ev.event_id} {ev.stream_id} {ev.seq} {ev.symbol} {ev.type} {ev.timestamp}"
    payload = " ".join(str(x) for x in ev.payload)
    base = f"{header} {payload}"
    return f"{base} {fnv1a(base)}"


def parse_event(line: str) -> Optional[Event]:
    tokens = line.split()
    if len(tokens) < 8:
        return None
    try:
        event_id = int(tokens[0])
        stream_id = int(tokens[1])
        seq = int(tokens[2])
        symbol = tokens[3]
        etype = int(tokens[4])
        timestamp = int(tokens[5])
        checksum = int(tokens[-1])
    except ValueError:
        return None

    need = PAYLOAD_TOKENS.get(etype)
    if need is None or len(tokens) != 7 + need:
        return Event(event_id, stream_id, seq, symbol, etype, timestamp, (), checksum,
                     valid=False, error="wrong payload arity")

    try:
        payload = tuple(int(x) for x in tokens[6:-1])
    except ValueError:
        return Event(event_id, stream_id, seq, symbol, etype, timestamp, (), checksum,
                     valid=False, error="bad payload")

    covered = " ".join(tokens[:-1])
    if fnv1a(covered) != checksum:
        return Event(event_id, stream_id, seq, symbol, etype, timestamp, payload, checksum,
                     valid=False, error="checksum mismatch")

    if symbol == "":
        return Event(event_id, stream_id, seq, symbol, etype, timestamp, payload, checksum,
                     valid=False, error="empty symbol")
    if seq <= 0:
        return Event(event_id, stream_id, seq, symbol, etype, timestamp, payload, checksum,
                     valid=False, error="non-positive sequence")
    return Event(event_id, stream_id, seq, symbol, etype, timestamp, payload, checksum)


@dataclass
class Order:
    order_id: int
    side: int
    price: int
    quantity: int


class ReferenceBook:
    def __init__(self, symbol: str):
        self.symbol = symbol
        self.orders: Dict[int, Order] = {}
        self.total_volume = 0
        self.total_trades = 0
        self.last_seq = 0

    def apply(self, ev: Event) -> None:
        self.last_seq = max(self.last_seq, ev.seq)
        if ev.type == ORDER_ADD:
            oid, side, price, qty = ev.payload
            if oid in self.orders:
                return
            if price < 0 or qty <= 0:
                return
            self.orders[oid] = Order(oid, side, price, qty)
        elif ev.type == ORDER_CANCEL:
            oid, qty = ev.payload
            order = self.orders.get(oid)
            if order is None:
                return
            remaining = order.quantity if qty == NO_QUANTITY else min(qty, order.quantity)
            order.quantity -= remaining
            if order.quantity <= 0:
                del self.orders[oid]
        elif ev.type == ORDER_MODIFY:
            oid, new_price, new_qty = ev.payload
            order = self.orders.get(oid)
            if order is None:
                return
            if new_price != NO_PRICE:
                order.price = new_price
            if new_qty != NO_QUANTITY:
                order.quantity = new_qty
            if order.quantity <= 0:
                del self.orders[oid]
        elif ev.type == TRADE:
            _buy, _sell, _price, qty = ev.payload
            self.total_volume += qty
            self.total_trades += 1
        elif ev.type == MARKET_UPDATE:
            pass

    def canonical_state(self) -> str:
        levels: Dict[int, Dict[int, int]] = {BID: {}, ASK: {}}
        active = []
        for order in self.orders.values():
            if order.quantity <= 0:
                continue
            active.append(order)
            levels[order.side][order.price] = levels[order.side].get(order.price, 0) + order.quantity
        active.sort(key=lambda o: o.order_id)

        lines = [
            f"STATE {self.symbol}",
            f"SEQ {self.last_seq}",
            f"VOL {self.total_volume}",
            f"TRD {self.total_trades}",
            f"LEVELS {len(levels[BID])} {len(levels[ASK])}",
        ]
        for order in active:
            lines.append(f"O {order.order_id} {order.side} {order.price} {order.quantity}")
        lines.append("END")
        return "\n".join(lines)


class ReferenceModel:
    """Applies the authoritative ordered event history and serializes state."""

    BOOK_SEPARATOR = "\n===BOOK===\n"

    def __init__(self):
        self.books: Dict[str, ReferenceBook] = {}

    def apply(self, ev: Event) -> bool:
        if not ev.valid:
            return False
        book = self.books.get(ev.symbol)
        if book is None:
            book = ReferenceBook(ev.symbol)
            self.books[ev.symbol] = book
        book.apply(ev)
        return True

    def canonical_state(self) -> str:
        return self.BOOK_SEPARATOR.join(
            self.books[sym].canonical_state() for sym in sorted(self.books)
        )


def state_hash(state: str) -> str:
    return f"{fnv1a(state) & MASK64:016x}"


def load_events(path: str) -> List[Event]:
    events: List[Event] = []
    with open(path, "r") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            ev = parse_event(line)
            if ev is not None:
                events.append(ev)
    return events


def main() -> int:
    parser = argparse.ArgumentParser(description="MarketState reference model")
    parser.add_argument("--events", required=True, help="canonical event stream")
    parser.add_argument("--state-out", help="write canonical state here")
    parser.add_argument("--summary-out", help="write JSON summary here")
    args = parser.parse_args()

    events = load_events(args.events)
    model = ReferenceModel()
    applied = 0
    for ev in events:
        if model.apply(ev):
            applied += 1

    state = model.canonical_state()
    if args.state_out:
        with open(args.state_out, "w") as handle:
            handle.write(state)
    summary = {
        "events_read": len(events),
        "events_applied": applied,
        "symbols": sorted(model.books),
        "state_hash": state_hash(state),
        "state_bytes": len(state),
    }
    if args.summary_out:
        with open(args.summary_out, "w") as handle:
            json.dump(summary, handle)
    print(json.dumps(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
