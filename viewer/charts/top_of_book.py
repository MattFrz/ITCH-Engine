"""Top-of-book context: mid price and spread through the day.

Built from snapshots.parquet (per-second top-5 book state) - gives the
execution charts their market context without touching the engine.
"""

from __future__ import annotations

import pandas as pd
import plotly.graph_objects as go
from plotly.subplots import make_subplots

from charts import _style

PRICE_SCALE = 1_000_000_000


def figure(snapshots: pd.DataFrame) -> go.Figure:
    best = snapshots[snapshots["rank"] == 0].pivot_table(
        index="ts", columns="side", values="price", aggfunc="first"
    )
    fig = make_subplots(
        rows=2, cols=1, shared_xaxes=True, row_heights=[0.72, 0.28],
        vertical_spacing=0.06,
    )
    if {"bid", "ask"}.issubset(best.columns):
        t = _style.to_chart_time(best.index)
        mid = (best["bid"] + best["ask"]) / 2 / PRICE_SCALE
        spread_bps = ((best["ask"] - best["bid"]) / PRICE_SCALE) / mid * 1e4
        fig.add_trace(go.Scatter(
            x=t, y=mid, name="mid", mode="lines",
            line=dict(color=_style.ACCENT, width=1.3),
            hovertemplate="%{x|%H:%M:%S}<br>$%{y:,.2f}<extra>mid</extra>",
        ), row=1, col=1)
        fig.add_trace(go.Scatter(
            x=t, y=spread_bps.clip(upper=spread_bps.quantile(0.995)),
            name="spread", mode="lines",
            line=dict(color=_style.AMBER, width=1),
            hovertemplate="%{x|%H:%M:%S}<br>%{y:.2f} bps<extra>spread</extra>",
        ), row=2, col=1)
    _style.apply(
        fig, "Top of book",
        "reconstructed best bid/ask, sampled every second (top panel: mid; "
        "bottom: quoted spread, 99.5% winsorized)",
    )
    fig.update_yaxes(title_text="mid (USD)", tickprefix="$", row=1, col=1)
    fig.update_yaxes(title_text="spread (bps)", row=2, col=1)
    return fig
