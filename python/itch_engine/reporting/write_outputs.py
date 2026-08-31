"""Phase 3/4 output writer - the single handoff point to the viewer.

The backtester writes exactly three files into `output/`:

* `trades.csv`       - one row per simulated fill
* `snapshots.parquet`- per-second top-5 book snapshots
* `metrics.json`     - everything the viewer charts that isn't raw fills or
                       snapshots: summary numbers, naive-vs-realistic equity
                       curves (downsampled), the latency histogram, fill-rate
                       by queue-position buckets, and the signal decay curve

No other component reads or writes these files, and the viewer reads nothing
else. Keeping the analysis pre-computed here is what lets the viewer stay a
dumb static-file reader.

Because output/ is a single, unpartitioned location (by design - the viewer
always reads "the current results", not a specific day), each run
overwrites whatever was there before. To avoid silently losing a previous
day's results, write_outputs() archives the existing output/ into
output_archive/<symbol>_<day>_<UTC-timestamp>/ before overwriting it -
see scripts/cleanup_data.py to inspect or prune those archives.
"""

from __future__ import annotations

import json
import shutil
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import pandas as pd

REPO_ROOT = Path(__file__).resolve().parents[3]
OUTPUT_DIR = REPO_ROOT / "output"
ARCHIVE_DIR = REPO_ROOT / "output_archive"


def _archive_existing(out_dir: Path) -> Path | None:
    """Moves a pre-existing output/ into output_archive/ before it's overwritten."""
    metrics_path = out_dir / "metrics.json"
    if not metrics_path.exists():
        return None
    try:
        old_params = json.loads(metrics_path.read_text()).get("params", {})
    except (json.JSONDecodeError, OSError):
        old_params = {}
    tag = f"{old_params.get('symbol', 'unknown')}_{old_params.get('day', 'unknown')}"
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    dest = ARCHIVE_DIR / f"{tag}_{stamp}"
    ARCHIVE_DIR.mkdir(parents=True, exist_ok=True)
    shutil.copytree(out_dir, dest)
    return dest


def _downsample(df: pd.DataFrame, max_rows: int = 5000) -> pd.DataFrame:
    if len(df) <= max_rows:
        return df
    step = int(np.ceil(len(df) / max_rows))
    return df.iloc[::step]


def _equity_records(equity: pd.DataFrame) -> list:
    d = _downsample(equity)
    return [
        {"ts": int(r.ts), "equity": float(r.equity), "position": int(r.position)}
        for r in d.itertuples()
    ]


def fill_rate_by_queue_bucket(orders: pd.DataFrame) -> list:
    """Fill rate of passive orders bucketed by queue depth at join."""
    passive = orders[~orders["aggressive"]]
    if passive.empty:
        return []
    edges = [0, 100, 250, 500, 1000, 2500, 5000, np.inf]
    labels = ["0-100", "100-250", "250-500", "500-1k", "1k-2.5k", "2.5k-5k", ">5k"]
    buckets = pd.cut(passive["queue_ahead_at_join"], bins=edges,
                     labels=labels, include_lowest=True, right=False)
    out = []
    for label, grp in passive.groupby(buckets, observed=False):
        if len(grp) == 0:
            continue
        out.append({
            "bucket": str(label),
            "orders": int(len(grp)),
            "fill_rate": float((grp["filled_qty"] > 0).mean()),
            "full_fill_rate": float((grp["filled_qty"] == grp["qty"]).mean()),
        })
    return out


def latency_histogram(boundary_ns: np.ndarray, n_bins: int = 40) -> dict:
    if len(boundary_ns) == 0:
        return {"bin_edges_us": [], "counts": [], "p50_us": None,
                "p95_us": None, "p99_us": None}
    us = boundary_ns / 1000.0
    # Clip the far tail so the histogram stays readable; percentiles don't.
    clipped = np.clip(us, 0, np.percentile(us, 99.5))
    counts, edges = np.histogram(clipped, bins=n_bins)
    return {
        "bin_edges_us": [float(e) for e in edges],
        "counts": [int(c) for c in counts],
        "p50_us": float(np.percentile(us, 50)),
        "p95_us": float(np.percentile(us, 95)),
        "p99_us": float(np.percentile(us, 99)),
        "samples": int(len(us)),
    }


def write_outputs(
    result,                       # BacktestResult
    naive_equity: pd.DataFrame,
    decay: list,
    params: dict,
    out_dir: Path = OUTPUT_DIR,
) -> Path:
    archived = _archive_existing(out_dir)
    if archived:
        print(f"archived previous output/ -> {archived}")
    out_dir.mkdir(parents=True, exist_ok=True)

    result.fills.to_csv(out_dir / "trades.csv", index=False)
    result.snapshots.to_parquet(out_dir / "snapshots.parquet", index=False)

    realistic_pnl = float(result.equity["equity"].iloc[-1]) if len(result.equity) else 0.0
    naive_pnl = float(naive_equity["equity"].iloc[-1]) if len(naive_equity) else 0.0
    overstatement = (
        (naive_pnl - realistic_pnl) / abs(naive_pnl) * 100.0 if naive_pnl else None
    )

    orders = result.orders
    passive = orders[~orders["aggressive"]] if len(orders) else orders
    events_per_s = (
        result.events_processed / result.elapsed_s if result.elapsed_s else None
    )

    metrics = {
        "params": params,
        "summary": {
            "naive_pnl_usd": naive_pnl,
            "realistic_pnl_usd": realistic_pnl,
            "naive_overstatement_pct": overstatement,
            "orders_submitted": int(len(orders)),
            "passive_orders": int(len(passive)),
            "passive_fill_rate": (
                float((passive["filled_qty"] > 0).mean()) if len(passive) else None
            ),
            "mean_queue_ahead_at_join": (
                float(passive["queue_ahead_at_join"].mean()) if len(passive) else None
            ),
            "fills": int(len(result.fills)),
            # Reported separately so the naive-vs-realistic gap can be
            # decomposed rather than hand-waved: fees are a known, constant
            # cost, and whatever is left over is queue position and latency.
            "fees_paid_usd": float(result.fees_paid),
            "realistic_pnl_before_fees_usd": realistic_pnl + float(result.fees_paid),
            "events_processed": int(result.events_processed),
            "unknown_order_events": int(result.unknown_order_events),
            "backtest_wall_time_s": float(result.elapsed_s),
            "events_per_second": events_per_s,
        },
        "fee_schedule": result.fee_schedule,
        "equity_realistic": _equity_records(result.equity),
        "equity_naive": _equity_records(naive_equity),
        "latency_histogram": latency_histogram(result.boundary_ns),
        "fill_rate_by_queue": fill_rate_by_queue_bucket(orders) if len(orders) else [],
        "signal_decay": decay,
    }
    with open(out_dir / "metrics.json", "w") as f:
        json.dump(metrics, f, indent=2)
    return out_dir
