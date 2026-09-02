# Low-latency architecture

The engine has two execution paths that share one internal event
representation and one set of order-book semantics. Neither replaces the
other.

```
HISTORICAL / RESEARCH MODE (unchanged)

  Databento XNAS.ITCH MBO  or  seeded synthetic day
          |  python/itch_engine/ingest/
          v
  data/processed/.../events.parquet
          |  pybind11
          v
  itch::OrderBook  (std::map + std::list + std::unordered_map)
          |
          v
  event-driven backtester -> trades.csv / metrics.json / snapshots.parquet
          |
          v
  Streamlit viewer


LOW-LATENCY MODE (new; no Python anywhere in it)

  NIC -> DMA -> kernel or DPDK -> packet buffer
          |  feed/network_backend.hpp     LinuxUdpBackend | DpdkBackend
          v  PacketView: a pointer INTO that buffer, no copy
  feed/moldudp64.hpp        sequence tracking, gap + duplicate detection
          |  message view: still a pointer into the same buffer
          v
  feed/itch_parser.hpp      fixed-width big-endian decode, no allocation
          |
          v
  MarketEvent (40 bytes)    market_event.hpp - the shared representation
          |
          v
  book/low_latency_book.hpp order pools, tick-indexed levels, open addressing
          |
          v
  strategy/strategy.hpp     observation only
          |
          v
  risk/risk.hpp  ->  execution/order_gateway.hpp  (simulated; nothing trades)
```

And the bridge that makes the second path developable without a feed:

```
  events.parquet --(scripts/export_events.py)--> .evbin
                                                   |  itch_make_capture
                                                   v
                       real ITCH messages inside real MoldUDP64 datagrams
                                                   |
                    +------------------------------+------------------+
                    |                                                 |
             itch_replay (file)                        itch_replay --publish-group
                    |                                       (real UDP multicast)
                    v                                                 v
     MoldUDP64 -> ITCH -> book                              itch_live -> same stack
```

The only component the capture replaces is the socket. Everything above it is
the code that runs against a live feed.

---

## Why two books

The research book is correct, readable and validated - and validated against
the venue rather than only against itself: besides agreeing to zero drift with
an independently written Python reference, it matches Databento's own
aggregated book (`mbp-10`) at all 3,774,380 points where that book changed on a
real session, ten levels a side, 226,462,800 field comparisons, zero
mismatches. It is also allocation-heavy by construction - a `std::list` node per
resting order, an `unordered_map` node per index entry, a red-black tree node
per price level. Measured: **1.0638 allocations per event**.

That is the right trade for research and the wrong one for a feed handler. So
the research book is untouched and a second implementation sits beside it,
with every difference driven by something the first one does that a hot path
must not:

| | research book | low-latency book |
|---|---|---|
| order storage | `std::list` node per order (one malloc/free each) | index into a preallocated pool, freelist pop/push |
| price levels | `std::map<Price, PriceLevel>`, O(log L), node per level | flat tick-indexed array + 3-level occupancy bitmap, O(1) |
| order index | `std::unordered_map`, node-based, rehashes | open addressing, SoA, backward-shift delete, never rehashes |
| allocations/event | 1.0638 | **0** |
| mean ns/event | 120.1 | **17.0** |
| p99 ns/event | 260 | **70** |

The two are held together by `cpp/tests/test_differential.cpp`, which replays
multi-million-event streams through both and fails on the first divergence in
any observable, and by `validation/validate_low_latency.py`, which does the
same on real parquet days.

---

## MarketEvent: 40 bytes

```
offset size field
------ ---- --------------------------------------------------------
     0    8 timestamp     exchange/message time, ns
     8    8 order_id      ITCH order reference (Replace: the original)
    16    8 new_order_id  Replace only; 0 otherwise
    24    8 price         fixed point, 1e-9 USD
    32    4 quantity      shares (ITCH's share field is 32-bit)
    36    2 stock_locate  ITCH symbol index
    38    1 type
    39    1 side
------ ---- 40 bytes, alignment 8, no padding holes
```

Verified by `static_assert` on size, alignment, every field offset, trivial
copyability and standard layout, so a change to the struct fails the build
rather than the wire format.

