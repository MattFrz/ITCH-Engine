"""Shared chart styling - one place, so every figure reads as one system.

Dark terminal palette tuned for a post-trade dashboard: off-black navy
canvas, one desaturated cyan accent for "realistic/engine", one warm
accent for "naive/optimistic", muted structure everywhere else.

Numerals are set in a monospace face throughout. In a dashboard where the
reader is comparing figures down a column, proportional digits make the
comparison harder than it needs to be; tabular ones line up.
"""

from __future__ import annotations

import pandas as pd
import plotly.graph_objects as go

ET = "America/New_York"


def to_chart_time(ts_ns) -> pd.DatetimeIndex:
    """Epoch nanoseconds -> naive Eastern wall-clock, ready for plotly.

    Converted to exchange time so the 09:30 open reads as 09:30 rather than
    as UTC, then stripped of tzinfo: static PNG export serializes the figure
    to JSON, and tz-aware Timestamps are not JSON serializable, so leaving
    them attached breaks `scripts/export_charts.py` while looking fine in
    the browser.
    """
    return pd.to_datetime(ts_ns, unit="ns", utc=True).tz_convert(ET).tz_localize(None)

# --- surfaces ------------------------------------------------------------
BG = "#080b12"          # page canvas (matches .streamlit/config.toml)
PANEL = "#0f141d"       # outer card shell
CORE = "#131924"        # inner core - what a figure actually sits on
PANEL_RAISED = "#18202c"
BORDER = "#212a38"
GRID = "#19212d"

# --- type ----------------------------------------------------------------
TEXT = "#e8ebf1"
MUTED = "#808b9c"       # lifted for contrast on the darker ground

# --- accents -------------------------------------------------------------
# One accent (cyan) carries "the engine / the honest number". One opposing
# warm hue carries "naive / optimistic assumption". Everything else is a
# muted marker, kept under ~70% saturation so nothing screams.
ACCENT = "#3fbfd4"
ACCENT_SOFT = "#1c4d59"
WARN = "#e2725b"
AMBER = "#d9a441"
GREEN = "#5fb98a"

FONT = "Geist, 'Segoe UI', system-ui, -apple-system, sans-serif"
MONO = "'Geist Mono', 'JetBrains Mono', 'Cascadia Code', Consolas, monospace"


def apply(fig: go.Figure, title: str, subtitle: str | None = None) -> go.Figure:
    """Applies the house layout to a figure."""
    title_text = f"<b>{title}</b>"
    if subtitle:
        title_text += (
            f"<br><span style='font-size:11.5px;font-weight:400;"
            f"color:{MUTED}'>{subtitle}</span>"
        )
    fig.update_layout(
        title=dict(text=title_text, font=dict(size=16, color=TEXT),
                   x=0, xanchor="left", y=0.97, yanchor="top"),
        font=dict(family=FONT, size=12, color=TEXT),
        paper_bgcolor="rgba(0,0,0,0)",
        plot_bgcolor="rgba(0,0,0,0)",
        margin=dict(t=76, r=12, b=44, l=60),
        legend=dict(
            orientation="h", y=1.0, x=1.0, xanchor="right", yanchor="bottom",
            bgcolor="rgba(0,0,0,0)", font=dict(size=11, color=MUTED),
        ),
        hoverlabel=dict(bgcolor=PANEL_RAISED, bordercolor=BORDER,
                        font=dict(family=MONO, size=11, color=TEXT)),
    )
    # Horizontal rules only: vertical gridlines add noise to a time series
    # without helping anyone read a value off the y-axis.
    fig.update_xaxes(
        showgrid=False, zeroline=False, showline=True, linecolor=BORDER,
        ticks="outside", ticklen=4, tickcolor=BORDER,
        tickfont=dict(family=MONO, size=10.5, color=MUTED),
        title_font=dict(family=FONT, size=11, color=MUTED),
    )
    fig.update_yaxes(
        gridcolor=GRID, zeroline=False, showline=False,
        tickfont=dict(family=MONO, size=10.5, color=MUTED),
        title_font=dict(family=FONT, size=11, color=MUTED),
    )
    return fig
