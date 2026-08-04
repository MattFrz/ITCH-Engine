"""Phase 4: the deliberately-dumb test client.

A single-feature order flow imbalance (OFI, Cont-Kukanov-Stoikov event
formulation) signal predicting the next ~1s mid move. It exists to produce
one comparison number - naive vs. realistic backtest of the *same* decisions
- not to be a good signal.

Three pieces:

* `build_quote_series` - replay the day through the C++ book and record the
  best quote after every event (input to the naive path and signal decay).
* `naive_backtest` - the textbook sin: vectorized, filled at mid, zero
  latency, zero queue. This is the baseline the engine exists to debunk.
* `OFIStrategy` - the same signal run live inside the event loop, where it
  must join real FIFO queues and act on stale data.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field

import numpy as np
import pandas as pd

import itch_engine_cpp as cpp

from itch_engine.backtest.event_loop import OrderRequest

SIDE_BID = 0
SIDE_ASK = 1


def build_quote_series(events: pd.DataFrame, every: int = 1) -> pd.DataFrame:
    """Replays events through the C++ book, recording best quotes.

    Returns columns: ts, bid_px, bid_qty, ask_px, ask_qty, mid (USD floats
    for the analysis layer; fixed-point stays inside the engine).
    """
    ts = np.ascontiguousarray(events["ts"].to_numpy(np.int64))
    order_id = np.ascontiguousarray(events["order_id"].to_numpy(np.uint64))
    etype = np.ascontiguousarray(events["type"].to_numpy(np.uint8))
    side = np.ascontiguousarray(events["side"].to_numpy(np.uint8))
    price = np.ascontiguousarray(events["price"].to_numpy(np.int64))
    qty = np.ascontiguousarray(events["qty"].to_numpy(np.int64))
    n = len(ts)

    # Replay in `every`-sized batches (one boundary crossing per batch) and
    # sample the quote after each - this is a replay-only path, so it takes
    # the cheap side of the boundary trade-off (see profiling/).
    book = cpp.OrderBook()
    rows = []
    for lo in range(0, n, every):
        hi = min(lo + every, n)
        book.apply_batch(ts[lo:hi], order_id[lo:hi], etype[lo:hi],
                         side[lo:hi], price[lo:hi], qty[lo:hi])
        q = book.best()
        if q.has_bid and q.has_ask:
            rows.append((int(ts[hi - 1]), q.bid_price, q.bid_qty,
                         q.ask_price, q.ask_qty))
    df = pd.DataFrame(rows, columns=["ts", "bid_px", "bid_qty", "ask_px", "ask_qty"])
    df["mid"] = (df["bid_px"] + df["ask_px"]) / 2.0 / cpp.PRICE_SCALE
    return df


def ofi_increments(quotes: pd.DataFrame) -> pd.Series:
    """Per-update OFI contribution e_n from consecutive best quotes."""
    bp = quotes["bid_px"].to_numpy(np.float64)
    bq = quotes["bid_qty"].to_numpy(np.float64)
    ap = quotes["ask_px"].to_numpy(np.float64)
    aq = quotes["ask_qty"].to_numpy(np.float64)
    e = np.zeros(len(quotes))
    e[1:] = (
        (bp[1:] >= bp[:-1]) * bq[1:] - (bp[1:] <= bp[:-1]) * bq[:-1]
        - (ap[1:] <= ap[:-1]) * aq[1:] + (ap[1:] >= ap[:-1]) * aq[:-1]
    )
    return pd.Series(e, index=quotes.index, name="ofi_e")


def rolling_ofi(quotes: pd.DataFrame, window_ns: int = 1_000_000_000) -> pd.Series:
    """OFI summed over a trailing time window, aligned to each quote update."""
    q = quotes.assign(ofi_e=ofi_increments(quotes))
    t = pd.to_datetime(q["ts"], unit="ns")
    return (
        q.set_index(t)["ofi_e"].rolling(pd.Timedelta(window_ns, "ns")).sum()
        .to_numpy()
    )


def signal_decay(quotes: pd.DataFrame, window_ns: int = 1_000_000_000,
                 horizons_ms=(100, 250, 500, 1000, 2000, 5000, 10000, 20000)) -> list:
    """Correlation of OFI with the future mid move at several horizons.

    The decay curve is the honest way to present a microstructure signal:
    it shows exactly how much predictive power survives a given latency.
    """
    ofi = rolling_ofi(quotes, window_ns)
    ts = quotes["ts"].to_numpy(np.int64)
    mid = quotes["mid"].to_numpy(np.float64)
    out = []
    for h in horizons_ms:
        h_ns = int(h * 1e6)
        idx = np.searchsorted(ts, ts + h_ns, side="left")
        valid = idx < len(ts)
        fwd = np.full(len(ts), np.nan)
        fwd[valid] = mid[idx[valid]] - mid[valid]
        mask = ~np.isnan(fwd) & ~np.isnan(ofi)
        corr = float(np.corrcoef(ofi[mask], fwd[mask])[0, 1]) if mask.sum() > 2 else np.nan
        out.append({"horizon_ms": h, "ic": corr})
    return out


def naive_backtest(
    quotes: pd.DataFrame,
    window_ns: int = 1_000_000_000,
    threshold: float = 2.0,
    hold_ns: int = 1_000_000_000,
    trade_qty: int = 100,
    sample_interval_ns: int = 100_000_000,
    sample_window: tuple = None,
) -> pd.DataFrame:
    """The lie: same signal, but filled at mid, instantly, with no queue.

    Samples the signal on the same 100 ms grid the realistic engine uses
    (and the same trading window, if one is set), holds for the same
    horizon, and 'fills' every trade at the current mid. Returns an equity
    DataFrame (ts, mid, position, cash, equity).
    """
    ts = quotes["ts"].to_numpy(np.int64)
    mid = quotes["mid"].to_numpy(np.float64)
    ofi = rolling_ofi(quotes, window_ns)
    # z-score against an expanding std so the threshold is scale-free.
    sd = pd.Series(ofi).expanding(min_periods=200).std().to_numpy()
    z = np.where(sd > 0, ofi / sd, 0.0)

    lo = ts[0] if sample_window is None else max(ts[0], int(sample_window[0]))
    hi = ts[-1] if sample_window is None else min(ts[-1], int(sample_window[1]))
    grid = np.arange(lo, hi, sample_interval_ns)
    at = np.searchsorted(ts, grid, side="right") - 1

    position = 0
    cash = 0.0
    entry_ts = 0
    rows = []
    for g, k in zip(grid, at):
        if k < 0:
            continue
        m = mid[k]
        if position == 0:
            if z[k] > threshold:
                position = trade_qty
                cash -= position * m
                entry_ts = g
            elif z[k] < -threshold:
                position = -trade_qty
                cash -= position * m
                entry_ts = g
        elif g - entry_ts >= hold_ns:
            cash += position * m
            position = 0
        rows.append((int(g), m, position, cash, cash + position * m))
    return pd.DataFrame(rows, columns=["ts", "mid", "position", "cash", "equity"])


@dataclass
class OFIStrategy:
    """The same OFI rule as `naive_backtest`, run inside the event loop.

    Entries join the touch passively (so they experience real queues);
    exits cross the spread aggressively after the hold period (so they pay
    real spread/depth). All state it sees is data-latency delayed.
    """
    window_ns: int = 1_000_000_000
    threshold: float = 2.0
    hold_ns: int = 1_000_000_000
    trade_qty: int = 100
    cooldown_ns: int = 500_000_000

    _events: deque = field(default_factory=deque)   # (ts, e_n)
    _prev_best: tuple = None
    _sum: float = 0.0
    _var_stats: list = field(default_factory=lambda: [0, 0.0, 0.0])  # n, mean, M2
    _entry_ts: int = None
    _last_action_ts: int = 0

    def __call__(self, ts, book, position, open_orders):
        best = book.best()
        if not (best.has_bid and best.has_ask):
            return []

        cur = (best.bid_price, best.bid_qty, best.ask_price, best.ask_qty)
        if self._prev_best is not None:
            bp0, bq0, ap0, aq0 = self._prev_best
            bp1, bq1, ap1, aq1 = cur
            e = ((bp1 >= bp0) * bq1 - (bp1 <= bp0) * bq0
                 - (ap1 <= ap0) * aq1 + (ap1 >= ap0) * aq0)
            self._events.append((ts, e))
            self._sum += e
        self._prev_best = cur
        while self._events and self._events[0][0] < ts - self.window_ns:
            self._sum -= self._events.popleft()[1]

        # Welford running std of the windowed OFI -> scale-free threshold.
        n, mean, m2 = self._var_stats
        n += 1
        d = self._sum - mean
        mean += d / n
        m2 += d * (self._sum - mean)
        self._var_stats = [n, mean, m2]
        sd = (m2 / n) ** 0.5 if n > 200 else 0.0
        z = self._sum / sd if sd > 0 else 0.0

        if ts - self._last_action_ts < self.cooldown_ns:
            return []

        # Position discipline. `open_orders` includes in-flight orders, not
        # just resting ones - an exit that has been submitted but hasn't
        # reached the exchange yet still counts as outstanding. Without
        # this, a fast market makes the strategy stack exits (each sized to
        # the full position), overshoot through zero, and escalate.
        has_open_exit = any(o.aggressive for o in open_orders)

        if position == 0 and not open_orders:
            if z > self.threshold:
                self._entry_ts = ts
                self._last_action_ts = ts
                return [OrderRequest(side=SIDE_BID, qty=self.trade_qty)]
            if z < -self.threshold:
                self._entry_ts = ts
                self._last_action_ts = ts
                return [OrderRequest(side=SIDE_ASK, qty=self.trade_qty)]
        elif position != 0 and self._entry_ts is not None and not has_open_exit:
            if ts - self._entry_ts >= self.hold_ns:
                self._last_action_ts = ts
                exit_side = SIDE_ASK if position > 0 else SIDE_BID
                # abs(position) is bounded by trade_qty now that entries
                # require a flat book of orders; the cap is structural.
                return [OrderRequest(side=exit_side, qty=abs(position), aggressive=True)]
        return []
