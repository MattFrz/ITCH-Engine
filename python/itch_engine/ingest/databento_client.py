"""Phase 1: pull one symbol / one day of XNAS.ITCH MBO data.

Two paths:

* **Real data** (preferred): requires the `databento` package and a
  `DATABENTO_API_KEY` environment variable. The raw pull is saved to Parquet
  immediately so free-tier credits are only ever spent once.
* **Synthetic data**: a seeded generator producing a statistically plausible
  MBO day (random-walk mid, Poisson add/cancel flow concentrated near the
  touch, executions against the front of the queue). It exists so every
  downstream phase - validation, backtesting, profiling, the viewer - runs
  end-to-end without credentials. The schema is identical to the real path.

Raw schema (Databento MBO convention):
    ts_event (int ns), order_id (uint64), action (A/C/M/F), side (B/A),
    price (int64, 1e-9 USD), size (int64)
"""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pandas as pd

REPO_ROOT = Path(__file__).resolve().parents[3]
RAW_DIR = REPO_ROOT / "data" / "raw"

DATASET = "XNAS.ITCH"
SCHEMA = "mbo"
PRICE_SCALE = 1_000_000_000


def raw_parquet_path(symbol: str, day: str) -> Path:
    # Partitioned by symbol/date even at 1-day scale (production layout).
    return RAW_DIR / f"symbol={symbol}" / f"date={day}" / "mbo.parquet"


def source_marker_path(symbol: str, day: str) -> Path:
    return raw_parquet_path(symbol, day).parent / "source.txt"


def data_source(symbol: str, day: str) -> str:
    """How the cached day was produced: 'databento', 'synthetic', or 'unknown'.

    Read from a marker written at fetch time, so the label reflects the data
    actually on disk - not whatever happens to be in the environment when a
    later analysis runs.
    """
    marker = source_marker_path(symbol, day)
    if marker.exists():
        return marker.read_text().strip()
    return "unknown"


def fetch_day(symbol: str, day: str, force: bool = False) -> Path:
    """Fetches one day of MBO data to raw Parquet and returns the path.

    Uses the cached file if present (credits are never spent twice). Falls
    back to the synthetic generator when no API key is configured. A
    source.txt marker records which path produced the file.
    """
    out = raw_parquet_path(symbol, day)
    if out.exists() and not force:
        return out
    out.parent.mkdir(parents=True, exist_ok=True)

    api_key = os.environ.get("DATABENTO_API_KEY")
    if api_key:
        df = _fetch_databento(symbol, day, api_key)
        source = "databento"
    else:
        print(
            "DATABENTO_API_KEY not set - generating a synthetic MBO day "
            "(schema-identical to the real feed)."
        )
        df = generate_synthetic_day(symbol, day)
        source = "synthetic"

    df.to_parquet(out, index=False)
    source_marker_path(symbol, day).write_text(source + "\n")
    return out


def _fetch_databento(symbol: str, day: str, api_key: str) -> pd.DataFrame:
    import databento as db  # optional dependency; see requirements.txt

    client = db.Historical(api_key)
    store = client.timeseries.get_range(
        dataset=DATASET,
        schema=SCHEMA,
        symbols=[symbol],
        start=day,
        end=day + "T23:59:59",
    )
    # price_type="fixed" keeps prices as int64 1e-9 USD (our internal
    # convention); pretty_ts=False keeps timestamps as int64 ns.
    df = store.to_df(price_type="fixed", pretty_ts=False).reset_index()
    keep = ["ts_event", "order_id", "action", "side", "price", "size"]
    df = df[keep]
    if np.issubdtype(df["ts_event"].dtype, np.datetime64):
        df["ts_event"] = df["ts_event"].astype("int64")
    return df


