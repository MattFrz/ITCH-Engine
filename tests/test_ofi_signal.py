"""OFI signal: sign conventions and the naive/live alignment.

The alignment test is the important one. The naive and realistic backtests
only isolate fill realism if they are driven by the same signal on the same
clock; if they drift apart again, the headline comparison stops meaning
what the README says it means.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from itch_engine.signals.ofi_signal import (
    _ofi_on_grid,
    _running_std,
    ofi_increments,
)


def quotes_from(bid_px, bid_qty, ask_px, ask_qty, step_ns=100_000_000):
    n = len(bid_px)
    return pd.DataFrame({
        "ts": np.arange(n, dtype=np.int64) * step_ns,
        "bid_px": np.asarray(bid_px, dtype=np.int64),
        "bid_qty": np.asarray(bid_qty, dtype=np.int64),
        "ask_px": np.asarray(ask_px, dtype=np.int64),
        "ask_qty": np.asarray(ask_qty, dtype=np.int64),
    })


def test_growing_bid_is_positive_pressure():
    q = quotes_from([100, 100], [10, 50], [101, 101], [10, 10])
    assert ofi_increments(q).iloc[1] > 0


def test_growing_ask_is_negative_pressure():
    q = quotes_from([100, 100], [10, 10], [101, 101], [10, 50])
    assert ofi_increments(q).iloc[1] < 0


def test_a_static_book_generates_no_signal():
    q = quotes_from([100] * 5, [10] * 5, [101] * 5, [10] * 5)
    assert ofi_increments(q).iloc[1:].abs().sum() == 0


def test_first_increment_is_zero():
    q = quotes_from([100, 100], [10, 99], [101, 101], [10, 10])
    assert ofi_increments(q).iloc[0] == 0


def test_grid_ofi_uses_grid_neighbours_not_quote_neighbours():
    # Bid size ramps every quote. Sampling every other quote must produce
    # increments measured between the sampled points, so the windowed sum
    # over the whole series still reflects the total build-up.
    q = quotes_from([100] * 6, [10, 20, 30, 40, 50, 60], [101] * 6, [10] * 6)
    at = np.array([0, 2, 4], dtype=np.int64)
    got = _ofi_on_grid(q, at, window_ns=10 ** 12, sample_interval_ns=1)
    # Trailing window covers everything, so the last value is the full sum
    # of grid-to-grid increments: (30-10) + (50-30) = 40.
    assert got[-1] == 40


def test_grid_points_before_the_first_quote_contribute_nothing():
    q = quotes_from([100, 100], [10, 20], [101, 101], [10, 10])
    at = np.array([-1, 0, 1], dtype=np.int64)
    got = _ofi_on_grid(q, at, window_ns=10 ** 12, sample_interval_ns=1)
    assert np.isfinite(got).all()
    assert got[0] == 0


def test_window_only_sums_the_trailing_span():
    q = quotes_from([100] * 5, [10, 20, 30, 40, 50], [101] * 5, [10] * 5)
    at = np.arange(5, dtype=np.int64)
    # A one-sample window keeps only the most recent increment (10 each).
    got = _ofi_on_grid(q, at, window_ns=1, sample_interval_ns=1)
    assert got[-1] == 10


def test_running_std_is_causal_and_warms_up():
    x = np.random.default_rng(0).normal(size=1000)
    sd = _running_std(x, min_periods=200)
    assert (sd[:200] == 0).all()          # suppressed during warm-up
    assert (sd[200:] > 0).all()
    # Causal: truncating the tail cannot change earlier values.
    assert np.allclose(sd[:500], _running_std(x[:500], min_periods=200))


def test_running_std_matches_welford_population_form():
    # The live strategy uses a Welford population variance (m2/n). The
    # vectorised version must agree, or the two paths threshold differently.
    x = np.random.default_rng(1).normal(size=400)
    sd = _running_std(x, min_periods=0)
    expected = np.array([np.sqrt(np.var(x[: i + 1])) for i in range(len(x))])
    assert np.allclose(sd, expected)
