"""Derived views over the backtester's output files.

Everything in here is read-only over `output/` - the viewer stays a static
file reader and never imports the engine or the event loop. All of it is
`st.cache_data`-backed and argument-free so Streamlit caches on the file
contents rather than re-hashing a 165k-row frame on every rerun.

The one piece of real analysis that lives here is the mark-out: for each
simulated fill, where the mid had moved N seconds later, signed so that a
negative number always means the fill was adversely selected. That is the
quantity the whole project argues about, and it is not in metrics.json -
it has to be reconstructed by joining fills against the reconstructed book.
"""

from __future__ import annotations

from pathlib import Path

import pandas as pd
import streamlit as st

REPO_ROOT = Path(__file__).resolve().parents[1]

# `output/` is gitignored - it is whatever the last local backtest wrote, and
# committing it would mean every run dirties the tree. A deployed copy of this
# app therefore has no data at all unless something is checked in, so `demo/`
# holds a committed snapshot of one synthetic run and is used as the fallback.
# Local runs always win; the demo set is only reached when output/ is absent.
LIVE_DIR = REPO_ROOT / "output"
DEMO_DIR = REPO_ROOT / "demo"


def _resolve_data_dir() -> Path:
    if (LIVE_DIR / "metrics.json").exists():
        return LIVE_DIR
    if (DEMO_DIR / "metrics.json").exists():
        return DEMO_DIR
    return LIVE_DIR  # nothing anywhere: name the real path in the empty state


OUTPUT_DIR = _resolve_data_dir()
IS_DEMO = OUTPUT_DIR == DEMO_DIR

PRICE_SCALE = 1_000_000_000
ET = "America/New_York"

# side encoding, per python/itch_engine/backtest/fill_model.py:49
SIDE_BUY, SIDE_SELL = 0, 1

# Regular trading hours, Eastern. Anything outside is pre/post-market.
RTH_OPEN, RTH_CLOSE = "09:30", "16:00"

# Mark-out horizons. 1s matches the strategy's own holding period; the
# longer ones show whether the adverse move keeps going or mean-reverts.
HORIZONS_S = (1, 5, 30, 60)


def to_et(ts_ns: pd.Series) -> pd.Series:
    """Epoch nanoseconds -> tz-aware Eastern.

    The charts previously rendered these as naive UTC, which put the 09:30
    open at '13:30' on the x-axis. Market data is read in exchange time.
    """
    return pd.to_datetime(ts_ns, unit="ns", utc=True).dt.tz_convert(ET)


@st.cache_data(show_spinner=False)
def load_metrics() -> dict:
    import json
    return json.loads((OUTPUT_DIR / "metrics.json").read_text())


@st.cache_data(show_spinner=False)
def load_trades() -> pd.DataFrame:
    return pd.read_csv(OUTPUT_DIR / "trades.csv")


@st.cache_data(show_spinner=False)
def load_snapshots() -> pd.DataFrame:
    return pd.read_parquet(OUTPUT_DIR / "snapshots.parquet")


@st.cache_data(show_spinner=False)
def best_quotes() -> pd.DataFrame:
    """Per-second best bid/ask/mid in dollars, indexed by raw ns timestamp."""
    s = load_snapshots()
    best = (
        s[s["rank"] == 0]
        .pivot_table(index="ts", columns="side", values="price", aggfunc="first")
        .sort_index()
    )
    out = pd.DataFrame(index=best.index)
    if {"bid", "ask"}.issubset(best.columns):
        out["bid"] = best["bid"] / PRICE_SCALE
        out["ask"] = best["ask"] / PRICE_SCALE
        out["mid"] = (out["bid"] + out["ask"]) / 2
        out["spread_bps"] = (out["ask"] - out["bid"]) / out["mid"] * 1e4
        # ~9% of sampled instants have only one side at rank 0, so their mid
        # is undefined. Dropping them here means an as-of join carries the
        # last *two-sided* quote forward instead of landing on a NaN and
        # silently reporting a missing mark-out.
        out = out.dropna(subset=["mid"])
    return out.reset_index()


def _session_label(et: pd.Series) -> pd.Series:
    t = et.dt.strftime("%H:%M")
    return pd.Series(
        pd.cut(
            t.map(lambda x: 0 if x < RTH_OPEN else (1 if x < RTH_CLOSE else 2)),
            bins=[-0.5, 0.5, 1.5, 2.5],
            labels=["pre-market", "regular hours", "after-hours"],
        ),
        index=et.index,
    ).astype(str)


