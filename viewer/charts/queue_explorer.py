"""Queue depth at join vs. what you got for it.

Bars are how many passive fills came from each queue-depth bucket; the line
is how adversely selected those fills turned out to be one second later.
The two together are the queue-position argument: joining the back of a
deep queue does not just fill less often, it fills *worse* - you are filled
precisely when someone is willing to run through the level.
"""

from __future__ import annotations

import pandas as pd
import plotly.graph_objects as go
from plotly.subplots import make_subplots

from charts import _style


def figure(buckets: pd.DataFrame) -> go.Figure:
    fig = make_subplots(specs=[[{"secondary_y": True}]])
    if not buckets.empty:
        fig.add_trace(go.Bar(
            x=buckets["bucket"].astype(str), y=buckets["fills"],
            name="fills", marker_color=_style.ACCENT_SOFT,
            marker_line=dict(color=_style.ACCENT, width=0.5),
            hovertemplate="%{x} ahead<br>%{y:,} fills<extra></extra>",
        ), secondary_y=False)
        fig.add_trace(go.Scatter(
            x=buckets["bucket"].astype(str), y=buckets["markout_cents"],
            name="1s mark-out", mode="lines+markers",
            line=dict(color=_style.WARN, width=2),
            marker=dict(size=8, color=_style.WARN,
                        line=dict(color=_style.CORE, width=1.5)),
            hovertemplate="%{x} ahead<br>%{y:+.3f} c/share at 1s<extra></extra>",
        ), secondary_y=True)
        fig.add_hline(y=0, line_color=_style.MUTED, line_width=1,
                      line_dash="dot", secondary_y=True)
    _style.apply(
        fig, "Queue depth at join vs. 1s mark-out",
        "passive fills only - bars are volume, the line is how badly those "
        "fills aged after one second",
    )
    fig.update_xaxes(title_text="shares ahead when the order joined")
    fig.update_yaxes(title_text="fills", tickformat=",", secondary_y=False)
    fig.update_yaxes(title_text="1s mark-out (c/share)", ticksuffix="c",
                     tickformat="+.2f", showgrid=False, secondary_y=True)
    return fig
