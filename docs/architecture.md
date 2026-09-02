# Architecture (historical / research mode)

This document describes the historical path: parquet in, backtest and viewer
out. It is unchanged.

There is a second execution path - network to book, no Python in it - described
in [low_latency_architecture.md](low_latency_architecture.md). The two share
one internal event representation (`MarketEvent`) and are held to identical
order-book semantics by differential tests. Nothing below changed to make room
for it.


```
Databento XNAS.ITCH MBO (or the schema-identical synthetic generator)
        │  python/itch_engine/ingest/databento_client.py
        ▼
data/raw/symbol=X/date=Y/mbo.parquet          (raw pull, saved immediately)
        │  python/itch_engine/ingest/normalize.py
        ▼
data/processed/symbol=X/date=Y/events.parquet (internal schema, ts-sorted)
        │
        ├────────────► validation/validate_book.py
        │              C++ book vs independent Python reference book,
        │              1,000 random checkpoints, 0 drift required
        │
        ▼
python/itch_engine/backtest/event_loop.py     (pure event loop, never vectorized)
   ├─ exchange book   ── C++ OrderBook (ground truth)
   ├─ strategy book   ── C++ OrderBook, fed data_latency late
   ├─ latency_model   ── data path + order path + jitter
   ├─ fill_model      ── queue-position-aware phantom fills
   └─ strategy        ── signals/ofi_signal.py (the deliberately-dumb client)
        │
        ▼
output/trades.csv  output/metrics.json  output/snapshots.parquet
        │                (the ONLY handoff; nothing else crosses this line)
        ▼
viewer/app.py (Streamlit) + viewer/charts/*  ── static-file reader, never
                                                imports the engine
```

## The C++/Python boundary

`cpp/` builds `itch_engine_cpp` (pybind11). Two entry styles, both profiled
(`profiling/profile_pybind_boundary.py`):

- `apply_event(...)` - one crossing per event, ~3.8 µs. Paid by the event
  loop, which needs per-event control flow in Python. Target <20 µs: met.
- `apply_batch(arrays)` - one crossing per day, ~0.6 µs/event amortized
  (~1.68M events/s on the real 14.3M-event day). Used by replay-only paths
  (quote series, validation setup, profiling baseline).

The boundary is ~84% of the scalar per-event cost, which is exactly why it
is measured instead of assumed.

## Two books, one latency model

The strategy is never shown the exchange book. It sees a second C++ book
that only receives events older than `data_latency_ns`, so stale-data
behavior is structural, not simulated by adjusting timestamps after the
fact. Strategy orders travel the other way: decided at sample time, they
reach the exchange `order_latency_ns` (+jitter) later and are evaluated
against the *exchange* book at arrival.

## Phantom orders

Simulated orders never mutate the replayed book (the reconstruction stays
exactly the exchange's history). The fill model tracks, per resting phantom
order: queue depth at join, feed executions at its level (advance the queue,
then fill us), and feed cancels at its level (expected-position decrement).
Marketable orders walk real depth. Conservative simplifications are
documented inline (modifies don't advance phantom queues; phantom fills
don't consume book liquidity - both bias *against* the strategy, which is
the correct direction for a tool whose job is debunking optimistic fills).

## Ordering and determinism

- Events are strictly ts-sorted once, at normalize time.
- The event loop interleaves feed events, strategy samples, and order
  arrivals in timestamp order.
- All randomness (synthetic day, latency jitter, validation checkpoints)
  is seeded; every number in the README regenerates identically.

## The second execution path

The low-latency path is deliberately not wired into anything above. It reads
packets, not parquet, and produces the same `MarketEvent` the historical path
produces:

```
                                 MarketEvent
                                      ^
        events.parquet ---------------|            (this document)
                                      |
  UDP multicast -> MoldUDP64 -> ITCH --+            (low_latency_architecture.md)
                                      |
                                      v
                    itch::OrderBook  or  itch::book::LowLatencyOrderBook
```

`itch::OrderBook` - the book everything in this document uses - gained one
additive method, `apply(const MarketEvent&)`, so it can be driven from either
side. Its existing `apply(const Event&)` path, its semantics and its
performance are untouched, which the existing validation confirms on every run.

The bridge between the two, for development and benchmarking without a live
feed:

```
data/processed/.../events.parquet
        |  scripts/export_events.py
        v
data/capture/SYMBOL_DATE.evbin        (MarketEvent records, verbatim)
        |  build/cpp/itch_make_capture
        v
data/capture/SYMBOL_DATE.itchcap      (real ITCH messages in real MoldUDP64
        |                              datagrams)
        +--> build/cpp/itch_replay          decode from the file
        +--> build/cpp/itch_replay --publish-group   put it on real multicast,
                                                     received by itch_live
```
