"""End-to-end run: ingest -> replay -> naive + realistic backtest -> outputs.

Produces the three viewer inputs in output/ (trades.csv, metrics.json,
snapshots.parquet). The headline number it prints is how much of the naive
backtest's PnL evaporates under real fills and latency.

Usage:
    python scripts/run_backtest.py [--symbol AAPL] [--day 2026-07-01]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "python"))

from itch_engine.backtest.event_loop import run_backtest  # noqa: E402
from itch_engine.backtest.latency_model import LatencyModel  # noqa: E402
from itch_engine.ingest.databento_client import data_source, fetch_day  # noqa: E402
from itch_engine.ingest.normalize import load_events, normalize_day  # noqa: E402
from itch_engine.reporting.write_outputs import write_outputs  # noqa: E402
from itch_engine.signals.ofi_signal import (  # noqa: E402
    OFIStrategy,
    build_quote_series,
    naive_backtest,
    signal_decay,
)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbol", default="AAPL")
    ap.add_argument("--day", default="2026-07-01")
    ap.add_argument("--threshold", type=float, default=2.0)
    ap.add_argument("--data-latency-ms", type=float, default=2.0)
    ap.add_argument("--order-latency-ms", type=float, default=3.0)
    ap.add_argument("--rth-only", action="store_true",
                    help="only trade during regular hours (09:30-16:00 ET); "
                         "pre/post-market books are thin and dominate PnL "
                         "with fast-market episodes")
    args = ap.parse_args()

    print(f"[1/5] ingest {args.symbol} {args.day}")
    raw = fetch_day(args.symbol, args.day)
    normalize_day(raw, args.symbol, args.day)
    events = load_events(args.symbol, args.day)
    print(f"      {len(events):,} normalized events")

    print("[2/5] quote series (C++ replay)")
    # Cap the quote series near ~1M samples; a real ITCH day has 10M+ events.
    every = max(1, len(events) // 1_000_000)
    quotes = build_quote_series(events, every=every)

    window = None
    if args.rth_only:
        import pandas as pd
        tz = "US/Eastern"
        window = (
            pd.Timestamp(f"{args.day} 09:30", tz=tz).tz_convert("UTC").value,
            pd.Timestamp(f"{args.day} 16:00", tz=tz).tz_convert("UTC").value,
        )

    print("[3/5] naive vectorized backtest (fills at mid, zero latency)")
    naive_eq = naive_backtest(quotes, threshold=args.threshold,
                              sample_window=window)

    print("[4/5] realistic event-driven backtest (queues + latency)")
    latency = LatencyModel(
        data_latency_ns=int(args.data_latency_ms * 1e6),
        order_latency_ns=int(args.order_latency_ms * 1e6),
    )
    strategy = OFIStrategy(threshold=args.threshold)
    result = run_backtest(events, strategy, latency, sample_window=window)
    print(f"      {result.events_processed:,} events in {result.elapsed_s:.1f}s "
          f"({result.events_processed / result.elapsed_s:,.0f} events/s), "
          f"{len(result.fills)} fills, {len(result.orders)} orders")

    print("[5/5] signal decay + outputs")
    decay = signal_decay(quotes)
    params = {
        "symbol": args.symbol,
        "day": args.day,
        "threshold": args.threshold,
        "data_latency_ms": args.data_latency_ms,
        "order_latency_ms": args.order_latency_ms,
        # Provenance of the data on disk, not the current environment.
        "synthetic_data": data_source(args.symbol, args.day) != "databento",
        "rth_only": bool(args.rth_only),
    }
    out_dir = write_outputs(result, naive_eq, decay, params)

    naive_pnl = float(naive_eq["equity"].iloc[-1]) if len(naive_eq) else 0.0
    real_pnl = float(result.equity["equity"].iloc[-1]) if len(result.equity) else 0.0
    print(f"\nnaive PnL:     ${naive_pnl:+,.2f}")
    print(f"realistic PnL: ${real_pnl:+,.2f}")
    if naive_pnl:
        print(f"naive backtest overstated PnL by "
              f"{(naive_pnl - real_pnl) / abs(naive_pnl) * 100:.0f}%")
    print(f"\noutputs written to {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
