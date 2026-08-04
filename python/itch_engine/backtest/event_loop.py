"""Phase 3: the event-driven backtester.

A single `while`-style pass over the normalized event stream - never
vectorized - interleaving three time-ordered streams:

1. **Feed events** applied to the *exchange book* (ground truth).
2. **Strategy samples** on a fixed grid; the strategy is only ever shown the
   *strategy book*, a second C++ book fed the same events delayed by the
   data-latency, plus its own orders/position.
3. **Order arrivals**: strategy decisions reach the exchange after the
   order-latency and are then evaluated against the exchange book by the
   queue-position-aware fill model.

Two separate C++ OrderBook instances make the latency model exact rather
than approximate: the strategy literally cannot observe state newer than
`data_latency_ns`.

Modify events with a price change do move real volume between levels, but
the normalized event doesn't carry the old price, so they are ignored for
phantom-queue advancement (conservative: our simulated order never gains
queue position from them).
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Callable, List, Optional

import numpy as np
import pandas as pd

import itch_engine_cpp as cpp

from itch_engine.backtest.fill_model import FillModel, SimOrder, SIDE_ASK, SIDE_BID
from itch_engine.backtest.latency_model import LatencyModel

EVENT_CANCEL = 1
EVENT_EXECUTE = 3


@dataclass
class OrderRequest:
    side: int              # 0 buy, 1 sell
    qty: int
    price: Optional[int] = None   # None = join the touch (passive) / cross (aggressive)
    aggressive: bool = False


@dataclass
class BacktestResult:
    fills: pd.DataFrame
    orders: pd.DataFrame
    equity: pd.DataFrame
    snapshots: pd.DataFrame
    boundary_ns: np.ndarray       # sampled per-event apply_event wall times
    events_processed: int
    unknown_order_events: int
    elapsed_s: float


def run_backtest(
    events: pd.DataFrame,
    strategy: Callable,
    latency: LatencyModel,
    sample_interval_ns: int = 100_000_000,     # strategy clock: 100 ms
    snapshot_interval_ns: int = 1_000_000_000,  # book snapshots: 1 s
    boundary_sample_every: int = 64,
    warmup_ns: int = 60_000_000_000,           # let the book build for 60 s
    order_ttl_ns: int = 5_000_000_000,         # unfilled passive orders expire
    sample_window: tuple = None,               # (start_ns, end_ns): only sample
                                               # the strategy inside this window
                                               # (e.g. regular trading hours)
) -> BacktestResult:
    ts = events["ts"].to_numpy(np.int64)
    order_id = events["order_id"].to_numpy(np.uint64)
    etype = events["type"].to_numpy(np.uint8)
    side = events["side"].to_numpy(np.uint8)
    price = events["price"].to_numpy(np.int64)
    qty = events["qty"].to_numpy(np.int64)
    n = len(ts)

    exchange = cpp.OrderBook()
    delayed = cpp.OrderBook()

    fill_model = FillModel()
    pending: List[SimOrder] = []   # submitted, in flight to the exchange
    live: List[SimOrder] = []      # resting phantom orders
    next_ref = 1

    position = 0
    cash = 0.0
    fills_seen = 0

    equity_rows = []
    snapshot_rows = []
    boundary_samples = []

    start_ts = int(ts[0])
    next_sample = start_ts + warmup_ns
    sample_end = int(ts[-1])
    if sample_window is not None:
        next_sample = max(next_sample, int(sample_window[0]))
        sample_end = int(sample_window[1])
    next_snapshot = start_ts + warmup_ns

    j = 0  # delayed-book cursor
    t0 = time.perf_counter()

    for i in range(n):
        now = int(ts[i])

        # --- order arrivals and strategy samples due before this event ----
        while pending and min(o.arrival_ts for o in pending) <= now:
            order = min(pending, key=lambda o: o.arrival_ts)
            pending.remove(order)
            fill_model.activate(order, exchange)
            if order.status == "live":
                live.append(order)

        while next_sample <= now and next_sample <= sample_end:
            # Catch the strategy book up to what is visible at sample time.
            visible = latency.visible_before(next_sample)
            while j < n and ts[j] <= visible:
                delayed.apply_event(int(ts[j]), int(order_id[j]), int(etype[j]),
                                    int(side[j]), int(price[j]), int(qty[j]))
                j += 1

            # The strategy sees every order it has outstanding - both resting
            # (live) and still in flight (pending). Hiding in-flight orders
            # is how strategies double-submit and blow through size limits.
            requests = strategy(next_sample, delayed, position, live + pending)
            for req in requests or ():
                px = req.price
                best = delayed.best()
                if px is None:
                    if req.aggressive:  # cross the (stale) spread
                        px = best.ask_price if req.side == SIDE_BID else best.bid_price
                    else:               # join the (stale) touch
                        px = best.bid_price if req.side == SIDE_BID else best.ask_price
                    if px == 0:
                        continue        # empty side; nothing sane to quote
                order = SimOrder(
                    ref=next_ref, side=req.side, price=int(px), qty=req.qty,
                    decision_ts=next_sample,
                    arrival_ts=latency.order_arrival(next_sample),
                    aggressive=req.aggressive,
                )
                next_ref += 1
                pending.append(order)
            next_sample += sample_interval_ns

        while next_snapshot <= now:
            snap_ts = next_snapshot
            for s, name in ((SIDE_BID, "bid"), (SIDE_ASK, "ask")):
                for rank, level in enumerate(exchange.depth(s, 5)):
                    snapshot_rows.append(
                        (snap_ts, name, rank, level.price, level.qty, level.order_count)
                    )
            mid = exchange.mid_price()
            if mid is not None:
                equity_rows.append(
                    (snap_ts, mid, position, cash, cash + position * mid)
                )
            next_snapshot += snapshot_interval_ns

        # --- phantom-queue bookkeeping needs pre-event level state ---------
        ev_type = int(etype[i])
        if ev_type == EVENT_CANCEL and live:
            ev_side, ev_price = int(side[i]), int(price[i])
            for order in live:
                if order.side == ev_side and order.price == ev_price:
                    before = (exchange.bid_qty_at(ev_price) if ev_side == SIDE_BID
                              else exchange.ask_qty_at(ev_price))
                    fill_model.on_cancel(order, ev_side, ev_price, int(qty[i]), before)

        # --- apply the event to the exchange book (timed sample) -----------
        if i % boundary_sample_every == 0:
            c0 = time.perf_counter_ns()
            exchange.apply_event(now, int(order_id[i]), ev_type,
                                 int(side[i]), int(price[i]), int(qty[i]))
            boundary_samples.append(time.perf_counter_ns() - c0)
        else:
            exchange.apply_event(now, int(order_id[i]), ev_type,
                                 int(side[i]), int(price[i]), int(qty[i]))

        if ev_type == EVENT_EXECUTE and live:
            ev_side, ev_price = int(side[i]), int(price[i])
            for order in live:
                fill_model.on_execute(order, now, ev_side, ev_price, int(qty[i]))

        # Settle new fills into position/cash; drop dead orders.
        if len(fill_model.fills) > fills_seen:
            for f in fill_model.fills[fills_seen:]:
                signed = f.qty if f.side == SIDE_BID else -f.qty
                position += signed
                cash -= signed * (f.price / cpp.PRICE_SCALE)
            fills_seen = len(fill_model.fills)
        if live:
            for o in live:
                # TTL: a resting phantom order that never reached the front
                # expires instead of blocking the strategy all day.
                if o.status == "live" and now - o.arrival_ts > order_ttl_ns:
                    fill_model.cancel(o)
            live = [o for o in live if o.status == "live"]

    elapsed = time.perf_counter() - t0

    # Anything still resting at the close is cancelled, unfilled.
    for order in pending + live:
        fill_model.cancel(order)

    fills = pd.DataFrame(
        [(f.ts, f.order_ref, f.side, f.price, f.qty, f.aggressive,
          f.queue_ahead_at_join) for f in fill_model.fills],
        columns=["ts", "order_ref", "side", "price", "qty", "aggressive",
                 "queue_ahead_at_join"],
    )
    orders = pd.DataFrame(
        [(o.ref, o.decision_ts, o.arrival_ts, o.side, o.price, o.qty,
          o.aggressive, o.queue_ahead_at_join, o.filled_qty, o.status)
         for o in fill_model.completed],
        columns=["ref", "decision_ts", "arrival_ts", "side", "price", "qty",
                 "aggressive", "queue_ahead_at_join", "filled_qty", "status"],
    )
    equity = pd.DataFrame(
        equity_rows, columns=["ts", "mid", "position", "cash", "equity"]
    )
    snapshots = pd.DataFrame(
        snapshot_rows, columns=["ts", "side", "rank", "price", "qty", "order_count"]
    )
    return BacktestResult(
        fills=fills, orders=orders, equity=equity, snapshots=snapshots,
        boundary_ns=np.asarray(boundary_samples, dtype=np.int64),
        events_processed=exchange.events_processed,
        unknown_order_events=exchange.unknown_order_events,
        elapsed_s=elapsed,
    )