Why not 32 bytes: dropping `new_order_id` would fit two events per cache line,
but the ITCH `U` (Order Replace) message carries two order references, and the
parser cannot split it into Delete + Add without knowing the resting order's
side - which only the book knows. Paying 8 bytes keeps the parser stateless,
and the parser is not where the time goes (4.67 ns/message against 15.97 for
the book).

Deliberately absent: strings, containers, pointers, and timing. `EventTiming`
is a separate 32-byte struct with four distinct clocks - exchange, receive,
parsed, book - because merging an exchange timestamp with a local one is how
latency numbers stop meaning anything. Feeds populate it only when timing is
switched on.

---

## MoldUDP64: the gap policy is the design

Sequence numbers exist so that lost messages are detectable. A decoder that
notices a gap and carries on has converted a detectable problem into a silent
one, and every book built on it is quietly wrong for the rest of the session.

So the decoder has an explicit state machine:

```
NORMAL --------- gap detected ---------> GAP_DETECTED
                                              |
                       recovery policy supplied and it succeeded
                                              |-----> RECOVERED --> NORMAL
                                              |
                       no policy, or it failed
                                              v
                                      RECOVERY_REQUIRED
                                      (delivery STOPS; every later packet is
                                       counted as blocked until an operator
                                       calls resync() or auto-resync is on)
```

`itch_live` defaults to strict: it stops and prints what is missing. Replay
defaults to auto-resync, because a capture has no retransmission service to
ask, and the loss is still counted and reported.

There is deliberately **no built-in Nasdaq retransmission client**. The
endpoints, entitlements and credentials for one are venue- and
contract-specific; inventing them would be worse than useless. `GapRecoveryPolicy`
is the injection point - implement it once you have an authorized feed.

Also handled, and tested: duplicate datagrams (suppressed exactly, including
partial overlap after a retransmission, where only the unseen tail is
delivered), heartbeats (count 0, never advance the sequence), end-of-session
(count 0xFFFF), session id changes (re-baseline and report), and malformed
packets (rejected whole - a half-applied packet is worse than a rejected one).

---

## ITCH parser

A single switch on the message type over a constexpr 256-entry length table,
so validating a message is one load and one compare before any field is read.
Every multi-byte field goes through an explicit big-endian load built on
`memcpy` - which folds to one unaligned load - rather than a `reinterpret_cast`
that would be undefined behaviour and would fault on a strict-alignment target.

No heap allocation, no strings, no dynamic parsing framework, and no
intermediate object: fields are decoded straight from the packet buffer into a
caller-owned `MarketEvent`.

Two decisions worth stating:

* **`C` (Order Executed with Price) does not give its price to the book.** That
  price is the execution print, which can differ from the display price. The
  book fills the resting order at the price it already rests at.
* **`S` with event code `O` (Start of Messages) maps to `Clear`.** It is the
  one point in the day where the book is known empty, which is what the
  historical path spells "Clear". Configurable off.

---

## Low-latency book internals

**Order pool.** A preallocated array of 24-byte nodes with an intrusive
doubly-linked list per price level and a freelist threaded through the `next`
field. Add is a freelist pop; delete is a push. No allocator involvement on any
path reachable from `apply()`, which the benchmark verifies by replacing the
global `operator new` and asserting zero.

**Price levels.** A flat array indexed by tick offset, plus a three-level
occupancy bitmap (one bit per tick, one per level-0 word, one per level-1
word). Best price is three dependent loads and three bit-scans; the next level
down is the same. Insert and erase are one OR, or one AND plus at most two more
when a word empties.

**Off-grid prices.** A tick-indexed array can only address prices on its grid
and inside its window, and real Nasdaq data does not stay there: the validation
session contains adds at $0.0001 and at $199,999.00, and seven that are not on
the penny grid at all. Rejecting them would be a book that is fast and wrong,
so they go into a small sorted overflow list that every query merges with the
grid in price order. The fast path is one branch (`overflow_.empty()`) and the
counters make the fallback visible. The real session needed 2 overflow levels.

**Order index.** Open addressing, structure-of-arrays so a probe touches 8
candidate keys per cache line instead of 5, Fibonacci hashing (ITCH order
references are near-sequential, the exact case where identity hashing clusters),
and **backward-shift deletion rather than tombstones** - an ITCH day is roughly
half cancels, and a tombstoned table degrades all day and then needs a rehash,
which is precisely the unbounded pause a hot path must not take. Measured on
the real session: 1.017 probes on average, 4 at worst.