def generate_synthetic_day(
    symbol: str,
    day: str,
    n_events: int = 400_000,
    seed: int = 7,
    open_price: float = 190.0,
) -> pd.DataFrame:
    """Generates a plausible one-day MBO stream (regular session, 6.5h).

    Not a market simulator - just enough microstructure realism for the
    engine to chew on: cancels dominate adds, executions hit the front of
    FIFO queues at the touch, and the mid random-walks with tick-size 0.01.
    """
    from collections import deque

    rng = np.random.default_rng(seed)
    session_open = pd.Timestamp(f"{day} 09:30:00", tz="US/Eastern")
    open_ns = session_open.tz_convert("UTC").value
    session_ns = int(6.5 * 3600 * 1e9)

    tick = int(0.01 * PRICE_SCALE)
    mid_ticks = int(round(open_price * PRICE_SCALE / tick))

    # Event timestamps: U-shaped intraday intensity (open/close clustering).
    u = rng.beta(0.8, 0.8, n_events)
    ts = np.sort(open_ns + (u * session_ns).astype(np.int64))

    live: dict = {}                 # oid -> (side, price_ticks, qty)
    order_pool: list = []           # oids for O(1) random cancel (lazy-deleted)
    queues: dict = {}               # (side, price_ticks) -> deque of oids (FIFO)
    next_id = 1

    rows = np.empty(n_events, dtype=[
        ("ts_event", "i8"), ("order_id", "u8"), ("action", "U1"),
        ("side", "U1"), ("price", "i8"), ("size", "i8"),
    ])
    n = 0

    def pop_front(side: str):
        """Oldest live order at the most aggressive price on `side`.

        Scans outward from the mid; queues are lazily cleaned, so stale ids
        (cancelled/repriced) are discarded as they surface.
        """
        step = -1 if side == "B" else 1
        start = mid_ticks + step
        for k in range(30):
            q = queues.get((side, start + step * k))
            while q:
                oid = q[0]
                entry = live.get(oid)
                if entry is None or entry[1] != start + step * k or entry[0] != side:
                    q.popleft()  # stale
                    continue
                return oid
        return None

    def pick_random_live(max_tries: int = 8):
        while order_pool and max_tries:
            i = int(rng.integers(0, len(order_pool)))
            oid = order_pool[i]
            if oid in live:
                return oid
            # Lazy delete: swap-remove the stale slot.
            order_pool[i] = order_pool[-1]
            order_pool.pop()
            max_tries -= 1
        return None

    # Aggressor flow is persistent (buy/sell pressure comes in runs) - the
    # empirical property that makes OFI predictive at short horizons.
    aggressor = "A"  # "A" = resting asks being lifted (buy pressure)

    for i in range(n_events):
        r = rng.random()
        oid = None
        if r >= 0.44:
            oid = pick_random_live()
        if oid is None or r < 0.44:
            # Add: passive order 0-4 ticks from the touch. Adds are the
            # baseline; cancels below dominate the message mix, as on a
            # real ITCH day.
            side = "B" if rng.random() < 0.5 else "A"
            offset = int(rng.integers(0, 5))
            price_ticks = mid_ticks - 1 - offset if side == "B" else mid_ticks + 1 + offset
            qty = int(rng.choice([100, 100, 100, 200, 300, 500]))
            oid = next_id
            next_id += 1
            live[oid] = (side, price_ticks, qty)
            order_pool.append(oid)
            queues.setdefault((side, price_ticks), deque()).append(oid)
            rows[n] = (ts[i], oid, "A", side, price_ticks * tick, qty)
            n += 1
        elif r < 0.86:
            # Full cancel of a random resting order.
            side, price_ticks, qty = live.pop(oid)
            rows[n] = (ts[i], oid, "C", side, price_ticks * tick, qty)
            n += 1
        elif r < 0.94:
            # Execute against the front of the queue at the touch. The
            # aggressor side persists across trades, flipping rarely.
            if rng.random() < 0.10:
                aggressor = "B" if aggressor == "A" else "A"
            hit_side = aggressor
            oid = pop_front(hit_side)
            if oid is None:
                continue
            side, price_ticks, qty = live[oid]
            fill = min(qty, int(rng.choice([100, 100, 200])))
            rows[n] = (ts[i], oid, "F", side, price_ticks * tick, fill)
            n += 1
            if fill == qty:
                del live[oid]
            else:
                live[oid] = (side, price_ticks, qty - fill)
            # Trades push the mid in the aggressor's direction; persistent
            # flow therefore produces short-horizon momentum.
            if rng.random() < 0.55:
                mid_ticks += 1 if side == "A" else -1
        else:
            # Modify: size-down half the time (keeps queue priority),
            # otherwise reprice (goes to the back of the new queue).
            side, price_ticks, qty = live[oid]
            if rng.random() < 0.5 and qty > 100:
                qty -= 100
            else:
                price_ticks += int(rng.integers(-2, 3))
                queues.setdefault((side, price_ticks), deque()).append(oid)
            live[oid] = (side, price_ticks, qty)
            rows[n] = (ts[i], oid, "M", side, price_ticks * tick, qty)
            n += 1

    df = pd.DataFrame(rows[:n])
    df["symbol"] = symbol
    return df
