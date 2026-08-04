"""Phase 5: post-trade viewer - a static-file reader, nothing more.

Reads exactly the three files the backtester wrote to output/ (trades.csv,
metrics.json, snapshots.parquet), plus the validation report if present.
It never imports the engine, never touches the event loop, and holds no
state. Run with:

    streamlit run viewer/app.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pandas as pd
import streamlit as st

REPO_ROOT = Path(__file__).resolve().parents[1]
OUTPUT_DIR = REPO_ROOT / "output"
VALIDATION_TXT = REPO_ROOT / "validation" / "results" / "validation_run.txt"
sys.path.insert(0, str(REPO_ROOT / "viewer"))

from charts import (  # noqa: E402
    equity_curve,
    fill_rate_by_queue,
    latency_histogram,
    signal_decay,
    top_of_book,
)

st.set_page_config(
    page_title="ITCH-Engine | post-trade",
    page_icon=":chart_with_downwards_trend:",
    layout="wide",
)

st.markdown(
    """
    <style>
      .block-container { padding-top: 2.2rem; max-width: 1400px; }
      [data-testid="stMetric"] {
          background: #111827; border: 1px solid #1f2937;
          border-radius: 10px; padding: 14px 18px;
      }
      [data-testid="stMetricLabel"] { color: #6b7280; }
      .ie-badge {
          display: inline-block; padding: 3px 10px; margin-right: 6px;
          border-radius: 999px; font-size: 12px; font-weight: 600;
          letter-spacing: .02em; border: 1px solid #1f2937; color: #9ca3af;
      }
      .ie-badge.pass  { color: #34d399; border-color: #14532d; }
      .ie-badge.warn  { color: #fbbf24; border-color: #713f12; }
      .ie-badge.accent{ color: #22d3ee; border-color: #164e63; }
      h1 { letter-spacing: -.02em; }
    </style>
    """,
    unsafe_allow_html=True,
)

# ---------------------------------------------------------------- load files
metrics_path = OUTPUT_DIR / "metrics.json"
if not metrics_path.exists():
    st.error(f"`{metrics_path}` not found - run `python scripts/run_backtest.py` first.")
    st.stop()

metrics = json.loads(metrics_path.read_text())
summary = metrics["summary"]
params = metrics.get("params", {})


@st.cache_data
def load_trades() -> pd.DataFrame:
    return pd.read_csv(OUTPUT_DIR / "trades.csv")


@st.cache_data
def load_snapshots() -> pd.DataFrame:
    return pd.read_parquet(OUTPUT_DIR / "snapshots.parquet")


validation_pass = VALIDATION_TXT.exists() and "PASS" in VALIDATION_TXT.read_text()

# ------------------------------------------------------------------- header
st.title("ITCH-Engine")
badges = [
    f"<span class='ie-badge accent'>{params.get('symbol', '?')} &middot; "
    f"{params.get('day', '?')}</span>",
    "<span class='ie-badge pass'>BOOK VALIDATED &middot; 0 DRIFT</span>"
    if validation_pass else
    "<span class='ie-badge warn'>VALIDATION NOT RUN</span>",
    "<span class='ie-badge warn'>SYNTHETIC DATA</span>"
    if params.get("synthetic_data") else
    "<span class='ie-badge accent'>XNAS.ITCH MBO</span>",
    f"<span class='ie-badge'>latency {params.get('data_latency_ms', '?')}ms data / "
    f"{params.get('order_latency_ms', '?')}ms order</span>",
    f"<span class='ie-badge'>{summary['events_processed']:,} events</span>",
]
st.markdown(" ".join(badges), unsafe_allow_html=True)
st.caption(
    "Post-trade analysis - decoupled static-file reader over the backtester's "
    "outputs. One signal, two fill assumptions; the gap is the point."
)

# ---------------------------------------------------------------- KPI strip
naive = summary["naive_pnl_usd"]
real = summary["realistic_pnl_usd"]
k1, k2, k3, k4, k5 = st.columns(5)
k1.metric("Naive PnL", f"${naive:+,.0f}",
          help="Vectorized backtest: fills at mid, zero latency, no queues")
k2.metric("Realistic PnL", f"${real:+,.0f}", delta=f"{real - naive:+,.0f} vs naive",
          delta_color="normal",
          help="Same decisions through queue-position fills and the latency model")
fr = summary.get("passive_fill_rate")
k3.metric("Passive fill rate", f"{fr:.0%}" if fr is not None else "n/a",
          delta="naive assumes 100%", delta_color="off")
k4.metric("Boundary p50",
          f"{metrics['latency_histogram'].get('p50_us', float('nan')):.1f} us",
          delta="target < 20 us", delta_color="off")
k5.metric("Fills / orders", f"{summary['fills']:,} / {summary['orders_submitted']:,}")

st.divider()

# --------------------------------------------------------------------- tabs
tab_overview, tab_exec, tab_engine, tab_data = st.tabs(
    ["Overview", "Execution quality", "Engine performance", "Raw outputs"]
)

with tab_overview:
    st.plotly_chart(equity_curve.figure(metrics), use_container_width=True)
    st.plotly_chart(top_of_book.figure(load_snapshots()), use_container_width=True)

with tab_exec:
    c1, c2 = st.columns(2)
    with c1:
        st.plotly_chart(fill_rate_by_queue.figure(metrics), use_container_width=True)
    with c2:
        st.plotly_chart(signal_decay.figure(metrics), use_container_width=True)
    st.caption(
        "Left: joining deeper queues fills less - and what does fill is "
        "adversely selected. Right: the signal's edge by horizon; every "
        "millisecond of latency shifts you right along this curve."
    )

with tab_engine:
    st.plotly_chart(latency_histogram.figure(metrics), use_container_width=True)
    e1, e2, e3 = st.columns(3)
    e1.metric("Events processed", f"{summary['events_processed']:,}")
    eps = summary.get("events_per_second")
    e2.metric("Event-loop throughput", f"{eps:,.0f} ev/s" if eps else "n/a",
              help="Full backtest loop incl. Python-side bookkeeping; the "
                   "C++ book alone replays at >1M events/s (see profiling/)")
    e3.metric("Unknown-order events", f"{summary['unknown_order_events']:,}",
              help="Feed messages referencing orders resting from before the "
                   "session window; counted and skipped, never guessed at")
    if validation_pass:
        st.code(VALIDATION_TXT.read_text().strip(), language=None)

with tab_data:
    st.markdown("**trades.csv** - one row per simulated fill")
    st.dataframe(load_trades(), use_container_width=True, height=300)
    st.markdown("**snapshots.parquet** - per-second top-5 book state (first 500 rows)")
    st.dataframe(load_snapshots().head(500), use_container_width=True, height=300)
    st.markdown("**metrics.json** - run summary")
    st.json({"summary": summary, "params": params})
