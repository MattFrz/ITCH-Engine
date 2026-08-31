# ITCH-Engine

[![CI](https://github.com/MattFrz/ITCH-Engine/actions/workflows/ci.yml/badge.svg)](https://github.com/MattFrz/ITCH-Engine/actions/workflows/ci.yml)

### C++ Order Book Reconstruction & Latency-Realistic Backtesting Infrastructure (Nasdaq ITCH/MBO)

## Summary

ITCH is the name of Nasdaq's market data protocol, the format the exchange uses to broadcast every order-level event (adds, cancels, modifies, executions) to the world. It is not an acronym: it is the playful companion to OUCH, Nasdaq's order-entry protocol, and neither has an official expansion. This project consumes the TotalView-ITCH feed (via Databento's XNAS.ITCH dataset) and rebuilds the full limit order book from it, so the name reads exactly as intended: an engine that processes Nasdaq ITCH data.

**What data does this use?**
The engine is built for Nasdaq XNAS.ITCH MBO (market-by-order) data pulled
from Databento: every order add, cancel, modify and execution that hit the
exchange on one day for one symbol, typically 10-15 million messages for a
liquid large-cap.

**The numbers in this README are from a real session**: AAPL, 2026-07-30,
14,469,900 normalized MBO messages.

If no `DATABENTO_API_KEY` is configured, ingestion generates a seeded
synthetic day with an identical schema, so the whole pipeline still runs end
to end with no paid data source and every step reproduces from a clean
clone. That synthetic day is also what the bundled `demo/` dataset and the
deployed viewer use, because licensed market data is not redistributed here.
The viewer labels which mode produced whatever you are looking at.

**What strategy does it trade?**
A single-feature order flow imbalance (OFI) signal. It watches the best
bid/ask and their sizes, and computes one number each time they change:
is buying pressure or selling pressure building at the top of the book
right now. If that number crosses a threshold, it buys or sells 100 shares
and holds for one second. It is deliberately simple on purpose (see "Why
this project" below) - the point of the project is not the strategy, it's
proving how differently the *same* strategy performs depending on how
honestly you simulate fills.

**How do I run it?**
See "Build & Run" further down for the exact commands. Short version:
build the C++ engine with CMake, run the validation script, run the
backtest script, then `streamlit run viewer/app.py` to see the dashboard.
Everything after the C++ build is plain Python.

**What does this actually prove?**
Two things, independently:

1. The order book reconstruction agrees exactly with an independent
   implementation. A second, deliberately simple Python book rebuilds the
   same state from the same events, and the two are compared at 1,000
   random points across the day: price levels, order counts, and each
   order's exact position in its queue. Zero mismatches. That rules out
   essentially every indexing and priority bug, but note what it is not:
   both books were written from the same reading of the spec, so it is
   agreement between implementations rather than proof against the
   exchange's own published book. See Limitations.
2. Backtests that assume instant, complete fills at your target price are
   dangerously optimistic. Running the same OFI signal, on the same clock,
   through a naive "fills at mid, zero latency" backtest versus an honest
   one (real FIFO order queues, a 2 ms data-latency handicap, a 3 ms
   order-latency handicap, and the actual exchange fee schedule) reverses
   the sign of the result. Numbers in the Results section below.

**Position discipline (and how the engine caught its own bug):**
An earlier version of the strategy could stack exit orders in fast markets
(it could not see its own in-flight orders, so it kept resubmitting exits
sized to the full position), letting the held position balloon far past
the intended 100 shares. The engine's own dashboard exposed this - max
position held is directly visible in the equity data - and the fix is
structural: the strategy sees every outstanding order (in-flight and
resting) and will not enter unless completely flat, nor submit an exit
while another exit is outstanding. The cap is structural rather than a
limit check, so the position never exceeds the intended 100 shares.

---

## One-line pitch
A validated, production-style market data infrastructure stack - exchange-native order book reconstruction (cross-checked against an independent reference implementation at 1,000 sampled timestamps, 0 drift), an event-driven backtester with queue-position fills, a latency model and real exchange fees, a profiled C++/Python hybrid core, and a decoupled static post-trade viewer. A trivial OFI signal is used only as a test client to demonstrate why naive backtests lie.

## Why this project
The interesting engineering in trading systems is the *infrastructure* strategies run on - and what matters there is correctness and performance, not alpha. This project leads with validation (proving the book is right), then performance (proving it's fast), then uses a deliberately simple signal at the end purely to demonstrate the engine's value. The post-trade viewer presents those findings and adds fill-level analysis (mark-outs, a filterable blotter, queue-depth breakdown), but it stays a decoupled static-file reader: it never touches the engine or the event loop.

## Scope
Deliberately small and airtight rather than broad:
- **One symbol, one day of data** for the entire project.
- **The signal is intentionally simple** - a single-feature order flow imbalance predictor. Its job is to produce one comparison number, not to be impressive on its own.
- **The dashboard is strictly decoupled** - a static-file reader over the backtester's outputs, never wired into the live engine.
- **Validation is the core deliverable.** Everything else - including the visualization layer - is in service of proving the book is correct and the engine is fast.

---

## Tech Stack
- **Core engine:** C++ (order book data structure, matching/fill logic) - industry-standard for performance-critical quant infra
- **Bindings:** pybind11 (C++ ↔ Python)
- **Orchestration / analysis:** Python (event loop, signal logic, backtest reporting)
- **Data storage:** Parquet (columnar, point-in-time correct, partitioned by symbol/day)
- **Data source:** Databento - `XNAS.ITCH` (Nasdaq TotalView-ITCH) MBO (Market-By-Order) feed, free-tier credits
- **Profiling:** perf / gprof (or equivalent) for hot-path optimization
- **Streamlit (or Plotly) for static post-trade analysis visualization** - reads only the files the backtester writes to disk; never touches the live engine

---

## Architecture Overview

```
Databento (XNAS.ITCH MBO) - ONE symbol, ONE day
        │
        ▼
[Phase 1] Data Ingestion & Normalization
   - Pull raw MBO messages, save raw Parquet immediately (preserve credits)
   - Normalize → internal schema (timestamp, order_id, side, price, qty, event_type)
   - Partitioned by symbol/date even at 1-day scale (production-style, not a hack)
        │
        ▼
[Phase 2] C++ Order Book Engine + VALIDATION (the real deliverable)
   - Price-level aggregation + per-order FIFO queue (true queue position)
   - Data structure choice justified explicitly (see below)
   - Validation script: C++ book state vs. an independent Python reference book
     at 1,000 random timestamps → assert 0 drift (comparison against exchange
     mbp-1 snapshots is the open item, see Limitations)
        │
        ▼
[Phase 3] Event-Driven Backtester (pure while event_queue loop - never vectorized)
   - Latency model: strategy sees data N ms late
   - Queue-position-aware partial fills
   - Profile the Python↔C++ boundary explicitly (pybind11 copy overhead is the
     usual hidden bottleneck) - target <20µs/event at that boundary
        │
        ▼
[Phase 4] Trivial ML Signal as Test Client (deliberately small)
   - Single-feature order flow imbalance (OFI) predicting next 1s mid-price move
   - Run through naive vectorized backtest AND the realistic engine
   - Headline result: how much of the apparent edge was a fill-assumption artifact
        │
        ▼
[Backtester writes to disk] trades.csv, metrics.json, snapshots.parquet
        │
        ▼
[Phase 5] Post-Trade Visualization & Reporting (fully decoupled, static-file reader)
   - Lightweight Streamlit/Plotly app reads ONLY the files above
   - Never connects to the live C++ engine or event loop - zero latency interference
   - 4-5 charts: Equity Curve, Latency Histogram, Fill-Rate by Queue Position, Signal Decay
   - FAILURE_MODES.md - the lead artifact, written first in spirit even if last in code
```

---

## Phase Breakdown

### Phase 1 - Data Ingestion (small and disposable)
- Pull 1 day of MBO data for one liquid large-cap symbol (e.g., AAPL or SPY)
- Save raw pull locally as Parquet immediately
- Normalize into internal schema; partition by symbol/date
- **Deliverable:** script that replays the stored day tick-by-tick and reconstructs book state at any timestamp

### Phase 2 - C++ Order Book Engine + Validation
- Limit order book: price-level aggregation + per-order FIFO queue
- **Data structure decision:** a naive `std::vector<OrderID>` per price level makes cancel-in-the-middle an O(N) operation. This book uses `std::list` with stored iterators so cancel/modify is O(1) - the full argument is in [docs/data_structure_tradeoffs.md](docs/data_structure_tradeoffs.md).
- Exposed to Python via pybind11
- **Deliverable (the one that matters most):** a validation script asserting exact agreement between the C++ book and an independent reference implementation at 1,000 random timestamps, 0 drift. The PASS output is reproduced in the Results section below.

### Phase 3 - Event-Driven Backtester
- True event loop calling into the C++ book - no `pandas.apply`, no vectorization, anywhere
- Latency model: strategy sees data N ms late; queue-position-aware partial fills; slippage tied to real book depth
- **The pybind11 boundary is profiled specifically** - µs/event crossing Python↔C++, not just internal C++ speed. This is the usual hidden bottleneck in hybrid systems.
- **Deliverable:** run a simple strategy through it and quantify fill realism vs. a naive "fills at mid" backtest
- Backtester writes its outputs to disk (`trades.csv`, `metrics.json`, `snapshots.parquet`) - this is the handoff point to Phase 5, and the only handoff point. No other component reads or writes these files.

### Phase 4 - Test-Client Signal (kept deliberately small)
- One-feature OFI predictor of the next 1-second mid-price move - nothing more elaborate
- Run through both the naive vectorized backtest and the realistic engine
- **Deliverable / thesis statement:** how much of the signal's apparent alpha was an artifact of unrealistic fill assumptions rather than real edge. See the Results section for the measured answer.

### Phase 5 - Post-Trade Visualization & Reporting
This phase exists to **present** the findings from Phases 2-4 clearly. It is explicitly **not** a production monitoring system, and it is **not** where the engineering effort of this project lives. Validation (Phase 2) and latency profiling (Phase 3) remain the headline deliverables; this is packaging.

Hard boundaries on this layer, by design:
- **Strictly a decoupled, static-file reader.** It reads `trades.csv`, `metrics.json`, and `snapshots.parquet` from disk - the same files Phase 3 already writes. Nothing more.
- **Never connects to the live C++ engine or the event loop, under any circumstance.** UI rendering is completely separated from the performance-critical path. This is intentional: a backtester that produces analysis files and a viewer that reads them afterward is a real production pattern; a UI wired into a live engine is a latency liability and architecturally the wrong call for this kind of tool. Decoupling here is the point being demonstrated.
- **No websockets, no real-time updates, no complex state management.** Streamlit and Plotly, rendering static charts from static files.
- **A handful of charts, no more:** Equity Curve, Top of Book, Fill-Rate by Queue Position, Signal Decay, Latency Histogram.

Also in this phase:
- Profile the C++ core, document before/after latency from any optimization
- Static JSON output: latency percentiles (p50/p95/p99), fill rates
- A handful of plotly-rendered PNGs embedded in the README (so the README doesn't depend on anyone running the Streamlit app)
- **`FAILURE_MODES.md` at repo root** - 2 focused sections: why queue position matters, why delayed data kills momentum-style signals - both written from this engine's own measured output.

---

## Build & Run

Prereqs: CMake >=3.20, a C++17 compiler (MSVC/GCC/Clang), Python 3.9+ with
the requirements installed. pybind11 is located automatically from the
Python environment, so no extra CMake flags are needed beyond the ones
shown.

On Windows, use `py -3.9` wherever these commands say `python`, and
`py -3.9 -m pip` for `pip` (the plain names are often not on PATH). Run
the CMake commands from an "x64 Native Tools Command Prompt for VS" so the
MSVC compiler is available.

Windows (copy-paste as-is, from the repo root):

```bash
py -3.9 -m pip install -r python/requirements.txt
```

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPYBIND11_FINDPYTHON=ON -DPython_EXECUTABLE="%LOCALAPPDATA%\Programs\Python\Python39\python.exe"
```

(The `Python_EXECUTABLE` pin makes CMake build against the same Python
that runs the scripts; without it, CMake may pick a different installed
Python, such as a Conda one, and the module will not import.)

```bash
cmake --build build
```

```bash
build\cpp\test_order_book.exe
```

```bash
py -3.9 validation/validate_book.py
```

```bash
py -3.9 scripts/run_backtest.py
```

```bash
py -3.9 profiling/profile_pybind_boundary.py
```

```bash
py -3.9 -m streamlit run viewer/app.py --server.address localhost
```

(`--server.address localhost` keeps the viewer on the loopback interface.
Streamlit's default is `0.0.0.0`, which publishes an unauthenticated
dashboard to every host on your network. It is not set in
`.streamlit/config.toml` because hosted deployments need to bind
externally.)

Linux/macOS equivalents:

```bash
pip install -r python/requirements.txt
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPYBIND11_FINDPYTHON=ON
cmake --build build
./build/cpp/test_order_book
python validation/validate_book.py
python scripts/run_backtest.py          # --rth-only restricts to 09:30-16:00 ET
python profiling/profile_pybind_boundary.py
streamlit run viewer/app.py --server.address localhost
```

Step order matters: build first, then validation (ingests data on first
run; synthetic unless `DATABENTO_API_KEY` is set), then the backtest
(writes `output/`), then the viewer (reads `output/`).

Without a Databento key, ingestion generates a seeded, schema-identical
synthetic MBO day so every phase runs end-to-end; with
`DATABENTO_API_KEY` set (and `pip install databento`), the same commands
pull and cache one real XNAS.ITCH day. The results below are from the
synthetic day, so they reproduce from a clean clone with no key.

### Pulling a new day, and what happens to old data

Step-by-step Windows commands (including API key setup):
[docs/PULLING_DATA.md](docs/PULLING_DATA.md).

Pull + normalize a day on its own (prints the record count and dollar cost
first if `DATABENTO_API_KEY` is set, and asks for confirmation before
spending credits), then run the pipeline on it:

```bash
py -3.9 scripts/pull_day.py --symbol AAPL --day 2026-08-03
```

```bash
py -3.9 scripts/run_backtest.py --symbol AAPL --day 2026-08-03
```

Nothing is deleted automatically. Every day you pull is cached forever in
its own folder (`data/raw/symbol=X/date=Y/`, `data/processed/symbol=X/
date=Y/`), so pulling a new day never touches an older one - they just
accumulate on disk. `output/` (the three files the viewer reads) is the one
exception: it is a single, unpartitioned location by design, so each new
backtest run overwrites the previous one's results. To avoid silently
losing a day's results, `write_outputs()` automatically archives the
existing `output/` into `output_archive/<symbol>_<day>_<timestamp>/`
before overwriting it.

See what's cached and how much disk it's using (never deletes anything):

```bash
py -3.9 scripts/cleanup_data.py
```

Reclaim space, keeping only the most recent day per symbol:

```bash
py -3.9 scripts/cleanup_data.py --keep-latest 1 --delete
```

Delete one specific cached day:

```bash
py -3.9 scripts/cleanup_data.py --symbol AAPL --day 2026-07-01 --delete
```

Prune old `output_archive/` snapshots, keeping the 3 most recent:

```bash
py -3.9 scripts/cleanup_data.py --archives --keep-latest 3 --delete
```

## Results (real XNAS.ITCH MBO: AAPL, 2026-07-30, 14.47M events)

Every number in this section comes from one real Nasdaq trading session
pulled from Databento: 14,771,847 raw MBO records, 14,469,900 after
normalization, spanning 03:04 to 19:59 ET (so pre-market and after-hours
are included, not just the regular session).

Without a `DATABENTO_API_KEY` the same commands run end to end against a
seeded synthetic day instead, which is what the bundled `demo/` dataset and
the deployed viewer use. Real market data is not redistributed in this repo.

**Book validation** (`validation/results/validation_run.txt`):

```
symbol=AAPL day=2026-07-30
events replayed: 14,469,900
checkpoints: 1,000 random timestamps
assertions: 8,000
mismatches: 0
RESULT: PASS - 0 drift across all sampled timestamps
```

At every checkpoint the C++ book is compared against an independently
written pure-Python reference book: top-5 levels per side (price, aggregate
qty, order count), best quotes, open order count, and exact FIFO
`queue_ahead` for sampled live orders.

Read this for what it is. It proves the two implementations agree exactly,
which catches essentially every indexing, priority and bookkeeping bug. It
does **not** prove either one reads the ITCH semantics the way Nasdaq
does, because both were written from the same interpretation. Validating
against exchange-published `mbp-1` snapshots would prove that, and is the
main open item (see Limitations).

Checked at every checkpoint against an independently written pure-Python
reference book: top-5 levels per side (price, aggregate qty, order count),
best quotes, open order count, and exact FIFO `queue_ahead` for sampled live
orders. About 1.3% of feed messages reference orders resting from before the
capture window; both books treat them identically and count them
(`unknown_order_events` in metrics.json). The session also contains exactly
one `R` (Clear) message, which wipes the book and is applied by both.

Read the result for what it is: it proves the two implementations agree,
which rules out essentially every indexing and priority bug. It does not
prove either matches Nasdaq's own published book, because both were written
from the same reading of the spec. See Limitations.

**Boundary profiling** (`profiling/results/latency_percentiles.json`):
`apply_event` costs **0.88 µs/event** mean (p50 0.9, p95 1.1, p99 1.3),
comfortably inside the <20 µs target. Batch replay runs at **0.13 µs/event**
(~7.8M events/s through the C++ book) because it crosses the pybind11
boundary once instead of once per event. About **85% of the scalar cost is
the crossing itself**, not book work, which is the whole argument for the
batch path on replay-only workloads.

**Headline result:** the naive vectorized backtest of the OFI signal shows
**+$369** on the day. The same signal, on the same 100 ms clock, run through
FIFO queue position, a 2 ms / 3 ms latency model and real exchange fees,
shows **-$2,679**. The naive backtest did not merely overstate the edge, it
got the *sign* wrong: the apparent alpha was entirely a fill-assumption
artifact.

The gap decomposes, which is the part worth reading:

| | |
|---|---|
| naive backtest | **+$368.50** |
| realistic, before fees | -$2,617.78 |
| exchange fees paid (net of maker rebates) | -$61.23 |
| **realistic, all-in** | **-$2,679.01** |

Fees are only $61 of the $3,048 swing. The overwhelming majority is queue
position, latency and the adverse selection they produce: the realistic run
loses roughly 2 cents per share across 3,601 fills and 116,814 shares
against real HFT flow, with no single episode dominating.

Passive fill rate was **45.7%** against the naive assumption of 100%, across
1,801 passive orders. Position discipline is structural rather than a limit
check, and the output confirms it: maximum absolute position held all day
was exactly the intended 100 shares.

Both paths derive the signal from best quotes observed on the *same*
sampling grid, so the difference above is attributable to execution realism
rather than to the two backtests quietly running different signals. This run
trades the full session; `scripts/run_backtest.py --rth-only` restricts it to
regular hours, 09:30-16:00 ET, excluding the thin pre- and post-market books.
Full argument: [FAILURE_MODES.md](FAILURE_MODES.md).

![Equity curve](docs/images/equity_curve.png)

| | |
|---|---|
| ![Fill rate by queue](docs/images/fill_rate_by_queue.png) | ![Latency histogram](docs/images/latency_histogram.png) |
| ![Signal decay](docs/images/signal_decay.png) | ![Top of book](docs/images/top_of_book.png) |

---

## Limitations

Written down deliberately. Everything here is a known boundary of the
current build, not something discovered later.

**Validation proves agreement, not exchange truth.** The C++ book and the
Python reference book agree exactly across every sampled checkpoint, but
both were written from the same reading of the MBO semantics, so a shared
misinterpretation would agree perfectly and still be wrong. The real check
is a comparison against exchange-published `mbp-1` snapshots
(`validation/reference_snapshots/`), which is not yet implemented. This is
the single most valuable open item in the repo.

**Scope is one symbol, one day, one venue.** There is no NBBO, so "mid"
means the XNAS mid and not the national mid, and the mark-out numbers are
measured against it accordingly. Nothing here streams: a day is loaded whole
into memory.

**Market structure not modeled.** Opening and closing crosses, halts, LULD
bands, and odd-lot handling are all absent. `T` (trades printed with no book
impact, including against hidden liquidity) is intentionally dropped, so
hidden-liquidity volume is invisible to both the book and the signal. `R`
(Clear) *is* handled.

**Simulated orders are phantom.** They never mutate the replayed book, so
there is no market impact and no self-impact. That is a fair approximation
at 100 shares and wrong at institutional size.

**Fill model approximations.** Queue advancement on feed cancels uses the
standard expected-position estimate (`cancel_qty * queue_ahead / level_qty`)
because MBO cancels do not say where in the queue the cancelled order sat.
`Modify` events are ignored for queue advancement, which is conservative:
a simulated order never gains priority from them. Marketable orders walk at
most 10 levels and behave as IOC, so any unfilled remainder is cancelled
rather than left resting.

**Costs are exchange fees only.** The Nasdaq displayed-order schedule
(taker $0.0030/share, maker rebate $0.0020/share) is applied to every fill.
These are list rates; a real desk negotiates tiers and pays less. Clearing
fees, regulatory fees, borrow costs and market-data costs are not modeled.

**Event loop scales to this strategy, not to a quoting engine.** Order
arrival and phantom-queue bookkeeping use linear scans over outstanding
orders, which is invisible at 2,061 orders and would need a heap and a
price-indexed order map for a strategy that quotes continuously.

**The signal is a test client, not alpha.** A single-feature OFI predictor
exists to produce one comparison number. It is not tuned, and it should not
be read as a trading strategy.

## Design Highlights
- **Validation-first** - the order book is cross-checked to 0 drift against an independent reference implementation across 1,000 sampled timestamps before anything is built on top of it
- **Real exchange-native data** - XNAS.ITCH MBO via Databento, with all the real-feed messiness handled explicitly (session-boundary orders, cancel-dominated message mix)
- **C++/Python hybrid with the boundary measured** - the pybind11 crossing is profiled per event, not assumed fast
- **Explicit data-structure reasoning** - O(1) cancel/modify via list-iterator indexing, justified against the actual message mix in [docs/data_structure_tradeoffs.md](docs/data_structure_tradeoffs.md)
- **Quantified realism gap** - the same signal run through naive and realistic fill assumptions, with the difference measured rather than hand-waved
- **Decoupled post-trade layer** - the viewer reads static output files only and never touches the engine, keeping analysis off the performance-critical path
- **[FAILURE_MODES.md](FAILURE_MODES.md)** - the two ways backtests lie, written from this engine's own measured output

## Project Status
- [x] Ingestion: real XNAS.ITCH day pulled, cached and normalized (14.47M MBO events), including `R`/Clear; seeded synthetic fallback for keyless runs
- [x] C++ order book + validation: 0 drift across 1,000 random checkpoints (8,000 assertions) on a real 14.47M-message session
- [x] Event-driven backtester with two-book latency model, queue-aware fills, exchange fees, and structural position discipline
- [x] pybind11 boundary profiled: 0.88 µs/event scalar, 0.13 µs batched (~7.8M events/s), <20 µs target met
- [x] OFI test client: naive backtest reversed the PnL sign vs the realistic engine (+$369 vs -$2,679, same signal and clock)
- [x] Streamlit viewer with mark-out analysis, filterable blotter and drilldown; README chart PNGs (`scripts/export_charts.py`)
- [x] CI on Linux and Windows: C++ tests, Python tests, book validation, end-to-end backtest
- [ ] **Open:** validate against exchange-published `mbp-1` snapshots (see Limitations)
- [ ] Possible extensions: more symbols/days, an intrusive-list book variant, a C++ fill model for full-speed strategy sweeps
