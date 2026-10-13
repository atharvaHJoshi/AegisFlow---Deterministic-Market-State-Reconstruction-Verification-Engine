#!/usr/bin/env python3
"""Deterministic market-event generator and chaos/fault injector.

Produces a clean authoritative stream and, optionally, a fault-injected
"wire" stream (drops, duplicates, reordering, delay, corruption) derived from a
reproducible seed. The clean stream doubles as the retransmission source.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import sys
from typing import Dict, List

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "reference", "python"))
import model as ref  # noqa: E402


class LiveOrders:
    """O(1) random selection and swap-removal of live orders for one symbol."""

    def __init__(self):
        self.by_id: Dict[int, ref.Order] = {}
        self.ids: List[int] = []
        self.index: Dict[int, int] = {}

    def __len__(self):
        return len(self.ids)

    def add(self, order: ref.Order) -> None:
        self.by_id[order.order_id] = order
        self.index[order.order_id] = len(self.ids)
        self.ids.append(order.order_id)

    def random_order(self, rng: random.Random) -> ref.Order:
        return self.by_id[self.ids[rng.randrange(len(self.ids))]]

    def remove(self, order_id: int) -> None:
        pos = self.index.pop(order_id)
        last = self.ids.pop()
        if last != order_id:
            self.ids[pos] = last
            self.index[last] = pos
        del self.by_id[order_id]


# Keep the active book realistic and the generator linear: above this many live
# orders per symbol, prefer cancels over adds.
LIVE_CAP = 4096


def generate(count: int, symbols: List[str], seed: int) -> List[ref.Event]:
    rng = random.Random(seed)
    base_price = {sym: 10000 + i * 5000 for i, sym in enumerate(symbols)}
    live: Dict[str, LiveOrders] = {sym: LiveOrders() for sym in symbols}
    events: List[ref.Event] = []
    next_order_id = 1

    for seq in range(1, count + 1):
        symbol = rng.choice(symbols)
        base = base_price[symbol]
        open_orders = live[symbol]

        roll = rng.random()
        if len(open_orders) == 0 or (roll < 0.55 and len(open_orders) < LIVE_CAP):
            oid = next_order_id
            next_order_id += 1
            side = rng.choice([ref.BID, ref.ASK])
            price = max(1, base + rng.randint(-25, 25))
            qty = rng.randint(1, 100)
            ev = ref.Event(seq, 1, seq, symbol, ref.ORDER_ADD, seq * 1000, (oid, side, price, qty), 0)
            open_orders.add(ref.Order(oid, side, price, qty))
        elif roll < 0.78:
            order = open_orders.random_order(rng)
            if rng.random() < 0.3 and order.quantity > 1:
                qty = rng.randint(1, order.quantity - 1)
                order.quantity -= qty
            else:
                qty = ref.NO_QUANTITY
                open_orders.remove(order.order_id)
            ev = ref.Event(seq, 1, seq, symbol, ref.ORDER_CANCEL, seq * 1000,
                           (order.order_id, qty), 0)
        elif roll < 0.9:
            order = open_orders.random_order(rng)
            if rng.random() < 0.5:
                new_price = max(1, base + rng.randint(-25, 25))
                order.price = new_price
            else:
                new_price = ref.NO_PRICE
            new_qty = rng.randint(1, 100)
            order.quantity = new_qty
            ev = ref.Event(seq, 1, seq, symbol, ref.ORDER_MODIFY, seq * 1000,
                           (order.order_id, new_price, new_qty), 0)
        else:
            order = open_orders.random_order(rng)
            qty = rng.randint(1, 50)
            ev = ref.Event(seq, 1, seq, symbol, ref.TRADE, seq * 1000,
                           (order.order_id, order.order_id, base, qty), 0)

        events.append(ev)
    return events


def apply_chaos(events: List[ref.Event], faults: Dict[str, float], seed: int):
    rng = random.Random(seed ^ 0x5EED)
    lines = [ref.encode_event(ev) for ev in events]
    dropped, duplicated, corrupted = [], [], []
    wire: List[str] = []

    drop = faults.get("drop", 0.0)
    dup = faults.get("dup", 0.0)
    reorder = faults.get("reorder", 0.0)
    delay = faults.get("delay", 0.0)
    corrupt = faults.get("corrupt", 0.0)

    for i, line in enumerate(lines):
        if rng.random() < drop:
            dropped.append(events[i].seq)
            continue
        wire.append(line)
        if rng.random() < dup:
            wire.append(line)
            duplicated.append(events[i].seq)

    # Reordering: local adjacent swaps.
    for i in range(len(wire) - 1):
        if rng.random() < reorder:
            wire[i], wire[i + 1] = wire[i + 1], wire[i]

    # Delay: push a few events later in the delivery order.
    for i in range(len(wire)):
        if rng.random() < delay and i + 3 < len(wire):
            item = wire.pop(i)
            wire.insert(i + 3, item)

    # Corruption: alter a value without fixing the checksum.
    out: List[str] = []
    for line in wire:
        if rng.random() < corrupt and line:
            tokens = line.split(" ")
            # mutate the last payload field, leaving the checksum stale
            idx = max(6, len(tokens) - 2)
            try:
                tokens[idx] = str(int(tokens[idx]) + 1)
                line = " ".join(tokens)
                corrupted.append(int(tokens[2]))
            except ValueError:
                pass
        out.append(line)

    scenario = {
        "dropped": len(dropped),
        "duplicated": len(duplicated),
        "corrupted": len(corrupted),
        "dropped_seqs": dropped[:50],
        "duplicated_seqs": duplicated[:50],
        "corrupted_seqs": corrupted[:50],
        "fault_rates": faults,
        "seed": seed,
        "wire_events": len(out),
        "clean_events": len(events),
    }
    return out, scenario


def write_lines(path: str, lines: List[str]) -> None:
    with open(path, "w") as handle:
        handle.write("\n".join(lines))
        if lines:
            handle.write("\n")


def parse_faults(spec: str) -> Dict[str, float]:
    faults = {"drop": 0.0, "dup": 0.0, "reorder": 0.0, "delay": 0.0, "corrupt": 0.0}
    for item in spec.split(","):
        item = item.strip()
        if not item:
            continue
        key, _, value = item.partition("=")
        key = key.strip()
        if key not in faults:
            raise SystemExit(f"unknown fault: {key}")
        faults[key] = float(value)
    return faults


def main() -> int:
    parser = argparse.ArgumentParser(description="MarketState event generator")
    parser.add_argument("--events", type=int, default=1000)
    parser.add_argument("--symbols", default="AAPL,MSFT")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--out", required=True, help="clean stream path")
    parser.add_argument("--faulty-out", help="fault-injected stream path")
    parser.add_argument("--scenario-out", help="scenario JSON path")
    parser.add_argument("--faults", default="", help="drop=0.02,dup=0.02,reorder=0.03,corrupt=0.01")
    args = parser.parse_args()

    symbols = [s.strip() for s in args.symbols.split(",") if s.strip()]
    events = generate(args.events, symbols, args.seed)
    clean_lines = [ref.encode_event(ev) for ev in events]
    write_lines(args.out, clean_lines)

    summary = {"events": len(events), "symbols": symbols, "seed": args.seed, "clean": args.out}

    if args.faulty_out:
        faults = parse_faults(args.faults) if args.faults else {}
        wire, scenario = apply_chaos(events, faults, args.seed)
        write_lines(args.faulty_out, wire)
        summary["faulty"] = args.faulty_out
        summary["scenario"] = scenario
        if args.scenario_out:
            with open(args.scenario_out, "w") as handle:
                json.dump(scenario, handle, indent=2)

    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
