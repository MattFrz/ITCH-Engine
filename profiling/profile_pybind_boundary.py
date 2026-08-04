"""Profile the Python <-> C++ boundary - the bottleneck nobody checks.

Measures, on the real normalized day:

1. `apply_event` (scalar): one pybind11 crossing per event. This is what the
   event-driven backtester pays, and the number the <20us/event target is
   about. Timed in blocks to separate the crossing cost from timer noise,
   plus a per-call sampled distribution for percentiles.
2. `apply_batch` (bulk): one crossing for the whole array. The delta vs. the
   scalar path is pure boundary overhead (argument conversion, GIL, call
   dispatch) - the C++ work is identical.
3. Hot query calls (`best`, `mid_price`, `bid_qty_at`) - the strategy-side
   crossings.

Writes profiling/results/latency_percentiles.json.

Usage:
    python profiling/profile_pybind_boundary.py [--symbol AAPL] [--day 2026-07-01]
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "python"))

import itch_engine_cpp as cpp  # noqa: E402

from itch_engine.ingest.databento_client import fetch_day  # noqa: E402
from itch_engine.ingest.normalize import (  # noqa: E402
    event_arrays,
    load_events,
    normalize_day,
)

RESULTS = REPO_ROOT / "profiling" / "results" / "latency_percentiles.json"


def pct(arr, p):
    return float(np.percentile(arr, p))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbol", default="AAPL")
    ap.add_argument("--day", default="2026-07-01")
    args = ap.parse_args()

    raw = fetch_day(args.symbol, args.day)
    normalize_day(raw, args.symbol, args.day)
    events = load_events(args.symbol, args.day)
    ts, oid, etype, side, price, qty = event_arrays(events)
    n = len(ts)
    print(f"{n:,} events")

    # --- 1a. scalar path, per-call sampled distribution -------------------
    book = cpp.OrderBook()
    samples = np.empty(n // 16 + 1, dtype=np.int64)
    k = 0
    t0 = time.perf_counter_ns()
    for i in range(n):
        if i % 16 == 0:
            c0 = time.perf_counter_ns()
            book.apply_event(ts[i], oid[i], etype[i], side[i], price[i], qty[i])
            samples[k] = time.perf_counter_ns() - c0
            k += 1
        else:
            book.apply_event(ts[i], oid[i], etype[i], side[i], price[i], qty[i])
    scalar_total_ns = time.perf_counter_ns() - t0
    samples = samples[:k]
    # NOTE: the loop above passes numpy scalars; the backtester passes ints.
    # Both are one pybind11 conversion per argument - same boundary.

    # --- 1b. scalar path, block-timed mean (no timer in the loop) ---------
    book2 = cpp.OrderBook()
    t0 = time.perf_counter_ns()
    for i in range(n):
        book2.apply_event(ts[i], oid[i], etype[i], side[i], price[i], qty[i])
    scalar_clean_ns = time.perf_counter_ns() - t0

    # --- 2. batch path ----------------------------------------------------
    book3 = cpp.OrderBook()
    t0 = time.perf_counter_ns()
    book3.apply_batch(ts, oid, etype, side, price, qty)
    batch_ns = time.perf_counter_ns() - t0

    # --- 3. query calls ---------------------------------------------------
    q_iters = 100_000
    t0 = time.perf_counter_ns()
    for _ in range(q_iters):
        book3.best()
    best_ns = (time.perf_counter_ns() - t0) / q_iters
    t0 = time.perf_counter_ns()
    for _ in range(q_iters):
        book3.mid_price()
    mid_ns = (time.perf_counter_ns() - t0) / q_iters

    scalar_mean_us = scalar_clean_ns / n / 1000
    batch_mean_us = batch_ns / n / 1000
    report = {
        "symbol": args.symbol,
        "day": args.day,
        "events": int(n),
        "scalar_apply_event": {
            "mean_us_per_event": scalar_mean_us,
            "p50_us": pct(samples, 50) / 1000,
            "p95_us": pct(samples, 95) / 1000,
            "p99_us": pct(samples, 99) / 1000,
            "sampled_calls": int(k),
            "total_wall_s": scalar_total_ns / 1e9,
            "target_us": 20.0,
            "meets_target": bool(scalar_mean_us < 20.0),
        },
        "batch_apply": {
            "mean_us_per_event": batch_mean_us,
            "total_wall_s": batch_ns / 1e9,
            "events_per_second": n / (batch_ns / 1e9),
        },
        "boundary_overhead_us_per_event": scalar_mean_us - batch_mean_us,
        "query_calls": {
            "best_us": best_ns / 1000,
            "mid_price_us": mid_ns / 1000,
        },
    }

    RESULTS.parent.mkdir(parents=True, exist_ok=True)
    RESULTS.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    print(f"\nwritten to {RESULTS}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
