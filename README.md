# ITCH-Engine

[![CI](https://github.com/MattFrz/ITCH-Engine/actions/workflows/ci.yml/badge.svg)](https://github.com/MattFrz/ITCH-Engine/actions/workflows/ci.yml)
[![Live demo](https://static.streamlit.io/badges/streamlit_badge_black_white.svg)](https://itch-engine-t4ngmjgaseufnqn5foske3.streamlit.app/)

### C++ Market-Data Engine: Order Book Reconstruction, Latency-Realistic Backtesting, and a Network-to-Book Low-Latency Path (Nasdaq ITCH/MBO)

**[Live demo: the post-trade viewer](https://itch-engine-t4ngmjgaseufnqn5foske3.streamlit.app/)** - equity
curve, mark-out analysis, queue-depth breakdown, and a trade blotter with
book-context drilldown. The deployed copy runs on the bundled synthetic day
(labelled as such in its header); the numbers in this README come from a
real 14.27M-message Nasdaq session.

## Summary

ITCH is the name of Nasdaq's market data protocol, the format the exchange uses to broadcast every order-level event (adds, cancels, modifies, executions) to the world. It is not an acronym: it is the playful companion to OUCH, Nasdaq's order-entry protocol, and neither has an official expansion. This project consumes the TotalView-ITCH feed (via Databento's XNAS.ITCH dataset) and rebuilds the full limit order book from it, so the name reads exactly as intended: an engine that processes Nasdaq ITCH data.

**What data does this use?**
The engine is built for Nasdaq XNAS.ITCH MBO (market-by-order) data pulled
from Databento: every order add, cancel, modify and execution that hit the
exchange on one day for one symbol, typically 10-15 million messages for a
liquid large-cap.

**The numbers in this README are from a real session**: AAPL, 2026-07-30,
14,270,119 normalized MBO messages.

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

**There are two execution paths, and they are different products.**
*Historical mode* is the research pipeline: parquet in, book reconstruction,
event-driven backtest, viewer out, Python orchestrating a C++ book. *Low-latency
mode* starts at a network interface instead of a file and contains no Python at
all: UDP multicast -> MoldUDP64 -> ITCH binary parser -> a preallocated,
zero-allocation order book -> a strategy interface. They share one internal
event representation and are held to identical book semantics by differential
tests. See [Low-Latency Mode](#low-latency-mode-network-to-book) below;
neither replaces the other.

**How do I run it?**
See "Build & Run" further down for the exact commands. Short version:
build the C++ engine with CMake, run the validation script, run the
backtest script, then `streamlit run viewer/app.py` to see the dashboard.
Everything after the C++ build is plain Python.

**What does this actually prove?**
Two things, independently:

1. The order book reconstruction is correct against the exchange's own
   book - not merely self-consistent. It is checked two ways. A second,
   deliberately simple Python book rebuilds the same state from the same
   events and agrees at 1,000 random points across the day, zero mismatches.
   And the reconstruction is compared against **Databento's `mbp-10`**, the
   aggregated book the vendor builds independently from the same feed, at
   **every one of 3,774,380 points where the venue book changed, ten levels
   per side**: 226,462,800 field comparisons, zero mismatches. The second
   check is the one that matters, and building it immediately found a real
   bug that the first could never see - see
   [Validated against the venue's own book](#validated-against-the-venues-own-book-and-what-that-caught).
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
- **Core engine:** C++17 (order book data structures, matching/fill logic) - two book implementations, a readable validated one for research and a preallocated one for the network path, held to identical semantics by differential tests
- **Market-data transport:** MoldUDP64 over IPv4 UDP multicast, with sequence-gap and duplicate handling; Nasdaq TotalView-ITCH 5.0 binary parser
- **Network backends:** Linux kernel sockets (`recvmmsg`, `SO_BUSY_POLL`, `SO_RXQ_OVFL`, hardware timestamps), with an optional DPDK poll-mode backend behind a build flag
- **Bindings:** pybind11 (C++ ↔ Python)
- **Orchestration / analysis:** Python (event loop, signal logic, backtest reporting)
- **Data storage:** Parquet (columnar, point-in-time correct, partitioned by symbol/day)
- **Data source:** Databento - `XNAS.ITCH` (Nasdaq TotalView-ITCH) MBO (Market-By-Order) feed, free-tier credits
- **Profiling:** perf / gprof (or equivalent) for hot-path optimization
- **Streamlit (or Plotly) for static post-trade analysis visualization** - reads only the files the backtester writes to disk; never touches the live engine

---

## Architecture Overview

This is the historical/research path. The low-latency path is
[further down](#low-latency-mode-network-to-book) and is deliberately separate:
it shares the internal event representation and the book semantics, and nothing
else.

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
     at 1,000 random timestamps → assert 0 drift, AND against the venue's own
     aggregated book (Databento mbp-10) at every book change, 10 levels a side
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
ctest --test-dir build --output-on-failure
python validation/validate_book.py
python validation/validate_low_latency.py
python scripts/run_backtest.py          # --rth-only restricts to 09:30-16:00 ET
python profiling/profile_pybind_boundary.py
streamlit run viewer/app.py --server.address localhost
```

Validating against the venue's own book needs a `DATABENTO_API_KEY` and spends
credits (~$0.52 for one session; the download is cached, so re-runs are free).
It is the only check here that cannot run on the synthetic day, because a
synthetic day has no exchange truth to compare against:

```bash
python scripts/pull_reference_snapshots.py --symbol AAPL --day 2026-07-30
```

```bash
python validation/validate_against_exchange.py --symbol AAPL --day 2026-07-30
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

## Results (real XNAS.ITCH MBO: AAPL, 2026-07-30, 14.27M events)

Every number in this section comes from one real Nasdaq trading session
pulled from Databento: 14,771,847 raw MBO records, 14,270,119 after
normalization, spanning 03:04 to 19:59 ET (so pre-market and after-hours
are included, not just the regular session).

(The normalized count was 14,469,900 until validation against the venue's own
book showed the feed reports every execution twice - once as a fill and once
as a cancel for the same shares. Dropping the 199,781 duplicates is the
difference. See below.)

Without a `DATABENTO_API_KEY` the same commands run end to end against a
seeded synthetic day instead, which is what the bundled `demo/` dataset and
the deployed viewer use. Real market data is not redistributed in this repo.

**Book validation** (`validation/results/validation_run.txt`):

```
symbol=AAPL day=2026-07-30
events replayed: 14,270,119
checkpoints: 1,000 random timestamps
assertions: 8,000
mismatches: 0
RESULT: PASS - 0 drift across all sampled timestamps
```

At every checkpoint the C++ book is compared against an independently
written pure-Python reference book: top-5 levels per side (price, aggregate
qty, order count), best quotes, open order count, and exact FIFO
`queue_ahead` for sampled live orders.

Read this for what it is: it proves the two implementations agree, which
rules out essentially every indexing, priority and bookkeeping bug. It does
not, on its own, prove either one reads the ITCH semantics the way Nasdaq
does, because both were written from the same interpretation. That is what
the next section is for - and it is not hypothetical, because building it
found a real bug this check had been agreeing with all along.

The session contains exactly one `R` (Clear) message, which wipes the book and
is applied by both. After the feed-semantics fix described below there are
**zero** `unknown_order_events` across the entire session: every message in
the feed refers to an order the reconstruction knows about.

### Validated against the venue's own book (and what that caught)

Everything above is *agreement between implementations*. The C++ book and the
Python reference book match to zero drift, and the low-latency book matches
both - but all three were written from the same reading of the MBO semantics,
so a shared misinterpretation would agree perfectly across every one of them
and still be wrong. The README used to say exactly that, and list this as the
single most valuable open item.

It is no longer open. `validation/validate_against_exchange.py` compares the
reconstruction against **Databento's `mbp-10`** - the aggregated book Databento
builds from the same XNAS.ITCH feed, independently of this repository - at
every point in the session where the venue book changed:

```
symbol=AAPL day=2026-07-30 schema=mbp-10
events replayed: 14,270,119
checkpoints: 3,774,380 (every timestamp at which the venue book changed)
levels compared: 10 per side, 3 fields each
field comparisons: 226,462,800

checkpoints fully matching all 10 levels: 3,774,380 / 3,774,380 (100.0000%)
field mismatches: 0 / 226,462,800 (0.000000%)

RESULT: PASS - the reconstruction matches the venue's own aggregated book
exactly, at every level and every checkpoint
```

Not sampled: **every** checkpoint, all ten levels per side, and for each level
the price, the aggregate size **and the order count** - `mbp-10` carries
`bid_ct_NN`/`ask_ct_NN`, which this engine tracks too, so per-level order
counts are checked rather than assumed.

#### What it caught

The first run scored **90.47%**, not 100%, and the mismatch rate was flat
across the day rather than concentrated at the open - so not a warm-up
artefact of starting from an empty book. Tracing one divergence to a single
order was decisive:

```
ts_event              order_id  action side        price  size
1785412629296848278   39640033       A    B 336400000000    80
1785412629296865645   39640033       F    B 336400000000    20   <- fill
1785412629296865645   39640033       C    B 336400000000    20   <- SAME 20 shares
```

Databento reports an execution **twice**: once as a fill (`F`) and once as a
cancel (`C`), same order, same timestamp, same size. The engine applied both,
so every partial execution removed the quantity twice. Across the session:
**199,781 fills, 100% of them paired with an identical-size cancel.**

Two implementations, written independently, both applied both records - and
therefore agreed with each other perfectly while both were wrong. That is
precisely the failure mode agreement-based validation cannot see, and the
reason this check was worth building.

The fix is in `normalize.drop_execution_paired_cancels()`, and the fill is
kept rather than the cancel: their effect on the book is identical, but the
backtester's fill model advances queue position differently for a feed
execution than for a feed cancel, so collapsing one into the other would have
silently changed the fill simulation.

Consequences, all of them corrections to things this README previously stated:

| | before | after |
|---|---|---|
| normalized events | 14,469,900 | **14,270,119** |
| `unknown_order_events` | 183,399 | **0** |
| exchange-book match | not measured | **100.0000%** |

The `unknown_order_events` line is the one worth dwelling on. This README used
to explain those 183,399 events as *"messages that reference orders resting
from before the capture window"*. That explanation was wrong. They were the
duplicate cancels arriving for orders the duplicate had already removed. There
are now zero unknown references in the entire session - every message in the
feed refers to an order the reconstruction knows about.

#### Aligning the two streams, which is harder than it looks

Worth recording, because three plausible alignments all produced convincing
false mismatches before the right one:

| join | result |
|---|---|
| on `ts_event` | 99.79% - an mbp-10 row can carry a *different* timestamp from the MBO record that produced it |
| on `sequence`, last row per sequence | 97.68% |
| on `sequence`, first row per sequence | 66.71% |
| on `sequence`, **respecting the row's `action`** | **100.00%** |

A single sequence emits several mbp-10 rows and they describe different
instants. The venue's own `action` label says which is which - and note that
mbp-10 contains no `F` at all, because in an aggregated book an execution *is*
a removal:

* `T` - a trade report. Shows the book **before** that sequence's change (the
  trade record precedes the removal it reports). Compare against
  `sequence < N`.
* `A` / `C` - an add or a removal. Shows the book **after**. Compare against
  `sequence <= N`.

Every one of the wrong alignments looks like a book bug and is not. Aligning
on `ts_event` in particular scores 99.79%, which is close enough to correct
that it would be easy to ship and call the residual "a known limitation".

#### Reproducing it

The MBO day must be pulled with the `sequence` column (the current
`databento_client` keeps it; older cached pulls predate it and the script says
so). Reference snapshots cost **$0.5174** for this session and the raw
download is cached, so re-runs and re-conversions spend nothing.

```bash
python scripts/pull_reference_snapshots.py --symbol AAPL --day 2026-07-30
```

```bash
python validation/validate_against_exchange.py --symbol AAPL --day 2026-07-30
```

This is the one check in the repository that cannot run without a
`DATABENTO_API_KEY`: a synthetic day has no exchange truth to compare against,
and generating reference snapshots from our own reconstruction would be
circular. The script refuses rather than pretending. CI therefore does not run
it, and the committed result lives in
[validation/results/exchange_validation.txt](validation/results/exchange_validation.txt).

**Boundary profiling** (`profiling/results/latency_percentiles.json`):
`apply_event` costs **0.88 µs/event** mean (p50 0.9, p95 1.1, p99 1.3),
comfortably inside the <20 µs target. Batch replay runs at **0.13 µs/event**
(~7.8M events/s through the C++ book) because it crosses the pybind11
boundary once instead of once per event. About **85% of the scalar cost is
the crossing itself**, not book work, which is the whole argument for the
batch path on replay-only workloads.

**Headline result:** the naive vectorized backtest of the OFI signal shows
**+$378** on the day. The same signal, on the same 100 ms clock, run through
FIFO queue position, a 2 ms / 3 ms latency model and real exchange fees,
shows **-$1,598**. The naive backtest did not merely overstate the edge, it
got the *sign* wrong: the apparent alpha was entirely a fill-assumption
artifact.

The gap decomposes, which is the part worth reading:

| | |
|---|---|
| naive backtest | **+$377.50** |
| realistic, before fees | -$1,553.94 |
| exchange fees paid (net of maker rebates) | -$44.23 |
| **realistic, all-in** | **-$1,598.17** |

Fees are only $44 of the $1,976 swing. The overwhelming majority is queue
position, latency and the adverse selection they produce: the realistic run
loses roughly 1.8 cents per share across 2,776 fills and 86,762 shares
against real HFT flow, with no single episode dominating.

Passive fill rate was **37.3%** against the naive assumption of 100%, across
1,704 passive orders. Position discipline is structural rather than a limit
check, and the output confirms it: maximum absolute position held all day
was exactly the intended 100 shares.

(These figures moved when the duplicate-execution bug described above was
fixed - the previous README reported +$369 / -$2,679 on a book that was
double-counting every partial fill. The conclusion is unchanged and the
mechanism is the same; the magnitude was wrong.)

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

## Low-Latency Mode (network to book)

The historical mode above is a research pipeline: parquet in, backtest and
viewer out, Python orchestrating a C++ book. It is the right shape for what it
does and it is unchanged.

It is the wrong shape for a feed handler. A production market-data path starts
at a network interface, not a file, and it cannot afford an allocator call per
event or a Python frame anywhere near it. So there is a second execution path
that starts at the wire and ends at a book, shares the internal event
representation with the first one, and contains no Python at all:

```
  Market-data packets (UDP multicast, or a recorded capture)
          |  feed/network_backend.hpp   LinuxUdpBackend | DpdkBackend
          v  PacketView: a pointer INTO the receive buffer - no copy
  MoldUDP64 decoder      sequence tracking, gap and duplicate detection
          |
          v
  ITCH 5.0 binary parser  fixed-width big-endian decode, no allocation
          |
          v
  MarketEvent (40 bytes)  the representation BOTH feeds produce
          |
          v
  LowLatencyOrderBook     order pools, tick-indexed levels, open addressing
          |
          v
  Strategy (observe only) -> Risk -> OrderGateway (simulated; nothing trades)
```

Both paths meet at `MarketEvent`, and both books are held to identical
semantics:

```
   events.parquet ------\
                         >---> MarketEvent ---> itch::OrderBook            (research)
   UDP -> MoldUDP64 ----/                  \--> LowLatencyOrderBook        (hot path)
          -> ITCH parser                          |
                                                  |
                              differential tests fail on ANY divergence
```

Full design and measurement methodology:
**[docs/low_latency_architecture.md](docs/low_latency_architecture.md)**.

### Developing without a live feed

Nasdaq TotalView-ITCH is licensed data on a private connection. Rather than
mock it, the historical days this repository already has are **re-encoded as
real ITCH messages inside real MoldUDP64 datagrams**, so the whole pipeline
runs against them:

```
data/processed/.../events.parquet
        |  scripts/export_events.py
        v
data/capture/AAPL_2026-07-01.evbin
        |  build/cpp/itch_make_capture
        v
data/capture/AAPL_2026-07-01.itchcap
        |
        +--> itch_replay                    read the capture as a file
        +--> itch_replay --publish-group    put it on real UDP multicast,
                                            received by itch_live
```

The only component a capture replaces is the socket. Everything above it -
sequence handling, binary decode, the book, the strategy interface - is the
code that will run against a live feed.

### Results: network to book

Real Nasdaq session, AAPL 2026-07-30, 14,270,119 messages in 309,944 MoldUDP64
datagrams (46.0 messages per datagram). Windows 11, MSVC 19.44, x64, invariant
TSC at 4.300 GHz, thread pinned to CPU 2, Release build, machine otherwise
idle. Best of 5 clean passes; the distribution comes from a separate sampled
pass so the timer is never inside the loop the mean is measured on. Three
independent runs agreed to within 0.6% end to end (23.29 / 23.38 / 23.43 ns).

| stage | ns/message | throughput |
|---|---|---|
| UDP buffer -> MoldUDP64 | 2.62 | 381.3M msgs/s |
| MoldUDP64 -> ITCH decode -> MarketEvent | 4.67 | 214.1M msgs/s |
| MarketEvent -> LowLatencyOrderBook | 15.97 | 62.6M msgs/s |
| **complete pipeline** | **23.27** | **43.0M msgs/s** |
| the same pipeline into the research book | 115.30 | 8.2M msgs/s |

**Allocations per message: 0.** Not asserted - measured, by replacing the
global `operator new` in the benchmark, so an allocation three layers down
inside something innocent-looking would still be caught.

| distribution (ns) | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|
| ITCH parse (per message) | 30 | 30 | 30 | 60 | 190 | 30,051 |
| book apply (per message) | 40 | 50 | 70 | 150 | 380 | 96,383 |
| parse + book (per message) | 80 | 90 | 110 | 200 | 490 | 96,653 |
| whole datagram (46 messages) | 1,060 | 1,300 | 1,620 | 4,300 | 12,390 | 100,643 |

The maxima are microseconds to milliseconds - thousands of times p99.99 - and
they are the operating system, not the code: this is an untuned general-purpose
desktop. [docs/linux_tuning.md](docs/linux_tuning.md) covers what removes them.

**Read these for what they are.** They are host processing numbers: packet
already in memory, to book updated. They are **not** network-to-book latency,
which additionally includes NIC, driver and kernel time and can only be
measured against a real feed with hardware timestamps. There are no
exchange-latency claims anywhere in this repository, and no DPDK numbers at
all, because no DPDK hardware was available.

### Results: the book on its own

4,000,000 generated events through both books, same stream, same methodology,
same idle machine:

| | research OrderBook | LowLatencyOrderBook |
|---|---|---|
| mean | 120.1 ns/event | **17.0 ns/event** |
| throughput | 8.33M events/s | **58.8M events/s** |
| p50 / p99 / p99.9 | 120 / 260 / 450 ns | **40 / 70 / 140 ns** |
| allocations per event | 1.0638 | **0** |
| resident memory | 1.4 MiB | 8.2 MiB |

7.1x in the mean, 3.7x at p99.

### Correctness: the new book has to be the old book

Speed means nothing if the book is wrong, and the research book is the
reference the whole project already trusts - it agrees to zero drift with an
independently written Python book across this same session. So the low-latency
book is held to it, everywhere:

**Packet pipeline vs the historical path**, real session, at 1,000 checkpoints
spread through the day (`itch_replay --verify`):

```
historical events   : 14,270,119 (normalized parquet -> research book)
checkpoints         : 1000, spread evenly across the session
compared            : millions of level snapshots and resting orders in
                      exact FIFO order
unknown references  : 0 (both books)
clears applied      : 1 (both books)
RESULT              : PASS - 0 divergence between the packet pipeline and
                      the historical path
```

That compares every live price level on both sides - price, aggregate size,
order count - and then the exact sequence of order ids resting at each one.
Two books can agree on aggregate sizes while disagreeing about queue order;
comparing the id sequences is what rules that out.

**Research book vs low-latency book**, both driven from the same parquet
(`validation/validate_low_latency.py`): 0 mismatches on the real day and on the
synthetic one.

**Generated streams** (`cpp/tests/test_differential.cpp`): 1.37M events across
six deliberately awkward shapes - cancel-dominated, a single price level, an
8,000-tick book, a tick window narrow enough to force almost everything through
the off-grid path, streams with venue clears - with unknown order references,
duplicate adds, zero quantities, off-grid and out-of-window prices injected
throughout. Any divergence in any observable fails the test.

**Live multicast loopback**: the capture published as real UDP datagrams and
received by `itch_live` through the socket path - 8,768 datagrams, 394,514
messages, 0 gaps, 0 kernel drops, and a final book identical to the file
replay.

### Build and run the low-latency path

Paths below are the single-config (Linux/macOS) form. On Windows, CMake's
Visual Studio generator puts binaries in a config subfolder, so read
`build/cpp/itch_replay` as `build\cpp\Release\itch_replay.exe` throughout.

The low-latency targets need no Python at all - the C++ core, the tests, the
tools and the benchmarks all build and run without pybind11 or an interpreter:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build --config Release
```

```bash
ctest --test-dir build --build-config Release --output-on-failure
```

Build a capture from a historical day (the first command ingests and normalizes
on first run - synthetic unless `DATABENTO_API_KEY` is set):

```bash
python scripts/export_events.py --symbol AAPL --day 2026-07-01
```

```bash
build/cpp/itch_make_capture --input data/capture/AAPL_2026-07-01.evbin --symbol AAPL --output data/capture/AAPL_2026-07-01.itchcap
```

Replay it through the full pipeline, and assert it reproduces the historical
book exactly:

```bash
build/cpp/itch_replay --capture data/capture/AAPL_2026-07-01.itchcap --verify data/capture/AAPL_2026-07-01.evbin --measure --cpu 2
```

Replay at the original packet timing instead of as fast as possible, or at a
multiple of it:

```bash
build/cpp/itch_replay --capture data/capture/AAPL_2026-07-01.itchcap --original-speed
```

```bash
build/cpp/itch_replay --capture data/capture/AAPL_2026-07-01.itchcap --speed 100
```

Benchmark the complete pipeline, or the book on its own:

```bash
build/cpp/bench_pipeline --capture data/capture/AAPL_2026-07-01.itchcap --cpu 2 --repeats 3
```

```bash
build/cpp/bench_book --events 4000000 --cpu 2 --repeats 3
```

### Simulated live feed, end to end over real multicast

Two terminals. The receiver joins a group you control on your own machine; the
sender puts the recorded session on the wire with TTL 0 so nothing leaves the
host:

```bash
build/cpp/itch_live --multicast-group 239.255.42.99 --port 35001 --interface 127.0.0.1 --duration 60 --relaxed-gaps --cpu 4
```

```bash
build/cpp/itch_replay --capture data/capture/AAPL_2026-07-01.itchcap --publish-group 239.255.42.99 --publish-port 35001 --publish-interface 127.0.0.1 --publish-ttl 0 --speed 2000
```

This is the only way to exercise the receive path - socket options, group
joins, batching, kernel drop accounting - without an exchange feed.

### Pointing it at an authorized feed later

`itch_live` has no built-in feed address, port, session or credential, and no
default. When you have an entitlement and a connection:

```bash
build/cpp/itch_live --multicast-group <your group> --port <your port> --interface <your NIC address> --buffer-size 268435456 --busy-poll 50 --hardware-timestamps --cpu 4 --lock-memory --realtime
```

Three things to decide before that command is a good idea:

1. **Gap policy.** The default is strict: an unrecoverable sequence gap stops
   delivery and reports what is missing, rather than skipping ahead and leaving
   a book that looks fine and is not. To do better than stopping, implement
   `feed::GapRecoveryPolicy` against your venue's retransmission service and
   inject it. There is deliberately no built-in Nasdaq recovery client - the
   endpoints and entitlements are contract-specific, and inventing them would
   be worse than useless.
2. **Session epoch.** ITCH timestamps are nanoseconds since midnight. Pass
   `--session-epoch <ns since epoch for that midnight>` to get absolute times
   that line up with the historical path.
3. **Capacity.** Size `BookConfig` from your own symbol - `itch_replay` prints
   the peaks it saw. This is a latency parameter, not just a safety limit; see
   below.

Host tuning that actually moves the tail: [docs/linux_tuning.md](docs/linux_tuning.md).
Optional DPDK backend, and why it is optional: [docs/dpdk.md](docs/dpdk.md).

### Two things measurement changed

Both of these were found by benchmarking and would not have been found by
reading the code, which is the argument for benchmarking everything.

**Capacity turned out to be a latency parameter.** The book preallocates
everything and never grows, so sizing it generously is not free - it spreads
the working set over pages that are never touched and pays TLB and cache misses
on every event. Swept on the real session (on the pre-fix normalization; the
ordering is what matters and holds on the corrected day too):

| configuration | book apply mean | p99 | memory |
|---|---|---|---|
| 2^20 orders / 2^21 index / 2^20 ticks | 25.13 ns | 270 ns | 66 MiB |
| **2^17 orders / 2^18 index / 2^17 ticks** | **15.78 ns** | **70 ns** | **9 MiB** |
| 2^16 orders / 2^17 index / 2^16 ticks | 18.72 ns | 80 ns | 5 MiB |

The middle row is now the default: ~2.4x headroom over that session's peak of
55,682 resting orders.

**An optimization that did not pay for itself was reverted.** Padding the order
node from 24 to 32 bytes, so two fit exactly in a cache line, measured 16.879 /
16.892 / 16.863 ns packed against 16.908 / 17.002 / 17.040 ns padded over three
runs - and cost 12% more memory. The default is the packed node; the comparison
is still buildable (`bench_book_packed`) so it can be re-run on other hardware.

### What the low-latency path deliberately does not do

- **It does not trade.** The strategy interface observes the book. The order
  gateway is a simulator that records what it was asked to do and does nothing
  else, behind a risk layer that is the only thing holding it. There is no
  venue connection and no OUCH implementation.
- **It does not thread.** Feed, parse, book and strategy run on one core. A
  handoff between threads costs a cache-line transfer and a queue round trip to
  overlap work of about the same size; at 43M messages/s against a feed that
  peaks in the low millions, the book is not the constraint. No mutexes, no
  shared mutable state, no queues, until a measurement asks for them.
- **It does not require DPDK, root, or a tuned kernel.** All of those are
  opt-in and every one degrades to a printed note rather than a failure. A run
  that could not get real-time scheduling says so, and its numbers should be
  read accordingly.

---

## Limitations

Written down deliberately. Everything here is a known boundary of the
current build, not something discovered later.

**Validation now covers exchange truth, for one session.** The
reconstruction is checked against Databento's `mbp-10` - the venue-derived
aggregated book - at all 3,774,380 points where that book changed, ten levels
a side, price, size and order count: 226,462,800 comparisons, zero mismatches.
That is a much stronger claim than the implementation-agreement check it
supersedes, and building it found a real bug (see
[Validated against the venue's own book](#validated-against-the-venues-own-book-and-what-that-caught)).

What it still does not cover: **one symbol, one session, one vendor.**
`mbp-10` is Databento's reconstruction, not a Nasdaq-published artefact, so a
shared misreading between this engine and Databento would still agree - though
that is a far narrower risk than two books in this repo agreeing with each
other. And a session with a halt, an LULD pause or a genuine cross would
exercise paths this day does not contain.

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

**Low-latency numbers are host processing, not network-to-book.** Every
latency figure in the Low-Latency Mode section starts from a packet that is
already in memory. Real network-to-book latency additionally includes NIC,
driver and kernel time, and can only be measured against a live feed with
hardware timestamps. There is no such measurement here.

**The DPDK backend is compile-gated and unmeasured.** It is written against the
DPDK 21.11+ API and has never been run: no DPDK hardware was available. There
are no DPDK numbers anywhere in this repository for that reason.

**No live feed was tested.** The receive path is exercised end to end over
loopback multicast against a recorded session, which covers socket options,
group joins and gap handling, but not a real venue's traffic pattern, burst
profile or retransmission service. There is no built-in gap recovery client -
`GapRecoveryPolicy` is the injection point and it is empty by design.

**Nothing trades.** The strategy interface observes; the order gateway is a
simulator; the risk layer is in front of that simulator. There is no OUCH
implementation and no venue connection.

**The low-latency book's capacity is finite and must be sized.** It
preallocates everything and never grows. Exceeding a limit is reported
(`degraded()`, `capacity_rejections`) and the event is dropped rather than
half-applied, but a badly sized book on an unusually busy symbol will drop
events. `itch_replay` prints the peaks it saw so the limits can be set from
data.

## Design Highlights
- **Validation-first** - the order book is cross-checked to 0 drift against an independent reference implementation across 1,000 sampled timestamps before anything is built on top of it
- **Real exchange-native data** - XNAS.ITCH MBO via Databento, with all the real-feed messiness handled explicitly (session-boundary orders, cancel-dominated message mix)
- **C++/Python hybrid with the boundary measured** - the pybind11 crossing is profiled per event, not assumed fast
- **Explicit data-structure reasoning** - O(1) cancel/modify via list-iterator indexing, justified against the actual message mix in [docs/data_structure_tradeoffs.md](docs/data_structure_tradeoffs.md)
- **Quantified realism gap** - the same signal run through naive and realistic fill assumptions, with the difference measured rather than hand-waved
- **Decoupled post-trade layer** - the viewer reads static output files only and never touches the engine, keeping analysis off the performance-critical path
- **[FAILURE_MODES.md](FAILURE_MODES.md)** - the two ways backtests lie, written from this engine's own measured output
- **Two books, one set of semantics** - a readable validated research book and a preallocated zero-allocation one, with differential tests that fail on any divergence in FIFO order, level state or anomaly counters
- **A gap policy that fails safe** - an unrecoverable MoldUDP64 sequence gap stops delivery and reports it rather than silently corrupting the book; the recovery mechanism is injectable rather than invented
- **The packet simulator is not a mock** - historical days are re-encoded as real ITCH messages inside real MoldUDP64 datagrams, and can be published as actual multicast, so the only thing a capture replaces is the socket
- **Optimizations are measured, and reverted when they do not pay** - the order-node padding and the original generous capacity defaults were both removed on the evidence

## Project Status
- [x] Ingestion: real XNAS.ITCH day pulled, cached and normalized (14.27M MBO events), including `R`/Clear; seeded synthetic fallback for keyless runs
- [x] C++ order book + validation: 0 drift across 1,000 random checkpoints (8,000 assertions) on a real 14.27M-message session
- [x] **Validated against the venue's own aggregated book** (Databento `mbp-10`): 226,462,800 field comparisons over 3,774,380 checkpoints, 10 levels a side, 0 mismatches - and it caught a real duplicate-execution bug that implementation-agreement testing could not see
- [x] Event-driven backtester with two-book latency model, queue-aware fills, exchange fees, and structural position discipline
- [x] pybind11 boundary profiled: 0.88 µs/event scalar, 0.13 µs batched (~7.8M events/s), <20 µs target met
- [x] OFI test client: naive backtest reversed the PnL sign vs the realistic engine (+$369 vs -$2,679, same signal and clock)
- [x] Streamlit viewer with mark-out analysis, filterable blotter and drilldown; README chart PNGs (`scripts/export_charts.py`)
- [x] CI on Linux and Windows: C++ tests, Python tests, book validation, end-to-end backtest
- [x] Low-latency path: MoldUDP64 transport, ITCH 5.0 binary parser, UDP multicast receiver, and a zero-allocation order book - 23.1 ns/message end to end, 43.2M messages/s, 0 allocations/message
- [x] Packet-level historical replay: real ITCH inside real MoldUDP64 datagrams, replayable from a file or published as actual multicast, at max speed or the original timing
- [x] Differential correctness: 0 divergence against the research book across the real 14.27M-message session (resting orders compared in exact FIFO order), plus 1.37M generated events over six stream shapes
- [x] Strategy, risk and simulated order gateway interfaces; nothing trades
- [x] Optional Linux runtime tuning (affinity, NUMA, mlockall, SCHED_FIFO) and an optional DPDK backend behind a build flag - neither required to build or run
- [x] CI extended: packet-pipeline equivalence and a performance-regression check with a hard zero-allocation invariant
- [ ] **Open:** extend the exchange-book validation to more sessions, especially one containing a halt or an LULD pause
- [ ] **Open:** the DPDK backend is written but unmeasured - no DPDK hardware was available
- [ ] Possible extensions: direct-addressed order index, `rte_flow` hardware filtering, an OUCH order-entry gateway, a C++ fill model for full-speed strategy sweeps
