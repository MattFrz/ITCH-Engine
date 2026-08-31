"""Book context around one selected fill - the blotter's drilldown.

Shows the reconstructed best bid/ask either side of a single fill, with the
fill marked at its own price, so you can see what the book looked like when
the order was filled and where it went afterwards. This is the view that
turns "the mark-out was -1.8c" into "here is the sweep that did it".
"""

from __future__ import annotations

import pandas as pd
import plotly.graph_objects as go

from charts import _style

BEFORE_S, AFTER_S = 30, 60


def figure(quotes: pd.DataFrame, trade: pd.Series) -> go.Figure:
    fig = go.Figure()
    ts = int(trade["ts"])
    lo, hi = ts - BEFORE_S * 1_000_000_000, ts + AFTER_S * 1_000_000_000
    w = quotes[(quotes["ts"] >= lo) & (quotes["ts"] <= hi)]

    if not w.empty:
        t = _style.to_chart_time(w["ts"].to_numpy())
        # Spread drawn as a band rather than two lines: the fill's position
        # inside or outside it is the thing being read here.
        fig.add_trace(go.Scatter(
            x=t, y=w["ask"], name="ask", mode="lines",
            line=dict(color=_style.MUTED, width=1),
            hovertemplate="%{x|%H:%M:%S}<br>ask $%{y:,.2f}<extra></extra>",
        ))
        fig.add_trace(go.Scatter(
            x=t, y=w["bid"], name="bid", mode="lines", fill="tonexty",
            fillcolor="rgba(63,191,212,.07)",
            line=dict(color=_style.MUTED, width=1),
            hovertemplate="%{x|%H:%M:%S}<br>bid $%{y:,.2f}<extra></extra>",
        ))
        fig.add_trace(go.Scatter(
            x=t, y=w["mid"], name="mid", mode="lines",
            line=dict(color=_style.ACCENT, width=1.6),
            hovertemplate="%{x|%H:%M:%S}<br>mid $%{y:,.2f}<extra></extra>",
        ))

    fill_t = _style.to_chart_time([ts])
    is_buy = trade["side_label"] == "buy"
    fig.add_trace(go.Scatter(
        x=fill_t, y=[trade["price_usd"]], name="fill", mode="markers",
        marker=dict(size=13, color=_style.GREEN if is_buy else _style.WARN,
                    symbol="triangle-up" if is_buy else "triangle-down",
                    line=dict(color=_style.CORE, width=1.5)),
        hovertemplate=f"{trade['side_label']} {int(trade['qty'])} @ "
                      f"$%{{y:,.2f}}<extra>fill</extra>",
    ))
    fig.add_vline(x=fill_t[0], line_color=_style.MUTED, line_width=1,
                  line_dash="dot")

    _style.apply(
        fig, "Book around the fill",
        f"reconstructed best quotes, {BEFORE_S}s before to {AFTER_S}s after",
    )
    fig.update_yaxes(title_text="price (USD)", tickprefix="$")
    fig.update_xaxes(title_text="")
    fig.update_layout(height=340)
    return fig
