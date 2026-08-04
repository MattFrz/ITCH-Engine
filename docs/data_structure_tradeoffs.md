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
better constants. Documented as future work in `profiling/profile_cpp_core.md`;
`std::list` keeps the code standard and the argument identical.

## Price levels: `std::map` vs alternatives

`std::map<Price, PriceLevel>` (bids `std::greater`, asks `std::less`):

- best bid/ask = `begin()` - O(1);
- ordered iteration gives top-N depth walks for free;
- insert/erase/find are O(log L) where L = live levels (hundreds), so
  ~8-10 comparisons - not the bottleneck (the profile confirms the boundary
  and hashing dominate).

A flat array indexed by tick offset from a reference price is the classic
low-latency alternative (O(1) everything near the touch) and is listed as a
future optimization. A hash map of levels alone is *wrong* for this use: it
cannot answer "best price" or "next level down" without scanning.

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
