"""Phase 1: normalize raw MBO Parquet into the internal event schema.

Internal schema (the contract shared with the C++ book — see
cpp/include/itch_engine/types.hpp):

    ts       int64   nanoseconds since epoch, ascending
    order_id uint64
    type     uint8   0=Add 1=Cancel 2=Modify 3=Execute
    side     uint8   0=Bid 1=Ask
    price    int64   fixed-point, 1e-9 USD
    qty      int64

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
    df = raw.loc[mask, ["ts_event", "order_id", "action", "side", "price", "size"]].copy()

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
