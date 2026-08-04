"""Inspect and reclaim disk space used by cached data and old results.

Nothing in this pipeline auto-deletes anything - every day you pull is
cached forever under data/raw/symbol=X/date=Y/ and data/processed/symbol=X/
date=Y/, and output/ (the backtest results) plus profiling/results/ and
validation/results/ are simply overwritten each run (previous run's numbers
are gone unless archived - see scripts/run_backtest.py, which now archives
the previous output/ into output_archive/ before overwriting it).

This script never deletes anything by default - it only lists what's on
disk and how big it is. Pass --delete flags to actually remove something.

Usage:
    python scripts/cleanup_data.py                        # just list, no changes
    python scripts/cleanup_data.py --symbol AAPL --day 2026-07-01 --delete
    python scripts/cleanup_data.py --symbol AAPL --keep-latest 1 --delete
    python scripts/cleanup_data.py --archives --keep-latest 3 --delete
    python scripts/cleanup_data.py --all --delete --yes    # wipe every cached day + archive
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
RAW_DIR = REPO_ROOT / "data" / "raw"
PROCESSED_DIR = REPO_ROOT / "data" / "processed"
ARCHIVE_DIR = REPO_ROOT / "output_archive"


def dir_size(path: Path) -> int:
    return sum(f.stat().st_size for f in path.rglob("*") if f.is_file())


def human(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:.0f}{unit}" if unit == "B" else f"{n:.1f}{unit}"
        n /= 1024
    return f"{n:.1f}TB"


def find_days(symbol_filter: str | None) -> list[tuple[str, str, Path, Path]]:
    """Returns (symbol, day, raw_dir, processed_dir) for every cached day."""
    out = []
    if not RAW_DIR.exists():
        return out
    for symbol_dir in sorted(RAW_DIR.glob("symbol=*")):
        symbol = symbol_dir.name.removeprefix("symbol=")
        if symbol_filter and symbol != symbol_filter:
            continue
        for day_dir in sorted(symbol_dir.glob("date=*")):
            day = day_dir.name.removeprefix("date=")
            processed = PROCESSED_DIR / symbol_dir.name / day_dir.name
            out.append((symbol, day, day_dir, processed))
    return out


def list_cached(symbol_filter: str | None) -> None:
    days = find_days(symbol_filter)
    if not days:
        print("no cached data/raw days found")
    else:
        print(f"{'symbol':<8} {'day':<12} {'source':<10} {'raw':>10} {'processed':>10}")
        total = 0
        for symbol, day, raw_dir, proc_dir in days:
            raw_sz = dir_size(raw_dir) if raw_dir.exists() else 0
            proc_sz = dir_size(proc_dir) if proc_dir.exists() else 0
            total += raw_sz + proc_sz
            marker = raw_dir / "source.txt"
            source = marker.read_text().strip() if marker.exists() else "unknown"
            print(f"{symbol:<8} {day:<12} {source:<10} "
                  f"{human(raw_sz):>10} {human(proc_sz):>10}")
        print(f"\ntotal cached data: {human(total)}")

    if ARCHIVE_DIR.exists():
        archives = sorted(ARCHIVE_DIR.iterdir())
        if archives:
            print(f"\n{len(archives)} archived output snapshot(s) in output_archive/:")
            for a in archives:
                print(f"  {a.name}  ({human(dir_size(a))})")


def delete_days(days: list[tuple[str, str, Path, Path]]) -> None:
    for symbol, day, raw_dir, proc_dir in days:
        if raw_dir.exists():
            shutil.rmtree(raw_dir)
            print(f"deleted {raw_dir}")
        if proc_dir.exists():
            shutil.rmtree(proc_dir)
            print(f"deleted {proc_dir}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbol", help="limit to this symbol")
    ap.add_argument("--day", help="delete exactly this symbol/day (requires --symbol)")
    ap.add_argument("--keep-latest", type=int,
                    help="delete all but the N most recent days per symbol")
    ap.add_argument("--archives", action="store_true",
                    help="operate on output_archive/ snapshots instead of data/")
    ap.add_argument("--all", action="store_true",
                    help="delete everything cached (all symbols, all days)")
    ap.add_argument("--delete", action="store_true",
                    help="actually delete; without this, only lists")
    ap.add_argument("--yes", action="store_true", help="skip confirmation prompt")
    args = ap.parse_args()

    if args.archives:
        if not ARCHIVE_DIR.exists():
            print("no output_archive/ directory")
            return 0
        archives = sorted(ARCHIVE_DIR.iterdir(), key=lambda p: p.stat().st_mtime)
        if not args.delete:
            list_cached(args.symbol)
            return 0
        keep = args.keep_latest if args.keep_latest is not None else 0
        to_delete = archives[:-keep] if keep else archives
        if not to_delete:
            print("nothing to delete")
            return 0
        print(f"will delete {len(to_delete)} archive snapshot(s):")
        for a in to_delete:
            print(f"  {a}")
        if not args.yes and input("proceed? [y/N] ").strip().lower() != "y":
            print("aborted")
            return 1
        for a in to_delete:
            shutil.rmtree(a)
        print("done")
        return 0

    if not args.delete:
        list_cached(args.symbol)
        return 0

    if args.day and not args.symbol:
        print("--day requires --symbol")
        return 1

    days = find_days(args.symbol)
    if args.day:
        days = [d for d in days if d[1] == args.day]
    elif args.keep_latest is not None:
        by_symbol: dict[str, list] = {}
        for d in days:
            by_symbol.setdefault(d[0], []).append(d)
        days = []
        for sym, group in by_symbol.items():
            group.sort(key=lambda d: d[1])  # ISO dates sort chronologically
            days.extend(group[:-args.keep_latest] if args.keep_latest else group)
    elif not args.all:
        print("specify --day, --keep-latest, or --all to choose what to delete")
        return 1

    if not days:
        print("nothing matches - nothing to delete")
        return 0

    print(f"will delete {len(days)} cached day(s):")
    for symbol, day, *_ in days:
        print(f"  {symbol} {day}")
    if not args.yes and input("proceed? [y/N] ").strip().lower() != "y":
        print("aborted")
        return 1
    delete_days(days)
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
