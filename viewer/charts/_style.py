"""Shared chart styling - one place, so every figure reads as one system.

Dark terminal palette tuned for a post-trade dashboard: near-black canvas,
one cool accent for "realistic/engine", one warm accent for "naive/warning",
muted structure everywhere else.
"""

from __future__ import annotations

import plotly.graph_objects as go

BG = "#0b0f19"          # page canvas (matches .streamlit/config.toml)
PANEL = "#111827"       # chart panel
GRID = "#1f2937"
TEXT = "#e5e7eb"
MUTED = "#6b7280"

ACCENT = "#22d3ee"      # cyan - engine / realistic
ACCENT_SOFT = "#164e63"
WARN = "#fb7185"        # rose - naive / thresholds
AMBER = "#fbbf24"
GREEN = "#34d399"

FONT = "Segoe UI, system-ui, sans-serif"


def apply(fig: go.Figure, title: str, subtitle: str | None = None) -> go.Figure:
    """Applies the house layout to a figure."""
    title_text = f"<b>{title}</b>"
    if subtitle:
        title_text += f"<br><span style='font-size:12px;color:{MUTED}'>{subtitle}</span>"
    fig.update_layout(
        title=dict(text=title_text, font=dict(size=17, color=TEXT),
                   x=0.01, xanchor="left"),
        font=dict(family=FONT, size=12, color=TEXT),
        paper_bgcolor="rgba(0,0,0,0)",
        plot_bgcolor=PANEL,
        margin=dict(t=70, r=16, b=48, l=56),
        legend=dict(
            orientation="h", y=1.0, x=1.0, xanchor="right", yanchor="bottom",
            bgcolor="rgba(0,0,0,0)", font=dict(size=11),
        ),
        hoverlabel=dict(bgcolor=PANEL, bordercolor=GRID, font=dict(family=FONT)),
    )
    fig.update_xaxes(gridcolor=GRID, zerolinecolor=GRID, linecolor=GRID,
                     tickfont=dict(color=MUTED))
    fig.update_yaxes(gridcolor=GRID, zerolinecolor=GRID, linecolor=GRID,
                     tickfont=dict(color=MUTED))
    return fig
