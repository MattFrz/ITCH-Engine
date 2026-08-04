"""Signal decay: OFI information coefficient vs. prediction horizon.

Read it with the latency model in mind: a strategy seeing data D ms late,
deciding on a sampling clock and landing O ms after that, harvests this
curve shifted right by its whole round trip - not the peak.
"""

from __future__ import annotations

import plotly.graph_objects as go

from charts import _style


def figure(metrics: dict) -> go.Figure:
    rows = metrics.get("signal_decay", [])
    fig = go.Figure()
    if rows:
        x = [r["horizon_ms"] for r in rows]
        y = [r["ic"] for r in rows]
        fig.add_trace(go.Scatter(
            x=x, y=y, mode="lines+markers", name="IC",
            line=dict(color=_style.ACCENT, width=2),
            marker=dict(size=7, color=_style.ACCENT),
            hovertemplate="%{x} ms<br>IC %{y:.3f}<extra></extra>",
        ))
        fig.add_hline(y=0, line_color=_style.MUTED, line_width=1, line_dash="dot")
        total_lat = (
            metrics.get("params", {}).get("data_latency_ms", 0)
            + metrics.get("params", {}).get("order_latency_ms", 0)
        )
        if total_lat:
            fig.add_vline(
                x=total_lat, line_color=_style.WARN, line_width=1, line_dash="dash",
                annotation_text=f"round trip ~{total_lat:.0f} ms",
                annotation_font=dict(size=11, color=_style.WARN),
            )
    _style.apply(
        fig, "OFI signal decay",
        "correlation of windowed OFI with the future mid move, by horizon",
    )
    fig.update_xaxes(title_text="horizon (ms)", type="log")
    fig.update_yaxes(title_text="information coefficient", tickformat=".3f")
    return fig
