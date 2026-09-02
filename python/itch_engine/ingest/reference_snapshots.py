"""Pull exchange-published aggregated book snapshots for independent validation.

Why this exists
---------------
`validation/validate_book.py` proves the C++ book and an independently written
Python book agree exactly. That rules out essentially every indexing and
priority bug, but both were written from the same reading of the MBO
semantics, so a shared misinterpretation would agree perfectly and still be
wrong. It is agreement, not truth.

This module pulls the venue-derived answer: Databento's `mbp-10` schema, which
is the aggregated book *they* reconstruct from the same XNAS.ITCH feed. It
carries, for the top ten levels on each side:

    bid_px_NN / ask_px_NN    price
    bid_sz_NN / ask_sz_NN    aggregate size
    bid_ct_NN / ask_ct_NN    ORDER COUNT at that level

All three are things this engine's book tracks, so the comparison is a real
external check rather than another restatement of our own assumptions.

`mbp-10` is used rather than `mbp-1` because it subsumes it - level 00 is the
top of book - and validates depth and per-level order counts as well, for less
per gigabyte.

Cost
----
Real money. The raw DBN download is cached on disk and never re-fetched, and
the cost is printed and confirmed before anything is spent, exactly like
scripts/pull_day.py. AAPL 2026-07-30 was $0.5174 for 3,774,382 records.
"""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pandas as pd

REPO_ROOT = Path(__file__).resolve().parents[3]
REFERENCE_DIR = REPO_ROOT / "validation" / "reference_snapshots"

DATASET = "XNAS.ITCH"
DEFAULT_SCHEMA = "mbp-10"
LEVELS = 10


def dbn_path(symbol: str, day: str, schema: str = DEFAULT_SCHEMA) -> Path:
    return REFERENCE_DIR / f"{symbol}_{day}_{schema}.dbn.zst"


def snapshot_parquet_path(symbol: str, day: str, schema: str = DEFAULT_SCHEMA) -> Path:
    return REFERENCE_DIR / f"{symbol}_{day}_{schema}.parquet"


def _level_columns(levels: int = LEVELS) -> list[str]:
    cols: list[str] = []
    for i in range(levels):
        for side in ("bid", "ask"):
            for field in ("px", "sz", "ct"):
                cols.append(f"{side}_{field}_{i:02d}")
    return cols


def estimate_cost(symbol: str, day: str, schema: str = DEFAULT_SCHEMA) -> tuple[float, int]:
    """Returns (cost in USD, record count). Metadata queries are free."""
    import databento as db

    client = db.Historical(os.environ["DATABENTO_API_KEY"])
    kwargs = dict(
        dataset=DATASET,
        schema=schema,
        symbols=[symbol],
        start=day,
        end=day + "T23:59:59",
    )
    return (
        float(client.metadata.get_cost(**kwargs)),
        int(client.metadata.get_record_count(**kwargs)),
    )


def fetch_reference_snapshots(
    symbol: str,
    day: str,
    schema: str = DEFAULT_SCHEMA,
    force: bool = False,
    assume_yes: bool = False,
    force_download: bool = False,
) -> Path:
    """Downloads aggregated book snapshots to Parquet and returns the path.

    The raw DBN is written to disk first and kept, so a failure while
    converting - or a later change to which columns are kept - never costs a
    second pull. `force` rebuilds the Parquet from the cached DBN and spends
    nothing; only `force_download` goes back to the API.
    """
    parquet = snapshot_parquet_path(symbol, day, schema)
    if parquet.exists() and not force:
        return parquet
    REFERENCE_DIR.mkdir(parents=True, exist_ok=True)

    api_key = os.environ.get("DATABENTO_API_KEY")
    if not api_key:
        raise SystemExit(
            "DATABENTO_API_KEY is not set. Exchange-published snapshots cannot be "
            "synthesized: generating them from our own reconstruction would be "
            "circular and would validate nothing. This check needs real data."
        )

    import databento as db

    raw = dbn_path(symbol, day, schema)
    if not raw.exists() or force_download:
        client = db.Historical(api_key)
        cost, records = estimate_cost(symbol, day, schema)
        print(
            f"{DATASET} {schema} {symbol} {day}: {records:,} records, "
            f"estimated cost ${cost:.4f}"
        )
        if not assume_yes:
            reply = input("proceed with this pull? [y/N] ").strip().lower()
            if reply not in ("y", "yes"):
                raise SystemExit("aborted - nothing spent")
        print(f"downloading to {raw} ...")
        client.timeseries.get_range(
            dataset=DATASET,
            schema=schema,
            symbols=[symbol],
            start=day,
            end=day + "T23:59:59",
            path=str(raw),
        )
        print(f"downloaded {raw.stat().st_size / 1e6:.1f} MB")
    else:
        print(f"using cached {raw} (no credits spent)")

    print("converting to parquet ...")
    store = db.DBNStore.from_file(raw)
    # price_type="fixed" keeps prices as int64 1e-9 USD (our internal
    # convention); pretty_ts=False keeps timestamps as int64 ns.
    df = store.to_df(price_type="fixed", pretty_ts=False).reset_index()

    # `action` matters: an mbp-10 row labelled 'T' is a trade report and shows
    # the book BEFORE that sequence's change, while a row labelled with a
    # book-changing action shows it AFTER. Without it the two cannot be told
    # apart and the comparison is off by one message. See
    # validation/validate_against_exchange.py.
    keep = ["ts_event", "sequence", "action"] + _level_columns()
    missing = [c for c in keep if c not in df.columns]
    if missing:
        raise SystemExit(f"{schema} response is missing expected columns: {missing}")
    df = df[keep].copy()

    if np.issubdtype(df["ts_event"].dtype, np.datetime64):
        df["ts_event"] = df["ts_event"].astype("int64")
    df["ts_event"] = df["ts_event"].astype("int64")
    df["sequence"] = df["sequence"].astype("uint32")
    df["action"] = df["action"].astype(str)
    for col in _level_columns():
        # Prices stay 64-bit fixed point; sizes and counts fit in 32 bits and
        # halving them matters at 3.8M rows.
        df[col] = df[col].astype("int64" if "_px_" in col else "uint32")

    df.to_parquet(parquet, index=False)
    print(f"wrote {parquet} ({parquet.stat().st_size / 1e6:.1f} MB, {len(df):,} rows)")
    return parquet


def load_reference_snapshots(
    symbol: str, day: str, schema: str = DEFAULT_SCHEMA
) -> pd.DataFrame:
    path = snapshot_parquet_path(symbol, day, schema)
    if not path.exists():
        raise FileNotFoundError(
            f"{path} missing - run scripts/pull_reference_snapshots.py first"
        )
    return pd.read_parquet(path)
