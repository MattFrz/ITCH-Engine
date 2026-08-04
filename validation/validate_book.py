"""Phase 2 deliverable: prove the C++ book is correct - 0 drift or fail.

Replays the normalized day through the C++ engine and, in lockstep, through
an independently written pure-Python reference book (different code path,
different data structures, shared only the event semantics). At 1,000
random timestamps it asserts exact agreement on:

* top-5 price levels per side: price, aggregate qty, order count
* best bid/ask
* number of open orders
* FIFO queue position (`queue_ahead`) for up to 5 sampled live orders

Any single mismatch fails the run. The summary is written to
validation/results/validation_run.txt.

When real Databento data is used, drop reference `mbp-1` snapshots into
validation/reference_snapshots/ as Parquet (columns: ts_recv, bid_px_00,
bid_sz_00, ask_px_00, ask_sz_00); extending the checkpoint comparison to
those exchange-published snapshots is a straight swap of the reference.

Usage:
    python validation/validate_book.py [--symbol AAPL] [--day 2026-07-01]
        [--checkpoints 1000] [--seed 3]
"""

from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "python"))

import itch_engine_cpp as cpp  # noqa: E402

from itch_engine.ingest.databento_client import fetch_day  # noqa: E402
from itch_engine.ingest.normalize import load_events, normalize_day  # noqa: E402

RESULTS_DIR = REPO_ROOT / "validation" / "results"


class ReferenceBook:
    """Deliberately naive per-order book: dicts and lists, no cleverness.

    Cancel-in-the-middle here is O(N) via list.remove - fine, because this
    book only exists to be obviously correct, not fast. Sharing nothing
    structurally with the C++ implementation is what makes agreement
    meaningful.
    """

    def __init__(self):
        self.orders = {}                    # oid -> [side, price, qty]
        self.queues = defaultdict(list)     # (side, price) -> [oid,...] FIFO

    def apply(self, oid, etype, side, price, qty):
        if etype == 0:  # Add
            if qty <= 0 or oid in self.orders:
                return
            self.orders[oid] = [side, price, qty]
            self.queues[(side, price)].append(oid)
        elif etype == 1:  # Cancel (partial keeps position; qty<=0 = full)
            o = self.orders.get(oid)
            if o is None:
                return
            take = o[2] if (qty <= 0 or qty >= o[2]) else qty
            o[2] -= take
            if o[2] == 0:
                self._drop(oid, o)
        elif etype == 2:  # Modify
            o = self.orders.get(oid)
            if o is None:
                return
            if price == o[1] and qty <= o[2]:
                o[2] = qty          # size-down keeps priority
                if qty == 0:
                    self._drop(oid, o)
            else:                    # reprice/size-up -> back of new queue
                self.queues[(o[0], o[1])].remove(oid)
                if not self.queues[(o[0], o[1])]:
                    del self.queues[(o[0], o[1])]
                if qty <= 0:
                    del self.orders[oid]
                else:
                    o[1] = price
                    o[2] = qty
                    self.queues[(o[0], price)].append(oid)
        elif etype == 3:  # Execute
            o = self.orders.get(oid)
            if o is None:
                return
            fill = min(max(qty, 0), o[2])
            if fill <= 0:
                return
            o[2] -= fill
            if o[2] == 0:
                self._drop(oid, o)

    def _drop(self, oid, o):
        key = (o[0], o[1])
        self.queues[key].remove(oid)
        if not self.queues[key]:
            del self.queues[key]
        del self.orders[oid]

    def top_levels(self, side, n=5):
        prices = sorted(
            (p for (s, p) in self.queues if s == side),
            reverse=(side == 0),
        )[:n]
        out = []
        for p in prices:
            q = self.queues[(side, p)]
            out.append((p, sum(self.orders[o][2] for o in q), len(q)))
        return out

    def queue_ahead(self, oid):
        o = self.orders.get(oid)
        if o is None:
            return None
        ahead = 0
        for other in self.queues[(o[0], o[1])]:
            if other == oid:
                return ahead
            ahead += self.orders[other][2]
        return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbol", default="AAPL")
    ap.add_argument("--day", default="2026-07-01")
    ap.add_argument("--checkpoints", type=int, default=1000)
    ap.add_argument("--seed", type=int, default=3)
    args = ap.parse_args()

    raw = fetch_day(args.symbol, args.day)
    normalize_day(raw, args.symbol, args.day)
    events = load_events(args.symbol, args.day)

    ts = events["ts"].to_numpy(np.int64)
    order_id = events["order_id"].to_numpy(np.uint64)
    etype = events["type"].to_numpy(np.uint8)
    side = events["side"].to_numpy(np.uint8)
    price = events["price"].to_numpy(np.int64)
    qty = events["qty"].to_numpy(np.int64)
    n = len(ts)

    rng = np.random.default_rng(args.seed)
    checkpoints = np.sort(rng.choice(np.arange(n // 100, n),
                                     size=min(args.checkpoints, n - n // 100),
                                     replace=False))
    cp_iter = iter(checkpoints.tolist())
    next_cp = next(cp_iter)

    book = cpp.OrderBook()
    ref = ReferenceBook()

    checks = 0
    mismatches = []

    def compare(i):
        nonlocal checks
        for s in (0, 1):
            got = [(lv.price, lv.qty, lv.order_count) for lv in book.depth(s, 5)]
            want = ref.top_levels(s, 5)
            checks += 1
            if got != want:
                mismatches.append((i, f"side {s} depth", want, got))
        checks += 1
        if book.open_order_count != len(ref.orders):
            mismatches.append((i, "open orders", len(ref.orders),
                               book.open_order_count))
        if ref.orders:
            oids = rng.choice(np.fromiter(ref.orders.keys(), dtype=np.uint64,
                                          count=len(ref.orders)),
                              size=min(5, len(ref.orders)), replace=False)
            for oid in oids:
                checks += 1
                if book.queue_ahead(int(oid)) != ref.queue_ahead(int(oid)):
                    mismatches.append((i, f"queue_ahead({oid})",
                                       ref.queue_ahead(int(oid)),
                                       book.queue_ahead(int(oid))))

    for i in range(n):
        book.apply_event(int(ts[i]), int(order_id[i]), int(etype[i]),
                         int(side[i]), int(price[i]), int(qty[i]))
        ref.apply(int(order_id[i]), int(etype[i]), int(side[i]),
                  int(price[i]), int(qty[i]))
        if next_cp is not None and i == next_cp:
            compare(i)
            next_cp = next(cp_iter, None)

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    lines = [
        f"symbol={args.symbol} day={args.day}",
        f"events replayed: {n:,}",
        f"checkpoints: {len(checkpoints):,} random timestamps",
        f"assertions: {checks:,}",
        f"mismatches: {len(mismatches)}",
    ]
    if mismatches:
        lines.append("RESULT: FAIL")
        for m in mismatches[:20]:
            lines.append(f"  event {m[0]}: {m[1]}: want={m[2]} got={m[3]}")
    else:
        lines.append("RESULT: PASS - 0 drift across all sampled timestamps")

    report = "\n".join(lines)
    (RESULTS_DIR / "validation_run.txt").write_text(report + "\n")
    print(report)
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
