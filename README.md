# ITCH-Engine

### C++ Order Book Reconstruction & Latency-Realistic Backtesting Infrastructure (Nasdaq ITCH/MBO)

## One-line pitch
A validated, production-style market data infrastructure stack - real exchange order book reconstruction (proven correct against reference snapshots), an event-driven backtester with realistic fills/latency, a profiled C++/Python hybrid core, and a decoupled static post-trade viewer for presenting results. A trivial ML signal is used only as a test client to demonstrate why naive backtests lie.

## Why this project
Most student projects show a trading *strategy*. Quant dev roles hire for the *infrastructure* that strategies run on, and they care about correctness and performance, not alpha. This project leads with validation (proving the book is right), then performance (proving it's fast), then uses a deliberately simple signal at the end purely to demonstrate the engine's value. A lightweight visualization layer exists only to present findings clearly - it is explicitly not the focus.

## Scope discipline (read this before starting)
This is sized to be genuinely finishable, not a 6-month infra build:
- **One symbol, one day of data** for the entire project. Don't expand until everything below is rock-solid.
- **The ML signal is intentionally dumb** - a single-feature order flow imbalance predictor, nothing more. Its job is to produce one comparison number, not to be impressive on its own.
- **The dashboard is intentionally minimal** - a static-file reader, capped at ~2 hours of dev time. It is not where your engineering effort should go.
- **Validation is the actual hard part and the actual deliverable.** Everything else - including the visualization layer - is in service of proving the book is correct and the engine is fast.

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
   - Validation script: C++ book state vs. Databento reference snapshot
     at 1,000 random timestamps → assert 0 drift → green PASS screenshot in README
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

### Phase 2 - C++ Order Book Engine + Validation (do not skip ahead from here)
- Limit order book: price-level aggregation + per-order queue
- **Data structure decision, stated explicitly in the writeup:** a naive `std::vector<OrderID>` per price level makes cancel-in-the-middle an O(N) operation. Use `std::list` with stored iterators (or an intrusive linked list) so cancel/modify is O(1). This single paragraph in your README signals you've actually thought about the problem.
- Exposed to Python via pybind11
- **Deliverable (this is the one that matters most):** a validation script asserting `cpp_book.get_level(price) == databento_snapshot` at 1,000 random timestamps, 0 drift. Screenshot the PASS output for the README - this is more credible than the engine itself.

### Phase 3 - Event-Driven Backtester
- True event loop calling into the C++ book - no `pandas.apply`, no vectorization, anywhere
- Latency model: strategy sees data N ms late; queue-position-aware partial fills; slippage tied to real book depth
- **Profile the pybind11 boundary specifically** - measure µs/event crossing Python↔C++, not just internal C++ speed. This is the bottleneck nobody checks, and checking it is a tell that you understand the stack.
- **Deliverable:** run a trivial market-making or VWAP-reversion strategy through it; quantify fill realism vs. a naive "fills at mid" backtest
- Backtester writes its outputs to disk (`trades.csv`, `metrics.json`, `snapshots.parquet`) - this is the handoff point to Phase 5, and the only handoff point. No other component reads or writes these files.

### Phase 4 - Trivial ML Signal (kept deliberately small)
- One-feature OFI predictor of next 1-second mid-price move - nothing more elaborate
- Run through both the naive vectorized backtest and the realistic engine
- **Deliverable / thesis statement:** "X% of this signal's apparent alpha was an artifact of unrealistic fill assumptions, not real edge." This sentence is your headline result - it's a stronger story than any model accuracy number.

### Phase 5 - Post-Trade Visualization & Reporting
This phase exists to **present** the findings from Phases 2-4 clearly - in interviews (screen-share friendly) and in the README. It is explicitly **not** a production monitoring system, and it is **not** where the engineering effort of this project lives. Validation (Phase 2) and latency profiling (Phase 3) remain the headline deliverables; this is packaging.

Hard boundaries on this layer, by design:
- **Strictly a decoupled, static-file reader.** It reads `trades.csv`, `metrics.json`, and `snapshots.parquet` from disk - the same files Phase 3 already writes. Nothing more.
- **Never connects to the live C++ engine or the event loop, under any circumstance.** UI rendering is completely separated from the performance-critical path. This is intentional: a backtester that produces analysis files and a viewer that reads them afterward is a real production pattern; a UI wired into a live engine is a latency liability and architecturally the wrong call for this kind of tool. Decoupling here is the point being demonstrated.
- **No websockets, no real-time updates, no complex state management.** Streamlit or Plotly, rendering static charts from static files.
- **Capped at ~2 hours of dev time.** If it's taking longer, you're over-building it.
- **4-5 charts, no more:** Equity Curve, Latency Histogram, Fill-Rate by Queue Position, Signal Decay curve (and optionally a simple summary table).

Also in this phase:
- Profile the C++ core, document before/after latency from any optimization
- Static JSON output: latency percentiles (p50/p95/p99), fill rates
- A handful of plotly-rendered PNGs embedded in the README (so the README doesn't depend on anyone running the Streamlit app)
- **`FAILURE_MODES.md` at repo root** - 2 focused sections: why queue position matters, why delayed data kills momentum-style signals. This is the artifact that reads as senior-engineer thinking, not student-project thinking.

---

## Resume / Recruiter Framing
ATS screens and generic recruiters will not read your phase breakdown - they scan for engineering keywords and numbers. Write bullets around throughput and latency, not finance:

> "Architected a C++ limit order book processing real Nasdaq ITCH MBO data with O(1) cancel/modify via intrusive linked-list price levels; validated against exchange reference snapshots with 0 drift across 1,000 sampled timestamps."

> "Built an event-driven backtesting engine modeling queue-position fills and execution latency; profiled the Python↔C++ boundary to <20µs/event via pybind11."

> "Demonstrated that a naive vectorized backtest overstated a trading signal's Sharpe ratio by [X]% relative to a fill-realistic simulation - quantifying the cost of common backtesting errors."

> "Designed a decoupled post-trade analysis layer reading static output files, intentionally isolated from the performance-critical engine - avoiding the latency/architecture pitfalls of coupling UI to a real-time system."

These get past ATS keyword scans *and* hold up under a technical lead's questions, which the finance framing alone does not.

## Why This Stands Out
- **Validation-first** - a 0-drift correctness proof against real exchange data is rarer and more credible than any model or strategy result
- **Real exchange-native data** (XNAS.ITCH via Databento), not synthetic or crypto proxies
- **C++/Python hybrid** - the real production pattern at quant firms, with the pybind11 boundary explicitly profiled, not assumed fast
- **Explicit data-structure tradeoff reasoning** (O(1) vs O(N) cancel) - the kind of detail a senior dev checks for
- **Quantified realism gap** - naive vs. realistic backtest is a concrete, defensible result, not a Sharpe ratio screenshot
- **Decoupled architecture as a deliberate design choice** - separating the static post-trade viewer from the live engine demonstrates an understanding of production architecture patterns (UI/analysis layers should never sit on a performance-critical path), not just an ability to build a chart
- **`FAILURE_MODES.md` as a lead artifact** - signals seniority of thought independent of the code

---

## Timeline
No fixed deadline, but scoped to be genuinely finishable: one symbol, one day of data, a trivial signal, a capped 2-hour visualization layer. Each phase gates the next - do not start Phase 3 until Phase 2's validation script shows 0 drift, do not start Phase 4 until Phase 3's latency profiling is documented, and do not start Phase 5 until Phase 3 is writing clean output files.

## Open Items / Next Steps
- [ ] Databento account + API key, confirm one symbol/day pull fits free-tier credits
- [x] Repo skeleton: directory structure, CMake + pybind11 scaffold, Databento client stub
- [ ] Decide queue-position data structure (`std::list` + iterators vs. intrusive linked list) before writing book code, not after
- [ ] Stub out `FAILURE_MODES.md` early - fill it in as you discover failure modes, don't write it retroactively
- [ ] Define the exact schema for `trades.csv` / `metrics.json` / `snapshots.parquet` before writing the Streamlit viewer, so Phase 3 and Phase 5 don't drift out of sync
