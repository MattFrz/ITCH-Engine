"""Exports the viewer charts as PNGs into docs/images/ for the README.

Same chart modules the Streamlit app uses, same output files as input - the
README images can never drift from what the viewer shows. The transparent
paper background is swapped for the theme canvas so the PNGs read correctly
on GitHub in both light and dark mode.

    python scripts/export_charts.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pandas as pd

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "viewer"))

from charts import (  # noqa: E402
    _style,
    equity_curve,
    fill_rate_by_queue,
    latency_histogram,
    signal_decay,
    top_of_book,
)

IMAGES = REPO_ROOT / "docs" / "images"


def main() -> int:
    metrics = json.loads((REPO_ROOT / "output" / "metrics.json").read_text())
    snapshots = pd.read_parquet(REPO_ROOT / "output" / "snapshots.parquet")
    IMAGES.mkdir(parents=True, exist_ok=True)

    figures = {
        "equity_curve": equity_curve.figure(metrics),
        "latency_histogram": latency_histogram.figure(metrics),
        "fill_rate_by_queue": fill_rate_by_queue.figure(metrics),
        "signal_decay": signal_decay.figure(metrics),
        "top_of_book": top_of_book.figure(snapshots),
    }
    for name, fig in figures.items():
        fig.update_layout(paper_bgcolor=_style.BG)
        out = IMAGES / f"{name}.png"
        fig.write_image(str(out), width=900, height=500, scale=2)
        print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
