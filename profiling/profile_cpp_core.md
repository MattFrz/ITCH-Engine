# Profiling: C++ core and the pybind11 boundary

Measured on one real XNAS.ITCH day (AAPL 2026-07-30, 14,469,899 normalized
MBO events), Windows 11, MSVC /O2, Python 3.9. Reproduce with:

```bash
py -3.9 profiling/profile_pybind_boundary.py --day 2026-07-30
```

(on Linux/macOS: `python profiling/profile_pybind_boundary.py --day 2026-07-30`)

Raw numbers land in `profiling/results/latency_percentiles.json`.

## Results

| Path | mean µs/event | p50 | p95 | p99 |
|---|---|---|---|---|
| `apply_event` (scalar, one crossing per event) | **3.79** | 3.3 | 5.8 | 11.2 |
| `apply_batch` (one crossing for the day) | **0.60** | - | - | - |

- Pure C++ throughput (batch path): **~1.68M events/s** over 14.3M real
  messages.
- Boundary overhead (scalar minus batch): **~3.2 µs/event**. That is, ~84%
  of the scalar per-event cost is the Python↔C++ crossing - argument
  conversion, call dispatch, GIL - not book work.
- Strategy-side query crossings: `best()` ~1.4 µs, `mid_price()` ~0.6 µs.
- Target from the project spec: **<20 µs/event at the boundary - met** with
  ~5x headroom.

## Why measure it this way

The usual mistake is benchmarking the C++ book in isolation and assuming the
bindings are free. The batch-vs-scalar comparison isolates the boundary cost
exactly, because the C++ work in both runs is identical - same events, same
book, same allocations. The difference is purely what pybind11 charges per
crossing.

Consequences for the design:

- The **event-driven backtester** intentionally pays the scalar price
  (~7 µs/event): the whole point of that loop is per-event control flow in
  Python. At ~400k events/day this costs seconds, which is acceptable.
- **Replay-only paths** (validation setup, quote-series building, profiling
  baselines) should use `apply_batch` and pay ~0.8 µs/event.
- Returning structs (`BestQuote`, `LevelView`) copies a few dozen bytes per
  call - noise at these sizes. The expensive part is the call itself, so the
  optimization lever is *fewer, fatter crossings*, not lighter payloads.

## Timer-overhead note

Per-call percentiles are measured on every 16th call with
`time.perf_counter_ns()` around a single call; the mean comes from a
separate clean run with no timer inside the loop. On this machine the timer
itself costs ~0.1-0.2 µs, so p50 (3.3) and the clean mean (3.79) agreeing
closely says the sampling isn't distorting the numbers.

## C++-side profile (batch path)

At 0.60 µs/event the book is dominated by `std::map` level lookup and
`unordered_map` order-index operations, as expected for this design. The
next optimization steps, in order of expected payoff, would be:

1. an open-addressing order index (the default `std::unordered_map` is
   node-based and cache-hostile);
2. a flat array of price levels indexed by tick offset from a reference
   price (books are dense near the touch), falling back to the map for the
   far tail;
3. an object pool for queue nodes to kill the per-add allocation.

For the historical path this analysis still stands: the boundary, not the
book, is where the time goes in the scalar path the backtester uses, so none
of the three is needed to hit this project's targets.

All three have since been done anyway, in a *second* book built for the
network-to-book path rather than as an edit to this one - see
[docs/low_latency_architecture.md](../docs/low_latency_architecture.md). On the
same real session, measured with the same methodology:

| | research book | low-latency book |
|---|---|---|
| mean | 113.84 ns/message | **15.78 ns/message** |
| p99 | 260 ns | **70 ns** |
| allocations/event | 1.0638 | **0** |

The research book is unchanged and remains the reference both are validated
against.
