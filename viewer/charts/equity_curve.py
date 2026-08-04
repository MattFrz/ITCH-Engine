"""Equity curve: naive vectorized backtest vs. the realistic engine."""

from __future__ import annotations

import pandas as pd
import plotly.graph_objects as go

from charts import _style


def figure(metrics: dict) -> go.Figure:
    fig = go.Figure()
    for key, name, color, width in (
        ("equity_naive", "Naive - fills at mid, zero latency", _style.WARN, 1.4),
        ("equity_realistic", "Realistic - queues + latency", _style.ACCENT, 1.8),
    ):
        rows = metrics.get(key, [])
        if not rows:
            continue
        t = pd.to_datetime([r["ts"] for r in rows], unit="ns")
        fig.add_trace(go.Scatter(
            x=t, y=[r["equity"] for r in rows], name=name,
            mode="lines", line=dict(color=color, width=width),
            hovertemplate="%{x|%H:%M:%S}<br>$%{y:,.0f}<extra>" + name + "</extra>",
        ))
    fig.add_hline(y=0, line_color=_style.MUTED, line_width=1, line_dash="dot")
    _style.apply(
        fig, "Equity - the same signal, two fill assumptions",
        "identical OFI decisions; the gap is entirely fill realism",
    )
    fig.update_yaxes(title_text="PnL (USD)", tickprefix="$", tickformat=",")
    fig.update_xaxes(title_text="")
    return fig
