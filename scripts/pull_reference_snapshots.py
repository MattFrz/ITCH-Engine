"""Pull exchange-published aggregated book snapshots (mbp-10) for validation.

Prints the cost and asks before spending anything. The raw DBN download is
cached, so a re-run never pays twice.

Usage:
    python scripts/pull_reference_snapshots.py --symbol AAPL --day 2026-07-30
    python scripts/pull_reference_snapshots.py --symbol AAPL --day 2026-07-30 --yes
    python scripts/pull_reference_snapshots.py --symbol AAPL --day 2026-07-30 --cost-only
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "python"))

from itch_engine.ingest.reference_snapshots import (  # noqa: E402
    DEFAULT_SCHEMA,
    estimate_cost,
    fetch_reference_snapshots,
    load_reference_snapshots,
)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--symbol", default="AAPL")
    ap.add_argument("--day", default="2026-07-30")
    ap.add_argument("--schema", default=DEFAULT_SCHEMA, help="mbp-10 (default) or mbp-1")
    ap.add_argument("--yes", action="store_true", help="skip the cost confirmation")
    ap.add_argument("--force", action="store_true", help="re-download even if cached")
    ap.add_argument("--cost-only", action="store_true", help="print the cost and stop")
    args = ap.parse_args()

    if args.cost_only:
        cost, records = estimate_cost(args.symbol, args.day, args.schema)
        print(f"{args.schema} {args.symbol} {args.day}: {records:,} records, ${cost:.4f}")
        return 0

    path = fetch_reference_snapshots(
        args.symbol, args.day, args.schema, force=args.force, assume_yes=args.yes
    )
    df = load_reference_snapshots(args.symbol, args.day, args.schema)
    print()
    print(f"rows        : {len(df):,}")
    print(f"ts range    : {df['ts_event'].min()} .. {df['ts_event'].max()}")
    print(f"columns     : {len(df.columns)}")
    print(f"path        : {path}")
    print()
    print("next:")
    print(
        f"  python validation/validate_against_exchange.py "
        f"--symbol {args.symbol} --day {args.day}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
