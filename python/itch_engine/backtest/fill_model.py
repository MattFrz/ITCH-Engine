"""Queue-position-aware fill model.

The point of this module is the difference between "the price touched my
limit, so I'm filled" (the naive assumption) and what actually happens: a
passive order joins the *back* of a FIFO queue and only fills after the
volume queued ahead of it has traded.

Model, per simulated passive order:

* At exchange arrival, `queue_ahead` is the aggregate resting quantity at
  its price level on its side (we join the back).
* Feed **Execute** events at that level advance the queue: executed volume
  first burns `queue_ahead`, and any excess fills our order.
* Feed **Cancel** events at that level shrink `queue_ahead` by the *expected*
  fraction ahead of us (`cancel_qty * queue_ahead / level_qty`) - the
  standard expected-queue-position approximation, since MBO cancels don't
  tell us whether the cancelled order was ahead of a phantom order.

Marketable orders fill immediately by walking real book depth (slippage is
whatever the book says it is, not a constant).

Simulated volume is phantom: it never mutates the replayed book. That keeps
the reconstruction exact and makes the model conservative for small sizes.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional

SIDE_BID = 0
SIDE_ASK = 1


@dataclass
class Fill:
    ts: int
    order_ref: int
    side: int          # side of OUR order (0 buy/bid, 1 sell/ask)
    price: int         # fixed-point
    qty: int
    aggressive: bool
    queue_ahead_at_join: int


@dataclass
class SimOrder:
    ref: int
    side: int          # 0 = buy (joins bid queue), 1 = sell (joins ask queue)
    price: int
    qty: int
    decision_ts: int
    arrival_ts: int
    aggressive: bool = False
    queue_ahead: float = 0.0
    queue_ahead_at_join: int = 0
    filled_qty: int = 0
    status: str = "pending"   # pending -> live -> filled | cancelled
    cancel_arrival_ts: Optional[int] = None

    @property
    def remaining(self) -> int:
        return self.qty - self.filled_qty


@dataclass
class FillModel:
    fills: List[Fill] = field(default_factory=list)
    completed: List[SimOrder] = field(default_factory=list)

    def activate(self, order: SimOrder, book) -> None:
        """Called when the order reaches the exchange (post order-latency)."""
        if order.aggressive:
            self._fill_marketable(order, book)
            return
        level_qty = (
            book.bid_qty_at(order.price) if order.side == SIDE_BID
            else book.ask_qty_at(order.price)
        )
        order.queue_ahead = float(level_qty)
        order.queue_ahead_at_join = int(level_qty)
        order.status = "live"

    def _fill_marketable(self, order: SimOrder, book) -> None:
        """Walks real depth on the opposite side; partial if the book is thin."""
        opposite = SIDE_ASK if order.side == SIDE_BID else SIDE_BID
        need = order.remaining
        for level in book.depth(opposite, 10):
            if need <= 0:
                break
            # A marketable buy lifts asks at/below its limit; sell symmetric.
            if order.side == SIDE_BID and level.price > order.price:
                break
            if order.side == SIDE_ASK and level.price < order.price:
                break
            take = min(need, level.qty)
            self.fills.append(Fill(
                ts=order.arrival_ts, order_ref=order.ref, side=order.side,
                price=level.price, qty=take, aggressive=True,
                queue_ahead_at_join=0,
            ))
            order.filled_qty += take
            need -= take
        order.status = "filled" if order.remaining == 0 else "cancelled"
        self.completed.append(order)

    def on_execute(self, order: SimOrder, ev_ts: int, ev_side: int,
                   ev_price: int, ev_qty: int) -> None:
        """Feed execution at a price level; advances queues, may fill us."""
        if order.status != "live" or order.aggressive:
            return
        if ev_side != order.side or ev_price != order.price:
            return
        burn = min(float(ev_qty), order.queue_ahead)
        order.queue_ahead -= burn
        spill = int(ev_qty - burn)
        if spill > 0 and order.remaining > 0:
            fill_qty = min(spill, order.remaining)
            order.filled_qty += fill_qty
            self.fills.append(Fill(
                ts=ev_ts, order_ref=order.ref, side=order.side,
                price=order.price, qty=fill_qty, aggressive=False,
                queue_ahead_at_join=order.queue_ahead_at_join,
            ))
            if order.remaining == 0:
                order.status = "filled"
                self.completed.append(order)

    def on_cancel(self, order: SimOrder, ev_side: int, ev_price: int,
                  ev_qty: int, level_qty_before: int) -> None:
        """Feed cancel at our level: expected-position queue decrement."""
        if order.status != "live" or order.aggressive:
            return
        if ev_side != order.side or ev_price != order.price:
            return
        if level_qty_before <= 0:
            return
        frac_ahead = min(order.queue_ahead / float(level_qty_before), 1.0)
        order.queue_ahead = max(order.queue_ahead - ev_qty * frac_ahead, 0.0)

    def cancel(self, order: SimOrder) -> None:
        """Strategy cancel taking effect at the exchange."""
        if order.status in ("pending", "live"):
            order.status = "cancelled"
            self.completed.append(order)
