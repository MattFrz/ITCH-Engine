"""Histogram of per-event Python<->C++ boundary latency during the backtest."""

from __future__ import annotations

import plotly.graph_objects as go

from charts import _style


def figure(metrics: dict) -> go.Figure:
    h = metrics.get("latency_histogram", {})
    edges = h.get("bin_edges_us", [])
    counts = h.get("counts", [])
    fig = go.Figure()
    if edges and counts:
        centers = [(edges[i] + edges[i + 1]) / 2 for i in range(len(counts))]
        width = (edges[1] - edges[0]) * 0.92 if len(edges) > 1 else 1
        fig.add_trace(go.Bar(
            x=centers, y=counts, width=width, marker_color=_style.ACCENT_SOFT,
            marker_line=dict(color=_style.ACCENT, width=0.5), name="events",
            hovertemplate="%{x:.1f} us<br>%{y:,} events<extra></extra>",
        ))
        for p, color in (("p50_us", _style.GREEN), ("p95_us", _style.AMBER),
                         ("p99_us", _style.WARN)):
            v = h.get(p)
            if v is not None:
                fig.add_vline(
                    x=v, line_color=color, line_width=1, line_dash="dash",
                    annotation_text=f"{p[:-3]} {v:.1f}us",
                    annotation_font=dict(size=11, color=color),
                )
    _style.apply(
        fig, "pybind11 boundary latency",
        f"apply_event wall time, {h.get('samples', 0):,} sampled calls - "
        "target <20 us/event",
    )
    fig.update_xaxes(title_text="microseconds / event")
    fig.update_yaxes(title_text="events", tickformat=",")
    return fig