---

## Capacity is a latency parameter

This is the single most surprising measured result of the work, so it gets its
own section.

Everything in the book is preallocated and never grows. The obvious way to set
the limits is generously - and that turns out to cost a lot, because an
oversized configuration spreads the working set over pages that are never used
and pays TLB and cache misses on every event.

Real AAPL 2026-07-30 session, peak ~55,700 resting orders (measured on the
14,469,900-message pre-fix normalization; the relative ordering is what
matters and is unchanged on the corrected 14,270,119-message day):

| configuration | book apply mean | p99 | book memory |
|---|---|---|---|
| 2^20 orders / 2^21 index / 2^20 ticks | 25.13 ns | 270 ns | 66 MiB |
| **2^17 orders / 2^18 index / 2^17 ticks** | **15.78 ns** | **70 ns** | **9 MiB** |
| 2^16 orders / 2^17 index / 2^16 ticks | 18.72 ns | 80 ns | 5 MiB |

The middle row is the shipped default: about 2.4x headroom over that session's
peak. Bigger is 60% slower in the mean and nearly 4x worse at p99. Smaller
pushes the index toward its load limit and starts costing probes.

Size it from your own data. `itch_replay` prints the peaks it saw, and
`validation/validate_low_latency.py` runs a cheap sizing pass first and reports
what it chose.

Exceeding any limit sets `degraded()`, increments `capacity_rejections` and
drops the event. It never half-applies one, and the tools exit non-zero.

---

## Measurement methodology

Latency numbers are easy to fake, so here is exactly how these are produced.

* **`cycles_begin()` is `LFENCE; RDTSC`, `cycles_end()` is `RDTSCP; LFENCE`.**
  Unfenced RDTSC is not ordered against surrounding instructions and measures
  the wrong window. The cost of the fence pair is measured and printed with
  every run (43 ticks on the reference machine).
* **"Cycles" means TSC ticks, not retired core clocks.** The TSC is invariant
  on the target hardware, which makes it a stable *time* unit but not a count
  of core cycles: at a boosted frequency more core cycles pass per tick. The
  benchmarks report whether the CPU advertises an invariant TSC and convert to
  nanoseconds with a measured frequency. Nothing here is a claim about IPC.
* **Throughput and distribution come from separate passes.** A fenced timer
  around every operation changes the loop it is timing; the mean therefore
  comes from a clean pass with no timer inside it, and the percentiles from a
  separate sampled pass.
* **Fenced per-operation numbers are a serialized upper bound.** The ITCH
  parse measures 30 ns at p50 with fences around it and 4.73 ns by subtraction
  from clean passes. Both are real: the fences stop the parse overlapping with
  neighbouring work, and real execution does overlap. The subtraction number is
  the one that describes throughput; the fenced one bounds a single isolated
  operation.
* **Percentiles are exact, from raw samples**, not from a bucketed histogram -
  p99.99 of ten million samples is the 1,000th worst value, and bucketing
  rounds exactly the numbers that matter. Recording is one bounds check and one
  4-byte store into a buffer allocated and touched before measurement begins.
* **Warm up first**, always. The first pass pays for page faults, cold branch
  predictors and an empty book.
* **Allocations are counted, not assumed.** The benchmarks replace the global
  `operator new`/`delete`, so an allocation three layers down inside something
  innocent-looking would still be caught.
* **Every run prints its conditions**: platform, TSC frequency and invariance,
  timer overhead, and whether pinning, memory locking and real-time scheduling
  were actually granted or refused.

### What these numbers are NOT

They are **host processing** numbers: packet-already-in-memory to book-updated.
They are not network-to-book latency, which additionally includes NIC, driver
and kernel time and can only be measured against a real feed with hardware
timestamps. No claim about exchange-level latency appears anywhere in this
repository, and there are no DPDK numbers at all because no DPDK hardware was
available.

---

## Results

Reference machine: Windows 11, MSVC 19.44, x64, TSC 4.300 GHz invariant,
pinned, Release (`/O2`). Full detail in the README.

