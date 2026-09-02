"""Phase 1: normalize raw MBO Parquet into the internal event schema.

Internal schema (the contract shared with the C++ book - see
cpp/include/itch_engine/types.hpp):

    ts       int64   nanoseconds since epoch, ascending
    sequence uint32  the feed's own message counter (see below)
    order_id uint64
    type     uint8   0=Add 1=Cancel 2=Modify 3=Execute
    side     uint8   0=Bid 1=Ask
    price    int64   fixed-point, 1e-9 USD
    qty      int64

`sequence` is carried through untouched. The book never reads it, but it is
the only exact join key against the venue's other schemas: `ts_event` is NOT
safe for that, because an aggregated-book row can carry a different timestamp
from the MBO record that produced it. See
validation/validate_against_exchange.py.

Output is partitioned by symbol/date under data/processed/ (production
layout, even at one-day scale).
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pandas as pd

from itch_engine import (
    EVENT_ADD,
    EVENT_CANCEL,
    EVENT_CLEAR,
    EVENT_EXECUTE,
    EVENT_MODIFY,
)

REPO_ROOT = Path(__file__).resolve().parents[3]
PROCESSED_DIR = REPO_ROOT / "data" / "processed"

# Databento MBO action codes -> internal event types.
#
# 'R' (Clear) DOES mutate book state: the venue wipes resting orders at
# session start and when a halt resumes. Dropping it leaves every pre-halt
# order resting forever in the reconstruction, so it is mapped, not ignored.
# 'T' (trade printed with no book impact, e.g. against hidden liquidity) and
# 'N' (none) genuinely do not change per-order state.
ACTION_MAP = {
    "A": EVENT_ADD,
    "C": EVENT_CANCEL,
    "M": EVENT_MODIFY,
    "F": EVENT_EXECUTE,
    "R": EVENT_CLEAR,
}
SIDE_MAP = {"B": 0, "A": 1}


def processed_parquet_path(symbol: str, day: str) -> Path:
    return PROCESSED_DIR / f"symbol={symbol}" / f"date={day}" / "events.parquet"


def drop_execution_paired_cancels(df: pd.DataFrame) -> tuple[pd.DataFrame, int]:
    """Removes the 'C' record that Databento pairs with every 'F'.

    Databento's MBO reports a partial execution as TWO records at the same
    timestamp for the same order reference: an `F` (fill) and a `C` (cancel),
    carrying the SAME size. They describe the same shares - the fill is the
    trade report, the cancel is the book mutation - so applying both removes
    the quantity twice and shrinks the book.

    This was not caught by comparing the C++ book against the Python reference
    book, because both applied both records and therefore agreed with each
    other exactly. It was caught by comparing against the venue's own
    aggregated book (validation/validate_against_exchange.py), which is the
    entire reason that check exists.

    On AAPL 2026-07-30: 199,781 F records, 100% of them paired with a C at the
    same (ts_event, order_id), and 100% of those pairs identical in size.

    The `F` is kept rather than the `C` because the two are NOT
    interchangeable downstream even though their book effect is identical: the
    backtester's fill model advances queue position differently for a feed
    execution than for a feed cancel, so collapsing executions into cancels
    would silently change the fill simulation.

    Returns the filtered frame and the number of rows dropped.
    """
    is_f = df["action"].eq("F")
    is_c = df["action"].eq("C")
    if not is_f.any() or not is_c.any():
        return df, 0

    # Mark every (ts_event, order_id) that carries a fill, then drop the cancel
    # rows sharing that key. Done as a merge rather than a Python set so it
    # stays vectorized at 14M+ rows.
    keys = df.loc[is_f, ["ts_event", "order_id"]].drop_duplicates()
    keys["_has_fill"] = True
    marked = df.merge(keys, on=["ts_event", "order_id"], how="left", sort=False)
    marked["_has_fill"] = marked["_has_fill"].fillna(False).to_numpy(dtype=bool)

    drop = marked["_has_fill"].to_numpy() & is_c.to_numpy()
    dropped = int(drop.sum())
    kept = marked.loc[~drop].drop(columns="_has_fill")
    return kept, dropped


def normalize_day(raw_path: Path, symbol: str, day: str, force: bool = False) -> Path:
    out = processed_parquet_path(symbol, day)
    if out.exists() and not force:
        return out
    out.parent.mkdir(parents=True, exist_ok=True)

    raw = pd.read_parquet(raw_path)
    # Clear carries no meaningful side (venues emit 'N'), so it must not be
    # filtered out by the side check the per-order actions need.
    is_clear = raw["action"].eq("R")
    mask = raw["action"].isin(ACTION_MAP) & (raw["side"].isin(SIDE_MAP) | is_clear)
    cols = ["ts_event", "order_id", "action", "side", "price", "size"]
    has_sequence = "sequence" in raw.columns
    if has_sequence:
        cols.insert(1, "sequence")
    df = raw.loc[mask, cols].copy()

    # An execution arrives as a fill AND a cancel for the same shares; applying
    # both double-counts it. See drop_execution_paired_cancels.
    df, dropped = drop_execution_paired_cancels(df)
    if dropped:
        print(
            f"normalize: dropped {dropped:,} cancel records paired with a fill "
            "(same shares reported twice by the feed)"
        )

    events = pd.DataFrame(
        {
            "ts": df["ts_event"].astype("int64").to_numpy(),
            "order_id": df["order_id"].astype("uint64").to_numpy(),
            "type": df["action"].map(ACTION_MAP).astype("uint8").to_numpy(),
            # Clear rows have no side; 0 is a placeholder the book ignores.
            "side": df["side"].map(SIDE_MAP).fillna(0).astype("uint8").to_numpy(),
            "price": df["price"].astype("int64").to_numpy(),
            "qty": df["size"].astype("int64").to_numpy(),
        }
    )
    if has_sequence:
        # Carried for cross-schema validation only; nothing in the book or the
        # backtester reads it. Older cached pulls predate the column, so its
        # absence is tolerated rather than fatal.
        events["sequence"] = df["sequence"].astype("uint32").to_numpy()
    # The book replay assumes time-ordered events; enforce it once here.
    events = events.sort_values("ts", kind="stable").reset_index(drop=True)
    events.to_parquet(out, index=False)
    return out


def load_events(symbol: str, day: str) -> pd.DataFrame:
    """Loads the normalized event stream for one symbol/day."""
    path = processed_parquet_path(symbol, day)
    if not path.exists():
        raise FileNotFoundError(
            f"{path} missing - run ingest first (see scripts in README)"
        )
    return pd.read_parquet(path)


def event_arrays(events: pd.DataFrame) -> tuple:
    """Contiguous numpy views in the order apply_batch expects."""
    return (
        np.ascontiguousarray(events["ts"].to_numpy(np.int64)),
        np.ascontiguousarray(events["order_id"].to_numpy(np.uint64)),
        np.ascontiguousarray(events["type"].to_numpy(np.uint8)),
        np.ascontiguousarray(events["side"].to_numpy(np.uint8)),
        np.ascontiguousarray(events["price"].to_numpy(np.int64)),
        np.ascontiguousarray(events["qty"].to_numpy(np.int64)),
    )