@st.cache_data(show_spinner=False)
def enriched_trades() -> pd.DataFrame:
    """trades.csv with display columns and signed mark-outs attached."""
    t = load_trades().copy()
    if t.empty:
        return t

    t["et"] = to_et(t["ts"])
    t["session"] = _session_label(t["et"])
    t["side_label"] = t["side"].map({SIDE_BUY: "buy", SIDE_SELL: "sell"})
    t["liquidity"] = t["aggressive"].map({True: "taker", False: "maker"})
    t["price_usd"] = t["price"] / PRICE_SCALE
    t["notional"] = t["price_usd"] * t["qty"]

    # +1 for a buy, -1 for a sell, so a positive mark-out always means the
    # market moved in the fill's favour regardless of direction.
    direction = pd.Series(1, index=t.index).where(t["side"] == SIDE_BUY, -1)

    q = best_quotes()
    if q.empty or "mid" not in q:
        return t

    quotes = q[["ts", "mid"]].sort_values("ts")
    for h in HORIZONS_S:
        target = pd.DataFrame(
            {"ts": t["ts"] + h * 1_000_000_000, "_i": t.index}
        ).sort_values("ts")
        # Backward as-of: the last two-sided quote at or before t+h. For the
        # handful of fills within h seconds of the close there is no later
        # quote, so their mark-out is measured at the closing mid rather
        # than a full h seconds out.
        joined = pd.merge_asof(target, quotes, on="ts", direction="backward")
        mid_at_h = joined.set_index("_i")["mid"].reindex(t.index)
        # cents per share, signed: negative == adverse selection
        t[f"markout_{h}s"] = direction * (mid_at_h - t["price_usd"]) * 100
    return t


def filter_trades(
    t: pd.DataFrame,
    sessions: list[str],
    sides: list[str],
    liquidity: list[str],
    time_range: tuple | None = None,
) -> pd.DataFrame:
    """Applies the sidebar filters. Empty selection means 'no filter'."""
    if t.empty:
        return t
    out = t
    if sessions:
        out = out[out["session"].isin(sessions)]
    if sides:
        out = out[out["side_label"].isin(sides)]
    if liquidity:
        out = out[out["liquidity"].isin(liquidity)]
    if time_range and len(time_range) == 2:
        lo, hi = time_range
        clock = out["et"].dt.time
        out = out[(clock >= lo) & (clock <= hi)]
    return out


def markout_summary(t: pd.DataFrame) -> pd.DataFrame:
    """Mean mark-out per horizon, split by maker/taker. Cents per share."""
    rows = []
    for h in HORIZONS_S:
        col = f"markout_{h}s"
        if col not in t:
            continue
        for liq in ("maker", "taker"):
            sub = t[t["liquidity"] == liq][col].dropna()
            if len(sub):
                rows.append({"horizon_s": h, "liquidity": liq,
                             "markout_cents": sub.mean(), "fills": len(sub)})
    return pd.DataFrame(rows)


def queue_buckets(t: pd.DataFrame, n_buckets: int = 6) -> pd.DataFrame:
    """Fills and mean 1s mark-out bucketed by queue depth at join.

    Passive fills only - an aggressive order never joins a queue, so its
    `queue_ahead_at_join` of 0 is not the same fact as a passive order that
    happened to join an empty level.
    """
    p = t[t["liquidity"] == "maker"].copy()
    if p.empty or "markout_1s" not in p:
        return pd.DataFrame()
    edges = [-1, 0, 100, 300, 600, 1200, 2400, float("inf")][: n_buckets + 1]
    if edges[-1] != float("inf"):
        edges.append(float("inf"))
    labels = []
    for i in range(len(edges) - 1):
        lo, hi = edges[i], edges[i + 1]
        labels.append("0" if lo == -1 and hi == 0
                      else (f"{int(lo) + 1}+" if hi == float("inf")
                            else f"{int(lo) + 1}-{int(hi)}"))
    p["bucket"] = pd.cut(p["queue_ahead_at_join"], bins=edges, labels=labels)
    g = p.groupby("bucket", observed=True).agg(
        fills=("markout_1s", "size"),
        markout_cents=("markout_1s", "mean"),
        median_queue=("queue_ahead_at_join", "median"),
    ).reset_index()
    return g[g["fills"] > 0]