**Pipeline**, real AAPL 2026-07-30 capture, 309,944 datagrams, 14,270,119
messages, 46.0 messages per datagram:

| stage | ticks/msg | ns/msg | throughput |
|---|---|---|---|
| UDP buffer -> MoldUDP64 | 11.28 | 2.62 | 381.3M msgs/s |
| ITCH decode -> MarketEvent | 20.09 | 4.67 | 214.1M msgs/s |
| MarketEvent -> LowLatencyOrderBook | 68.68 | 15.97 | 62.6M msgs/s |
| **complete pipeline** | **100.05** | **23.27** | **43.0M msgs/s** |
| (same, into the research book) | 495.80 | 115.30 | 8.2M msgs/s |

Allocations per message: **0**.

| distribution (ns) | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|
| ITCH parse (per message) | 30 | 30 | 30 | 60 | 190 | 30,051 |
| book apply (per message) | 40 | 50 | 70 | 150 | 380 | 96,383 |
| parse + book (per message) | 80 | 90 | 110 | 200 | 490 | 96,653 |
| whole datagram (46 messages) | 1,060 | 1,300 | 1,620 | 4,300 | 12,390 | 100,643 |

The maxima are OS scheduling artifacts on a general-purpose desktop, not
algorithmic: they are microseconds to milliseconds, thousands of times the
p99.99. See [linux_tuning.md](linux_tuning.md) for what removes them.

**Dispatch**, the same event stream through the engine with a feed and a
strategy:

| | ticks/event | ns/event |
|---|---|---|
| static (templates) | 107.00 | 24.88 |
| virtual (two vcalls) | 141.06 | 32.81 (+31.8%) |

Which is why the runtime is a template and the virtual interfaces exist for
tools and tests.

---

## Threading model

Single-threaded, and that is a decision rather than an omission.

A pipeline that hands events between a receive thread and a book thread pays a
cache-line transfer and a queue round trip per handoff - tens to hundreds of
nanoseconds - to overlap work that takes about the same. It buys throughput on
a saturated link and costs end-to-end latency. At 43M messages per second
against a feed that peaks in the low millions, the book is not the constraint,
so there are no threads, no mutexes, no shared mutable state and no queues.

When a measurement says otherwise, an SPSC ring buffer between the receiver and
the parser is the first thing to try, and the benchmark is already set up to
compare it.

---

## Optimizations that were tried and reverted

Kept here because a design document that only lists what worked is not telling
you much.

* **Padding the order node from 24 to 32 bytes** so two fit exactly in a cache
  line. Measured over three runs of 3M events: 16.879 / 16.892 / 16.863 ns
  packed against 16.908 / 17.002 / 17.040 ns padded, and 12% more memory. The
  padding does not pay for itself, so the default is the packed node. The
  comparison is still buildable (`bench_book_packed`) so it can be re-run on
  different hardware.
* **Generous default capacities.** See "Capacity is a latency parameter" above:
  the original 2^20/2^21/2^20 defaults were 60% slower in the mean and nearly
  4x worse at p99 than right-sized ones.

Both were found by benchmarking rather than by inspection, which is the point.

---

## Remaining bottlenecks, in the order worth attacking

1. **The order index is the largest remaining term in `apply()`.** Every
   non-Add event is a hash lookup into a table far larger than L2. Direct
   addressing on a truncated order reference (Nasdaq assigns them
   near-sequentially) would replace it with one load, at the cost of a
   sparse array and a fallback for references outside the window.
2. **Level lookup on Add** touches the tick array, the bitmap and the level
   pool - three separate lines. Storing hot level fields adjacent to the tick
   slot would fold two of them together.
3. **The whole-datagram tail** (p99.9 2.76 us against a p50 of 1.06 us) is
   dominated by scheduler noise on a desktop OS, not by the code. Pinning,
   isolated cores and a fixed governor address it; nothing in the source will.
4. **Receive-side batching is unmeasured on this machine.** `recvmmsg` is
   implemented and Linux-only; the Windows development host falls back to
   `recvfrom` per datagram. The syscall cost is the dominant term in the kernel
   receive path and none of the numbers here include it.
5. **Software prefetch has not been tried.** It should be, but only with a
   measurement - a prefetch added on intuition is as likely to evict something
   useful as to help.
