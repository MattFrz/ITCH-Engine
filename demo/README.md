# Bundled demo run

A committed snapshot of one backtest run, so a deployed copy of the viewer
has something to render. `output/` is gitignored (it is whatever the last
local run wrote), so without this the deployed app would only ever show its
empty state.

**This is the seeded synthetic day, not the real session the README reports.**
That is deliberate. The README's numbers come from a real Databento
XNAS.ITCH pull (AAPL, 2026-07-30), but `snapshots.parquet` is reconstructed
book state derived from licensed market data, and redistributing it in a
public repository is not something this project does. The synthetic day is
schema-identical, carries no licensing restrictions, and exercises every
code path the viewer has.

The viewer labels it in the header: `SYNTHETIC DATA` and `BUNDLED DEMO RUN`
both appear, so nobody mistakes the deployed demo for the real result.

Regenerate the demo set (keyless, so it stays synthetic):

    # with DATABENTO_API_KEY unset, or pointed at a symbol/day you have not pulled
    python scripts/run_backtest.py
    cp output/trades.csv output/metrics.json output/snapshots.parquet demo/

The viewer prefers `output/` whenever it exists, so a local run always
overrides these files.
