"""Differential validation: research book vs low-latency book, real data.

The research book is this repository's trusted reference. `validate_book.py`
proves it agrees to zero drift with an independently written pure-Python book
across a full session; this script extends that trust to the low-latency book
by replaying the same normalized day through both C++ implementations and
comparing everything observable at random checkpoints:

    events.parquet
        |
        +--> itch_engine_cpp.OrderBook             (research, the reference)
        |
        +--> itch_engine_cpp.LowLatencyOrderBook   (pools, tick grid, open
                                                    addressing)

At every checkpoint:

  * top-N price levels per side: price, aggregate quantity, order count
  * best bid/ask and the mid
  * open order count and live level count per side
  * the exact FIFO order of every id resting at every top level
  * `queue_ahead` for a sample of live orders
  * the anomaly counters (unknown references, clears applied)

Any single mismatch fails the run. This is the Python-side half of the
differential testing; the C++ half (cpp/tests/test_differential.cpp) runs
generated streams that inject edge cases real data does not always contain,
and cpp/apps/itch_replay.cpp --verify checks the packet pipeline end to end.

Usage:
    python validation/validate_low_latency.py [--symbol AAPL] [--day 2026-07-01]
        [--checkpoints 1000] [--seed 7] [--levels 10]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "python"))

import itch_engine_cpp as cpp  # noqa: E402

from itch_engine.ingest.databento_client import fetch_day  # noqa: E402
from itch_engine.ingest.normalize import load_events, normalize_day  # noqa: E402

RESULTS_DIR = REPO_ROOT / "validation" / "results"


def next_pow2(n: int) -> int:
    p = 1
    while p < n:
        p <<= 1
    return p


def sized_config(arrays, prices: np.ndarray) -> "cpp.BookConfig":
    """Capacity sized from a measurement pass over this exact day.

    Capacity is not only a safety limit here, it is a latency parameter: the
    book preallocates everything and never grows, so an oversized configuration
    spreads the working set over memory that is never touched and pays TLB and
    cache misses on every event. On the reference session that cost 60% in the
    mean and nearly 4x at p99 (see docs/low_latency_architecture.md).

    So rather than guess, this runs one cheap batch pass through a deliberately
    oversized book, reads the peaks it actually reached, and sizes the real one
    from those with 2x headroom. The pass is not timed and does not affect the
    comparison; it costs a couple of seconds on a full session.
    """
    # Tick grid: size it for where the prices actually are, not for the most
    # extreme one. The reference session has adds at $199,999.00; a grid that
    # reached them would be 16 MiB per side to address a handful of levels. The
    # off-grid overflow exists precisely for that tail, so the grid covers the
    # 99.9th percentile and the rest takes the slower path.
    finite = prices[(prices > 0) & (prices < np.iinfo(np.int64).max)]
    dense_top = int(np.percentile(finite, 99.9)) if finite.size else 0
    tick_count = min(max(next_pow2(dense_top // cpp.BookConfig().tick_size + 2), 1 << 12), 1 << 20)

    probe_cfg = cpp.BookConfig()
    probe_cfg.max_orders = 1 << 22
    probe_cfg.index_capacity = 1 << 23
    probe_cfg.max_levels = 1 << 18
    probe_cfg.tick_count = tick_count
    probe_cfg.max_offgrid_levels = 1 << 16
    probe = cpp.LowLatencyOrderBook(probe_cfg)
    probe.apply_batch(*arrays)
    if probe.degraded:
        raise SystemExit(
            f"sizing pass exhausted even the probe capacity "
            f"({probe.capacity_rejections} rejections) - this day is larger than "
            "this script expects"
        )

    # The probe ran with the final tick grid, so its off-grid peak is the real
    # one and the overflow can be sized from it.
    cfg = cpp.BookConfig()
    cfg.max_orders = max(next_pow2(probe.peak_orders * 2), 1 << 14)
    cfg.index_capacity = cfg.max_orders * 2
    cfg.max_levels = max(next_pow2(probe.peak_levels * 2), 1 << 12)
    cfg.max_offgrid_levels = max(next_pow2(probe.peak_offgrid_levels * 4), 1024)
    cfg.tick_count = tick_count
    return cfg, probe.peak_orders, probe.peak_levels


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbol", default="AAPL")
    ap.add_argument("--day", default="2026-07-01")
    ap.add_argument("--checkpoints", type=int, default=1000)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--levels", type=int, default=10, help="levels per side to compare")
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

    arrays = (ts, order_id, etype, side, price, qty)
    cfg, probe_peak_orders, probe_peak_levels = sized_config(arrays, price)
    print(
        f"sizing pass: peak {probe_peak_orders:,} resting orders, "
        f"{probe_peak_levels:,} live levels -> max_orders={cfg.max_orders:,} "
        f"index_capacity={cfg.index_capacity:,} max_levels={cfg.max_levels:,} "
        f"tick_count={cfg.tick_count:,}"
    )

    research = cpp.OrderBook()
    fast = cpp.LowLatencyOrderBook(cfg)

    rng = np.random.default_rng(args.seed)
    checkpoints = np.sort(
        rng.choice(
            np.arange(n // 100, n),
            size=min(args.checkpoints, n - n // 100),
            replace=False,
        )
    )
    cp_iter = iter(checkpoints.tolist())
    next_cp = next(cp_iter, None)

    checks = 0
    orders_compared = 0
    mismatches = []

    def compare(i):
        nonlocal checks, orders_compared
        for s in (0, 1):
            rd = [(lv.price, lv.qty, lv.order_count) for lv in research.depth(s, args.levels)]
            fd = [(lv.price, lv.qty, lv.order_count) for lv in fast.depth(s, args.levels)]
            checks += 1
            if rd != fd:
                mismatches.append((i, f"side {s} depth", rd, fd))
                continue
            # Exact FIFO order at every compared level.
            for lv_price, _, _ in rd:
                checks += 1
                rq = research.queue_at(s, lv_price)
                fq = fast.queue_at(s, lv_price)
                orders_compared += len(rq)
                if rq != fq:
                    mismatches.append((i, f"side {s} FIFO @ {lv_price}", rq[:8], fq[:8]))

        rb = research.best()
        fb = fast.best()
        checks += 1
        if (rb.has_bid, rb.bid_price, rb.bid_qty, rb.has_ask, rb.ask_price, rb.ask_qty) != (
            fb.has_bid,
            fb.bid_price,
            fb.bid_qty,
            fb.has_ask,
            fb.ask_price,
            fb.ask_qty,
        ):
            mismatches.append(
                (
                    i,
                    "best quote",
                    (rb.bid_price, rb.bid_qty, rb.ask_price, rb.ask_qty),
                    (fb.bid_price, fb.bid_qty, fb.ask_price, fb.ask_qty),
                )
            )

        checks += 1
        if research.mid_price() != fast.mid_price():
            mismatches.append((i, "mid", research.mid_price(), fast.mid_price()))

        for label, a, b in (
            ("open orders", research.open_order_count, fast.open_order_count),
            ("bid levels", research.bid_level_count, fast.bid_level_count),
            ("ask levels", research.ask_level_count, fast.ask_level_count),
            ("unknown refs", research.unknown_order_events, fast.unknown_order_events),
            ("clears", research.clears_applied, fast.clears_applied),
        ):
            checks += 1
            if a != b:
                mismatches.append((i, label, a, b))

        # queue_ahead for a sample of live orders. It is O(position), so a
        # sample rather than every order; the FIFO id comparison above already
        # pins the ordering exactly.
        top = research.depth(0, 1)
        if top:
            ids = research.queue_at(0, top[0].price)[:5]
            for oid in ids:
                checks += 1
                if research.queue_ahead(oid) != fast.queue_ahead(oid):
                    mismatches.append(
                        (i, f"queue_ahead({oid})", research.queue_ahead(oid), fast.queue_ahead(oid))
                    )

    for i in range(n):
        research.apply_event(
            int(ts[i]), int(order_id[i]), int(etype[i]), int(side[i]), int(price[i]), int(qty[i])
        )
        fast.apply_event(
            int(ts[i]), int(order_id[i]), int(etype[i]), int(side[i]), int(price[i]), int(qty[i])
        )
        if next_cp is not None and i == next_cp:
            compare(i)
            next_cp = next(cp_iter, None)

    # A degraded book has silently dropped events; comparing against it would
    # be comparing against something incomplete.
    if fast.degraded:
        mismatches.append((n, "capacity", 0, fast.capacity_rejections))

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    lines = [
        f"symbol={args.symbol} day={args.day}",
        f"events replayed: {n:,}",
        f"checkpoints: {len(checkpoints):,} random timestamps",
        f"assertions: {checks:,}",
        f"resting orders compared in exact FIFO order: {orders_compared:,}",
        f"low-latency book: peak orders {fast.peak_orders:,} (of {cfg.max_orders:,}), "
        f"peak levels {fast.peak_levels:,} (of {cfg.max_levels:,}), "
        f"off-grid levels {fast.peak_offgrid_levels:,}",
        f"order index: avg probes {fast.index_average_probes:.3f}, "
        f"max probe {fast.index_max_probe}",
        f"book memory: {fast.memory_bytes / (1024 * 1024):.1f} MiB",
        f"mismatches: {len(mismatches)}",
    ]
    if mismatches:
        lines.append("RESULT: FAIL")
        for m in mismatches[:20]:
            lines.append(f"  event {m[0]}: {m[1]}: research={m[2]} low_latency={m[3]}")
    else:
        lines.append(
            "RESULT: PASS - low-latency book matches the research book exactly "
            "at every sampled timestamp"
        )

    report = "\n".join(lines)
    (RESULTS_DIR / "low_latency_validation.txt").write_text(report + "\n")
    print(report)
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
