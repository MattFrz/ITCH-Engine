"""Latency model: delays must always be forward in time."""

from __future__ import annotations

from itch_engine.backtest.latency_model import LatencyModel


def test_orders_never_arrive_before_they_were_decided():
    lat = LatencyModel()
    for ts in range(0, 10_000_000_000, 250_000_000):
        assert lat.order_arrival(ts) > ts


def test_arrival_sits_inside_the_jitter_band():
    lat = LatencyModel(order_latency_ns=3_000_000, jitter_ns=500_000)
    lo = 3_000_000 - 500_000
    hi = 3_000_000 + 500_000
    for ts in range(0, 100_000_000, 1_000_000):
        delay = lat.order_arrival(ts) - ts
        assert lo <= delay <= hi


def test_visible_horizon_lags_by_the_data_latency():
    lat = LatencyModel(data_latency_ns=2_000_000)
    assert lat.visible_before(1_000_000_000) == 998_000_000


def test_zero_latency_still_moves_strictly_forward():
    # A degenerate config must not let an order arrive at its decision time,
    # which would silently hand the strategy a free look at the future.
    lat = LatencyModel(order_latency_ns=0, jitter_ns=0)
    assert lat.order_arrival(500) >= 500


def test_seed_makes_jitter_reproducible():
    a = LatencyModel(seed=42)
    b = LatencyModel(seed=42)
    assert [a.order_arrival(i) for i in range(50)] == \
           [b.order_arrival(i) for i in range(50)]
