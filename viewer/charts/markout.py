"""Adverse selection: where the mid went after each fill.

The single most useful post-trade question, and the one a naive backtest
cannot ask at all. Signed so that below zero always means the market moved
against the fill - maker fills sitting well below zero is the signature of
being picked off rather than providing liquidity profitably.
"""

from __future__ import annotations

import pandas as pd
import plotly.graph_objects as go

from charts import _style

LIQ_STYLE = {
    "maker": (_style.ACCENT, "passive (maker)"),
    "taker": (_style.AMBER, "aggressive (taker)"),
}


def figure(summary: pd.DataFrame) -> go.Figure:
    fig = go.Figure()
    if not summary.empty:
        for liq, (color, label) in LIQ_STYLE.items():
            sub = summary[summary["liquidity"] == liq]
            if sub.empty:
                continue
            fig.add_trace(go.Scatter(
                x=sub["horizon_s"], y=sub["markout_cents"],
                name=label, mode="lines+markers",
                line=dict(color=color, width=2, shape="spline", smoothing=0.4),
                marker=dict(size=8, color=color,
                            line=dict(color=_style.CORE, width=1.5)),
                customdata=sub["fills"],
                hovertemplate="%{x}s after fill<br>%{y:+.3f} c/share"
                              "<br>%{customdata:,} fills<extra>" + label + "</extra>",
            ))
        fig.add_hline(y=0, line_color=_style.MUTED, line_width=1, line_dash="dot")
    _style.apply(
        fig, "Mark-out after fill",
        "signed mark-out of the mid against each fill - below zero is the "
        "market moving against you",
    )
    fig.update_xaxes(title_text="seconds after fill", type="log",
                     tickvals=[1, 5, 30, 60], ticktext=["1s", "5s", "30s", "60s"])
    fig.update_yaxes(title_text="cents / share", ticksuffix="c", tickformat="+.2f")
    return fig
