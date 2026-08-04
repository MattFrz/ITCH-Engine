"""Fill rate of passive orders bucketed by queue depth at join.

The queue-position argument in one picture: what actually fills, given
where you joined the FIFO queue - a fact naive backtests assume away.
"""

from __future__ import annotations

import plotly.graph_objects as go

from charts import _style


def figure(metrics: dict) -> go.Figure:
    rows = metrics.get("fill_rate_by_queue", [])
    fig = go.Figure()
    if rows:
        buckets = [r["bucket"] for r in rows]
        fig.add_trace(go.Bar(
            x=buckets, y=[r["fill_rate"] for r in rows],
            name="any fill", marker_color=_style.ACCENT,
            customdata=[r["orders"] for r in rows],
            hovertemplate="%{x} ahead<br>any fill %{y:.0%}"
                          "<br>%{customdata} orders<extra></extra>",
        ))
        fig.add_trace(go.Bar(
            x=buckets, y=[r["full_fill_rate"] for r in rows],
            name="full fill", marker_color=_style.ACCENT_SOFT,
            marker_line=dict(color=_style.ACCENT, width=0.5),
            hovertemplate="%{x} ahead<br>full fill %{y:.0%}<extra></extra>",
        ))
        fig.add_hline(
            y=1.0, line_color=_style.WARN, line_width=1, line_dash="dot",
            annotation_text="naive assumption: 100%",
            annotation_font=dict(size=11, color=_style.WARN),
        )
    _style.apply(
        fig, "Passive fill rate by queue depth at join",
        "per simulated order: shares resting ahead when it joined the queue",
    )
    fig.update_xaxes(title_text="shares ahead at join")
    fig.update_yaxes(title_text="fill rate", tickformat=".0%",
                     range=[0, 1.08])
    fig.update_layout(barmode="group", bargap=0.25)
    return fig
