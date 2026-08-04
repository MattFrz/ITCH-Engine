"""Latency model for the event-driven backtester.

Two one-way delays, both in nanoseconds:

* `data_latency_ns` - how stale the strategy's view of the market is. The
  event loop maintains a second order book that only receives events once
  they are at least this old, and the strategy is only ever shown that book.
* `order_latency_ns` - how long a strategy order (or cancel) takes to reach
  the exchange. Fills are evaluated against the *exchange* book state at
  arrival time, not at decision time.

This is deliberately a constant-delay model: the point of the project is to
show that *any* nonzero latency plus queue position destroys naive-backtest
alpha, not to model the network. A jitter term is still included because a
constant delay is unrealistically kind to latency-sensitive signals.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass
class LatencyModel:
    data_latency_ns: int = 2_000_000     # 2 ms market-data path
    order_latency_ns: int = 3_000_000    # 3 ms order-entry path
    jitter_ns: int = 500_000             # +/- uniform jitter on each order
    seed: int = 11

    def __post_init__(self) -> None:
        self._rng = np.random.default_rng(self.seed)

    def order_arrival(self, decision_ts: int) -> int:
        """Exchange-side arrival time for an order decided at decision_ts."""
        jitter = int(self._rng.integers(-self.jitter_ns, self.jitter_ns + 1))
        return decision_ts + self.order_latency_ns + max(jitter, -self.order_latency_ns + 1)

    def visible_before(self, exchange_ts: int) -> int:
        """Latest event timestamp the strategy may see at exchange_ts."""
        return exchange_ts - self.data_latency_ns
