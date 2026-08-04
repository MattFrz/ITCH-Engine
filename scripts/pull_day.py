"""Pull and normalize a single day, on its own, with a cost check first.

This is the standalone version of the ingest step embedded in
run_backtest.py / validate_book.py - useful when you just want to add a new
day to the local cache (data/raw/symbol=X/date=Y/) without running the
whole pipeline yet.

Each day is cached under its own symbol/date folder and never overwrites or
deletes a different day - see scripts/cleanup_data.py to reclaim disk space
once you've pulled more days than you need.

Usage:
    python scripts/pull_day.py --symbol AAPL --day 2026-08-01
    python scripts/pull_day.py --symbol AAPL --day 2026-08-01 --yes   # skip prompt
    python scripts/pull_day.py --symbol AAPL --day 2026-08-01 --force # re-pull even if cached
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "python"))

from itch_engine.ingest.databento_client import (  # noqa: E402
    DATASET,
    SCHEMA,
    data_source,
    fetch_day,
    raw_parquet_path,
)
from itch_engine.ingest.normalize import normalize_day, processed_parquet_path  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbol", required=True)
    ap.add_argument("--day", required=True, help="YYYY-MM-DD")
    ap.add_argument("--force", action="store_true",
                    help="re-pull even if this symbol/day is already cached")
    ap.add_argument("--yes", action="store_true",
                    help="skip the cost confirmation prompt (for scripting)")
    args = ap.parse_args()

    cached = raw_parquet_path(args.symbol, args.day)
    if cached.exists() and not args.force:
        source = data_source(args.symbol, args.day)
        print(f"already cached: {cached} (source: {source})")
        if source != "databento" and os.environ.get("DATABENTO_API_KEY"):
            print("NOTE: the cached day is SYNTHETIC but your API key is set.")
            print("      Re-run with --force to replace it with real data:")
            print(f"      py -3.9 scripts/pull_day.py --symbol {args.symbol} "
                  f"--day {args.day} --force")
            return 1
    else:
        api_key = os.environ.get("DATABENTO_API_KEY")
        if api_key:
            import databento as db
            client = db.Historical(api_key)
            end = args.day + "T23:59:59"
            cost = client.metadata.get_cost(
                dataset=DATASET, schema=SCHEMA, symbols=[args.symbol],
                start=args.day, end=end,
            )
            records = client.metadata.get_record_count(
                dataset=DATASET, schema=SCHEMA, symbols=[args.symbol],
                start=args.day, end=end,
            )
            print(f"{args.symbol} {args.day}: {records:,} records, "
                  f"estimated cost ${cost:.4f}")
            if not args.yes:
                reply = input("proceed with this pull? [y/N] ").strip().lower()
                if reply != "y":
                    print("aborted")
                    return 1
        else:
            print("DATABENTO_API_KEY not set - generating synthetic data instead "
                  "(no cost, no confirmation needed)")

        fetch_day(args.symbol, args.day, force=args.force)
        print(f"raw saved: {raw_parquet_path(args.symbol, args.day)}")

    out = normalize_day(raw_parquet_path(args.symbol, args.day), args.symbol,
                        args.day, force=args.force)
    print(f"normalized: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
