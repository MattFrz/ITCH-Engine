# Order book data structure trade-offs

The decision the README promises to justify, in full.

## The workload

On a real Nasdaq ITCH day, the message mix is dominated by **cancels** -
typically well over half of all messages, and the large majority of them
cancel orders sitting *in the middle* of a price level's queue, not at the
front. Any book design must therefore be judged primarily on the cost of
"delete an arbitrary resting order given its id", with adds second and
executions (front-of-queue removals) a distant third.

## Per-level queue: `std::vector` vs `std::list` + stored iterators

| Operation | `std::vector<Order>` | `std::list<Order>` + iterator index |
|---|---|---|
| add (back) | amortized O(1) | O(1) (one allocation) |
| cancel in middle | **O(N)** shift, invalidates *all* later indices | **O(1)** erase, other iterators stable |
| execute front | O(N) (shift) or O(1) with a head cursor | O(1) |
| queue position scan | O(position), cache-friendly | O(position), cache-hostile |

The vector's O(N) cancel is not the whole problem - the killer is that
erasing invalidates every stored index after it, so an id->position index
must be rebuilt or patched on every cancel. The list's iterators are stable
under everyone else's erases, which is exactly the property the order index
needs.

Chosen design (see `cpp/include/itch_engine/`):

- `PriceLevel` holds `std::list<RestingOrder>` plus a cached aggregate qty,
  so level-quantity queries don't walk the queue.
- `OrderBook` keeps `std::unordered_map<OrderId, {side, price, list::iterator}>`.
  Cancel/modify/execute of any order: one hash lookup + one O(1) list
  operation. This is the "O(1) cancel via stored iterators" claim, and the
  unit tests assert the queue-position semantics it implies (partial cancel
  keeps priority, size-up loses it, etc.).

An intrusive doubly-linked list (nodes owned by a pool, no per-node
allocation) is the production refinement of the same idea - same asymptotics,
better constants. That refinement now exists, as a second book rather than a
replacement: see "The second book" below.

## Price levels: `std::map` vs alternatives

`std::map<Price, PriceLevel>` (bids `std::greater`, asks `std::less`):

- best bid/ask = `begin()` - O(1);
- ordered iteration gives top-N depth walks for free;
- insert/erase/find are O(log L) where L = live levels (hundreds), so
  ~8-10 comparisons - not the bottleneck (the profile confirms the boundary
  and hashing dominate).

A hash map of levels alone is *wrong* for this use: it cannot answer "best
price" or "next level down" without scanning. A flat array indexed by tick
offset from a reference price is the classic low-latency alternative - O(1)
everything - and is what the second book uses, with a hierarchical bitmap to
answer "best" and "next level down" that the array alone cannot. See below.

## Fixed-point prices

Prices are `int64_t` in 1e-9 USD units end to end (Databento's MBO
convention). Floating-point prices are how books silently drift: two prices
that should be the same level compare unequal by 1 ulp and split the level.
Floats appear only in the analysis layer, after the book.

## Why this matters

Naive-vector-book replays of a full ITCH day are quadratic in the busy
levels' queue lengths and measurably slow; worse, most implementations
"fix" it by dropping per-order identity (aggregating to price levels),
which destroys the queue-position information that Phase 3's fill model -
and the entire point of this project - depends on.

---

## The second book

Everything above is about `itch::OrderBook`, the research book, and it still
holds: the design is right for a book that has to be obviously correct and
fast enough, and it is the validated reference the rest of the project trusts.

It is also allocation-heavy by construction. Measured: **1.0638 allocations per
event** - a `std::list` node per resting order, an `unordered_map` node per
index entry, a red-black tree node per price level. That is fine at 8.5M
events/s through a backtest and unacceptable on a feed handler, where the
allocator is an unbounded pause waiting to happen.

So `itch::book::LowLatencyOrderBook` sits beside it, applying the refinements
this document listed as future work, with the trade-offs made the other way:

| | research book | low-latency book |
|---|---|---|
| queue node | `std::list<RestingOrder>`, one allocation each | intrusive list over a preallocated pool, freelist pop/push |
| price levels | `std::map`, O(log L), pointer chase per comparison | tick-indexed flat array + 3-level occupancy bitmap, O(1) |
| best price | `begin()`, O(1) but a pointer chase | 3 dependent loads and 3 bit-scans |
| next level down | tree iteration | one bitmap scan |
| order index | `std::unordered_map` | open addressing, SoA keys, backward-shift delete, no rehash |
| arbitrary prices | any `int64` works | tick grid + a sorted overflow list for off-grid and out-of-window prices |
| allocations/event | 1.0638 | 0 |
| mean ns/event | 118.2 | 17.0 |
| p99 ns/event | 260 | 70 |

The cost is that it is a great deal less obvious, which is why it is a second
implementation and not an edit: `cpp/tests/test_differential.cpp` and
`validation/validate_low_latency.py` hold it to the research book's exact
behaviour, including FIFO order at every level and the anomaly counters, on
generated streams and on real sessions.

Two details worth carrying over from the argument above.

**The tick array cannot address every price, and that matters.** The
validation session contains adds at $0.0001 and at $199,999.00, and seven that
are not on the penny grid. A flat array over a bounded window addresses none of
those. Rejecting them would be a book that is fast and wrong, so they go into a
small sorted overflow list, merged with the grid in price order for every
query, behind a single `empty()` branch on the fast path.

**Capacity became a latency parameter.** Because the low-latency book
preallocates everything, sizing it generously is not free: it spreads the
working set over memory that is never touched. Right-sizing the pools to the
session was worth 60% in the mean and nearly 4x at p99. Full numbers in
[low_latency_architecture.md](low_latency_architecture.md).
