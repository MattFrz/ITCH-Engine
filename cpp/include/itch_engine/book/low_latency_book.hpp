#pragma once

// LowLatencyOrderBook - the production-shaped sibling of itch::OrderBook.
//
// The research book (cpp/src/order_book.cpp) is unchanged and still the
// reference: std::map levels, std::list queues, std::unordered_map index. It
// is correct, readable, and validated. This one trades that readability for
// the properties a network-to-book path needs, and every difference is a
// deliberate answer to something the research book does that a hot path must
// not:
//
//   research book                     low-latency book
//   ------------------------------    ---------------------------------------
//   std::list node per add            index into a preallocated order pool
//   (one malloc/free per order)       (freelist pop/push, zero allocation)
//
//   std::map<Price, PriceLevel>       flat tick-indexed array + 3-level
//   (O(log L), a pointer chase        occupancy bitmap (O(1) lookup, best
//   per comparison, node per level)   price in three dependent loads)
//
//   std::unordered_map order index    open-addressing table, SoA, no
//   (node-based, cache-hostile,       tombstones, no rehash ever
//   rehashes without warning)
//
// Everything the research book guarantees semantically is guaranteed here,
// byte for byte: FIFO priority, partial cancel keeps position, size-up and
// reprice lose it, unknown references are tolerated and counted. That is not
// an aspiration - cpp/tests/test_differential.cpp replays multi-million-event
// streams through both and fails on the first divergence in any observable.
//
// Off-grid and out-of-window prices
// ---------------------------------
// A tick-indexed array only addresses prices on its grid and inside its
// window. Real Nasdaq data does not stay there: the AAPL session used for
// validation contains adds at $0.0001 and at $199,999.00, and seven adds that
// are not on the penny grid at all. Rejecting those would be a book that is
// fast and wrong. They go into a small sorted overflow list instead, merged
// with the grid in price order for every query. The fast path is one branch
// (`overflow_.empty()`), and the counters make the fallback visible.
//
// Capacity
// --------
// Every limit is explicit and monitored. Exhausting the order pool, the level
// pool, the index or the overflow list sets `degraded()` and counts the
// rejection; the event is dropped rather than half-applied. A silently
// truncated book is the failure mode this design exists to avoid.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "itch_engine/book/hierarchical_bitmap.hpp"
#include "itch_engine/book/order_index.hpp"
#include "itch_engine/market_event.hpp"
#include "itch_engine/order_book.hpp"  // LevelView, BestQuote - identical types
#include "itch_engine/types.hpp"

// Order-node padding, kept as a build flag because it is a measurement rather
// than an opinion.
//
// The plausible argument for padding to 32 bytes: two nodes per cache line
// exactly, and no node ever straddling a line. The argument against: a third
// more memory, and therefore a third more of the cache and TLB pressure that
// turned out to dominate this book's tail (see BookConfig below).
//
// Measured, 3M synthetic events, pinned, best of 3, repeated three times:
//
//   24-byte packed : 16.879 / 16.892 / 16.863 ns per event, 8.16 MiB
//   32-byte padded : 16.908 / 17.002 / 17.040 ns per event, 9.16 MiB
//
// The padding does not pay for itself, so it is off. Build
// `bench_book_packed` alongside `bench_book` to re-run the comparison on your
// own hardware before changing this.
#ifndef ITCH_LL_ORDER_PAD32
#define ITCH_LL_ORDER_PAD32 0
#endif

namespace itch {
namespace book {

struct LLOrder {
    OrderId id;             // 8
    std::uint32_t qty;      // 4
    std::uint32_t next;     // 4  kNull terminated; also the freelist link
    std::uint32_t prev;     // 4
    std::uint32_t level;    // 4  index into the shared level pool
#if ITCH_LL_ORDER_PAD32
    std::uint32_t pad_[2];  // 8
#endif
};

struct LLLevel {
    Price price;                // 8
    std::int64_t qty;           // 8  aggregate resting quantity
    std::uint32_t head;         // 4  first order (FIFO front); freelist link when free
    std::uint32_t tail;         // 4  last order (FIFO back)
    std::uint32_t count;        // 4  order count
    std::uint32_t tick_side;    // 4  bit31 = side, bits 0-30 = tick index or kOffGrid
};

static_assert(sizeof(LLLevel) == 32, "LLLevel must stay 32 bytes (half a cache line)");
#if ITCH_LL_ORDER_PAD32
static_assert(sizeof(LLOrder) == 32, "padded LLOrder must be 32 bytes");
#else
static_assert(sizeof(LLOrder) == 24, "packed LLOrder must be 24 bytes");
#endif

// Capacity is a LATENCY parameter here, not only a safety limit.
//
// Everything below is preallocated and never grows, so an oversized
// configuration does not cost only memory: it spreads the working set over
// pages that are never used and pays TLB and cache misses on every event.
// This is measured, not assumed. On the real AAPL 2026-07-30 session
// (14,469,900 messages, peak 55,682 resting orders), with the tick grid, order
// pool and index scaled together:
//
//   configuration                     book apply mean   p99      book memory
//   ------------------------------    ---------------   ------   -----------
//   2^20 orders / 2^21 index / 2^20 ticks     25.1 ns   270 ns      66 MiB
//   2^17 orders / 2^18 index / 2^17 ticks     15.8 ns    70 ns       9 MiB
//   2^16 orders / 2^17 index / 2^16 ticks     18.7 ns    80 ns       5 MiB
//
// The 2^17 row is the default below: about 2.4x headroom over that session's
// peak. Going bigger is 60% slower in the mean and nearly 4x worse at p99;
// going smaller pushes the index toward its load limit and starts costing
// probes. Size this from your own data - `itch_replay` prints the peaks it
// saw, and validation/validate_low_latency.py sizes itself from the day.
//
// Exceeding any limit is reported (degraded(), capacity_rejections) and drops
// the event; it never half-applies one.
struct BookConfig {
    // Peak simultaneously resting orders.
    std::size_t max_orders = 1u << 17;  // 131,072

    // Order-index slots. Max load factor is 0.5, so this must be >= 2x
    // max_orders for the index never to reject.
    std::size_t index_capacity = 1u << 18;  // 262,144

    // Live price levels across both sides. The reference session peaked at
    // 9,257.
    std::size_t max_levels = 1u << 15;  // 32,768

    // Tick grid: $0.01 ticks from $0.00, 2^17 of them, so prices from $0.00 to
    // $1,310.71 are addressed directly. Anything outside - and anything not on
    // the penny grid, which real ITCH data does contain - goes through the
    // off-grid overflow, which is correct but slower.
    Price tick_size = 10'000'000;  // $0.01 in 1e-9 USD units
    Price base_price = 0;
    std::size_t tick_count = 1u << 17;

    // Levels living outside the grid. The reference session needed 2.
    std::size_t max_offgrid_levels = 4096;
};

struct BookMetrics {
    std::uint64_t capacity_rejections = 0;
    std::size_t peak_orders = 0;
    std::size_t peak_levels = 0;
    std::size_t peak_offgrid = 0;
    std::uint64_t offgrid_level_creations = 0;
    std::uint64_t offgrid_lookups = 0;
};

class LowLatencyOrderBook {
public:
    static constexpr std::uint32_t kNull = 0xFFFFFFFFu;
    static constexpr std::uint32_t kOffGrid = 0x7FFFFFFFu;

    LowLatencyOrderBook() { reset(BookConfig{}); }
    explicit LowLatencyOrderBook(const BookConfig& cfg) { reset(cfg); }

    // Allocates everything, once. After this the book never touches the
    // allocator again on any code path reachable from apply().
    void reset(const BookConfig& cfg) {
        cfg_ = cfg;
        if (cfg_.tick_size <= 0) cfg_.tick_size = 1;

        orders_.assign(cfg_.max_orders, LLOrder{});
        for (std::size_t i = 0; i + 1 < orders_.size(); ++i) {
            orders_[i].next = static_cast<std::uint32_t>(i + 1);
        }
        if (!orders_.empty()) orders_.back().next = kNull;
        order_free_ = orders_.empty() ? kNull : 0;
        orders_used_ = 0;

        levels_.assign(cfg_.max_levels, LLLevel{});
        for (std::size_t i = 0; i + 1 < levels_.size(); ++i) {
            levels_[i].head = static_cast<std::uint32_t>(i + 1);
        }
        if (!levels_.empty()) levels_.back().head = kNull;
        level_free_ = levels_.empty() ? kNull : 0;
        levels_used_ = 0;

        for (int s = 0; s < 2; ++s) {
            grid_[s].assign(cfg_.tick_count, kNull);
            occupancy_[s].reset(cfg_.tick_count);
            overflow_[s].clear();
            overflow_[s].reserve(cfg_.max_offgrid_levels);
        }

        index_.reset(cfg_.index_capacity);
        events_processed_ = 0;
        unknown_order_events_ = 0;
        clears_applied_ = 0;
        metrics_ = BookMetrics{};
        degraded_ = false;
    }

    const BookConfig& config() const noexcept { return cfg_; }

    // --- hot path -----------------------------------------------------------

    void apply(const MarketEvent& ev) noexcept {
        if (!mutates_book(ev.type)) return;
        ++events_processed_;

        if (ev.type == EventType::Add) {
            add_order(ev);
            return;
        }
        if (ev.type == EventType::Clear) {
            clear();
            return;
        }

        const std::uint32_t slot = index_.find(ev.order_id);
        if (slot == OrderIndex::kNull) {
            ++unknown_order_events_;
            return;
        }

        switch (ev.type) {
            case EventType::Cancel: cancel_order(ev.quantity, slot, ev.order_id); break;
            case EventType::Delete: cancel_order(0, slot, ev.order_id); break;
            case EventType::Execute: execute_order(ev, slot); break;
            case EventType::Modify: modify_order(ev, slot); break;
            case EventType::Replace: replace_order(ev, slot); break;
            default: break;
        }
    }

    // Legacy Event entry point, so the same book can be driven from the
    // historical arrays without converting first.
    void apply(const Event& ev) noexcept { apply(from_legacy(ev)); }

    void clear() noexcept {
        for (int s = 0; s < 2; ++s) {
            // Reset the grid slots that are actually live rather than memsetting
            // 4 MiB per side: a clear happens once or twice a day, but touching
            // 8 MiB evicts the whole working set with it.
            const auto& bm = occupancy_[s];
            std::size_t t = bm.lowest();
            while (t != HierarchicalBitmap::kNone) {
                grid_[s][t] = kNull;
                t = bm.next_above(t);
            }
            occupancy_[s].clear();
            overflow_[s].clear();
        }
        // Rebuild both freelists in one pass.
        for (std::size_t i = 0; i + 1 < orders_.size(); ++i) {
            orders_[i].next = static_cast<std::uint32_t>(i + 1);
        }
        if (!orders_.empty()) orders_.back().next = kNull;
        order_free_ = orders_.empty() ? kNull : 0;
        orders_used_ = 0;
        for (std::size_t i = 0; i + 1 < levels_.size(); ++i) {
            levels_[i].head = static_cast<std::uint32_t>(i + 1);
        }
        if (!levels_.empty()) levels_.back().head = kNull;
        level_free_ = levels_.empty() ? kNull : 0;
        levels_used_ = 0;
        index_.clear();
        ++clears_applied_;
    }

    // --- counters (identical semantics to itch::OrderBook) ------------------

    std::uint64_t events_processed() const noexcept { return events_processed_; }
    std::uint64_t clears_applied() const noexcept { return clears_applied_; }
    std::uint64_t unknown_order_events() const noexcept { return unknown_order_events_; }

    // --- health -------------------------------------------------------------

    bool degraded() const noexcept { return degraded_; }
    const BookMetrics& metrics() const noexcept { return metrics_; }
    const OrderIndex& index() const noexcept { return index_; }
    std::size_t orders_in_use() const noexcept { return orders_used_; }
    std::size_t levels_in_use() const noexcept { return levels_used_; }

    std::size_t memory_bytes() const noexcept {
        std::size_t n = orders_.capacity() * sizeof(LLOrder) + levels_.capacity() * sizeof(LLLevel);
        for (int s = 0; s < 2; ++s) {
            n += grid_[s].capacity() * sizeof(std::uint32_t);
            n += occupancy_[s].memory_bytes();
            n += overflow_[s].capacity() * sizeof(OffGrid);
        }
        return n + index_.memory_bytes();
    }

    // --- state queries (same shape as itch::OrderBook) ----------------------

    BestQuote best() const noexcept {
        BestQuote q;
        const std::uint32_t b = best_level(0);
        if (b != kNull) {
            q.has_bid = true;
            q.bid_price = levels_[b].price;
            q.bid_qty = levels_[b].qty;
        }
        const std::uint32_t a = best_level(1);
        if (a != kNull) {
            q.has_ask = true;
            q.ask_price = levels_[a].price;
            q.ask_qty = levels_[a].qty;
        }
        return q;
    }

    std::optional<double> mid_price() const noexcept {
        const std::uint32_t b = best_level(0);
        const std::uint32_t a = best_level(1);
        if (b == kNull || a == kNull) return std::nullopt;
        const double bid = static_cast<double>(levels_[b].price);
        const double ask = static_cast<double>(levels_[a].price);
        return (bid + ask) / 2.0 / static_cast<double>(PRICE_SCALE);
    }

    Qty bid_qty_at(Price price) const noexcept { return qty_at(0, price); }
    Qty ask_qty_at(Price price) const noexcept { return qty_at(1, price); }

    std::vector<LevelView> depth(Side side, std::size_t n) const {
        std::vector<LevelView> out;
        out.reserve(n);
        walk_depth(side == Side::Bid ? 0 : 1, n, [&](const LLLevel& lv) {
            out.push_back(LevelView{lv.price, lv.qty, lv.count});
        });
        return out;
    }

    std::size_t bid_level_count() const noexcept { return level_count(0); }
    std::size_t ask_level_count() const noexcept { return level_count(1); }
    std::size_t open_order_count() const noexcept { return index_.size(); }

    std::optional<Qty> order_qty(OrderId id) const noexcept {
        const std::uint32_t slot = index_.find(id);
        if (slot == OrderIndex::kNull) return std::nullopt;
        return static_cast<Qty>(orders_[slot].qty);
    }

    // FIFO quantity ahead of a live order. O(position), analysis only - the
    // same complexity and the same answer as the research book.
    std::optional<Qty> queue_ahead(OrderId id) const noexcept {
        const std::uint32_t slot = index_.find(id);
        if (slot == OrderIndex::kNull) return std::nullopt;
        const LLLevel& lv = levels_[orders_[slot].level];
        Qty ahead = 0;
        for (std::uint32_t cur = lv.head; cur != kNull; cur = orders_[cur].next) {
            if (cur == slot) return ahead;
            ahead += static_cast<Qty>(orders_[cur].qty);
        }
        return std::nullopt;
    }

    // Order ids at a price level, front of queue first. Test/diagnostic only.
    std::vector<OrderId> queue_at(Side side, Price price) const {
        std::vector<OrderId> out;
        const std::uint32_t lidx = find_level(side == Side::Bid ? 0 : 1, price);
        if (lidx == kNull) return out;
        for (std::uint32_t cur = levels_[lidx].head; cur != kNull; cur = orders_[cur].next) {
            out.push_back(orders_[cur].id);
        }
        return out;
    }

private:
    struct OffGrid {
        Price price;
        std::uint32_t level;
    };

    // --- pools --------------------------------------------------------------

    std::uint32_t alloc_order() noexcept {
        if (order_free_ == kNull) return kNull;
        const std::uint32_t slot = order_free_;
        order_free_ = orders_[slot].next;
        ++orders_used_;
        if (orders_used_ > metrics_.peak_orders) metrics_.peak_orders = orders_used_;
        return slot;
    }

    void free_order(std::uint32_t slot) noexcept {
        orders_[slot].next = order_free_;
        order_free_ = slot;
        --orders_used_;
    }

    std::uint32_t alloc_level() noexcept {
        if (level_free_ == kNull) return kNull;
        const std::uint32_t idx = level_free_;
        level_free_ = levels_[idx].head;
        ++levels_used_;
        if (levels_used_ > metrics_.peak_levels) metrics_.peak_levels = levels_used_;
        return idx;
    }

    void free_level(std::uint32_t idx) noexcept {
        levels_[idx].head = level_free_;
        level_free_ = idx;
        --levels_used_;
    }

    void fail_capacity() noexcept {
        ++metrics_.capacity_rejections;
        degraded_ = true;
    }

    // --- tick grid ----------------------------------------------------------

    // Returns kOffGrid for a price that is not addressable by the grid.
    std::uint32_t tick_of(Price price) const noexcept {
        const Price rel = price - cfg_.base_price;
        if (rel < 0) return kOffGrid;
        if (rel % cfg_.tick_size != 0) return kOffGrid;
        const Price t = rel / cfg_.tick_size;
        if (t >= static_cast<Price>(cfg_.tick_count)) return kOffGrid;
        return static_cast<std::uint32_t>(t);
    }

    Price price_of_tick(std::size_t tick) const noexcept {
        return cfg_.base_price + static_cast<Price>(tick) * cfg_.tick_size;
    }

    static std::uint32_t pack_tick_side(std::uint32_t tick, int side) noexcept {
        return tick | (static_cast<std::uint32_t>(side) << 31);
    }
    static std::uint32_t unpack_tick(std::uint32_t tick_side) noexcept {
        return tick_side & 0x7FFFFFFFu;
    }
    static int unpack_side(std::uint32_t tick_side) noexcept {
        return static_cast<int>(tick_side >> 31);
    }

    // Binary search of the overflow list (sorted ascending by price).
    std::size_t offgrid_pos(int side, Price price) const noexcept {
        const auto& v = overflow_[side];
        std::size_t lo = 0, hi = v.size();
        while (lo < hi) {
            const std::size_t mid = (lo + hi) / 2;
            if (v[mid].price < price) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    std::uint32_t find_level(int side, Price price) const noexcept {
        const std::uint32_t tick = tick_of(price);
        if (tick != kOffGrid) return grid_[side][tick];
        if (overflow_[side].empty()) return kNull;
        ++metrics_.offgrid_lookups;
        const std::size_t pos = offgrid_pos(side, price);
        if (pos < overflow_[side].size() && overflow_[side][pos].price == price) {
            return overflow_[side][pos].level;
        }
        return kNull;
    }

    // Finds or creates the level for `price`. kNull means capacity exhausted.
    std::uint32_t level_for(int side, Price price) noexcept {
        const std::uint32_t tick = tick_of(price);
        if (tick != kOffGrid) {
            std::uint32_t idx = grid_[side][tick];
            if (idx != kNull) return idx;
            idx = alloc_level();
            if (idx == kNull) {
                fail_capacity();
                return kNull;
            }
            init_level(idx, price, pack_tick_side(tick, side));
            grid_[side][tick] = idx;
            occupancy_[side].set(tick);
            return idx;
        }

        ++metrics_.offgrid_lookups;
        const std::size_t pos = offgrid_pos(side, price);
        auto& v = overflow_[side];
        if (pos < v.size() && v[pos].price == price) return v[pos].level;
        if (v.size() >= cfg_.max_offgrid_levels) {
            fail_capacity();
            return kNull;
        }
        const std::uint32_t idx = alloc_level();
        if (idx == kNull) {
            fail_capacity();
            return kNull;
        }
        init_level(idx, price, pack_tick_side(kOffGrid, side));
        v.insert(v.begin() + static_cast<std::ptrdiff_t>(pos), OffGrid{price, idx});
        ++metrics_.offgrid_level_creations;
        if (v.size() > metrics_.peak_offgrid) metrics_.peak_offgrid = v.size();
        return idx;
    }

    void init_level(std::uint32_t idx, Price price, std::uint32_t tick_side) noexcept {
        LLLevel& lv = levels_[idx];
        lv.price = price;
        lv.qty = 0;
        lv.head = kNull;
        lv.tail = kNull;
        lv.count = 0;
        lv.tick_side = tick_side;
    }

    void release_level(std::uint32_t idx) noexcept {
        const LLLevel& lv = levels_[idx];
        const int side = unpack_side(lv.tick_side);
        const std::uint32_t tick = unpack_tick(lv.tick_side);
        if (tick != kOffGrid) {
            grid_[side][tick] = kNull;
            occupancy_[side].reset_bit(tick);
        } else {
            auto& v = overflow_[side];
            const std::size_t pos = offgrid_pos(side, lv.price);
            if (pos < v.size() && v[pos].price == lv.price) {
                v.erase(v.begin() + static_cast<std::ptrdiff_t>(pos));
            }
        }
        free_level(idx);
    }

    // --- level queue --------------------------------------------------------

    void link_back(std::uint32_t lidx, std::uint32_t slot) noexcept {
        LLLevel& lv = levels_[lidx];
        LLOrder& o = orders_[slot];
        o.next = kNull;
        o.prev = lv.tail;
        o.level = lidx;
        if (lv.tail != kNull) {
            orders_[lv.tail].next = slot;
        } else {
            lv.head = slot;
        }
        lv.tail = slot;
        lv.qty += static_cast<std::int64_t>(o.qty);
        ++lv.count;
    }

    void unlink(std::uint32_t slot) noexcept {
        LLOrder& o = orders_[slot];
        LLLevel& lv = levels_[o.level];
        if (o.prev != kNull) {
            orders_[o.prev].next = o.next;
        } else {
            lv.head = o.next;
        }
        if (o.next != kNull) {
            orders_[o.next].prev = o.prev;
        } else {
            lv.tail = o.prev;
        }
        lv.qty -= static_cast<std::int64_t>(o.qty);
        --lv.count;
    }

    // Removes an order entirely: unlink, drop the level if it emptied, return
    // the pool slot and forget the id.
    void remove_order(std::uint32_t slot, OrderId id) noexcept {
        const std::uint32_t lidx = orders_[slot].level;
        unlink(slot);
        if (levels_[lidx].count == 0) release_level(lidx);
        free_order(slot);
        index_.erase(id);
    }

    // --- event handlers (semantics mirror itch::OrderBook exactly) ----------

    void add_order(const MarketEvent& ev) noexcept {
        if (ev.quantity == 0) return;  // the research book drops these silently
        if (index_.find(ev.order_id) != OrderIndex::kNull) {
            ++unknown_order_events_;  // duplicate add for a live id: anomaly
            return;
        }
        const int side = ev.side == Side::Ask ? 1 : 0;
        const std::uint32_t lidx = level_for(side, ev.price);
        if (lidx == kNull) return;  // capacity: counted, event dropped
        const std::uint32_t slot = alloc_order();
        if (slot == kNull) {
            if (levels_[lidx].count == 0) release_level(lidx);
            fail_capacity();
            return;
        }
        orders_[slot].id = ev.order_id;
        orders_[slot].qty = ev.quantity;
        if (!index_.insert(ev.order_id, slot)) {
            free_order(slot);
            if (levels_[lidx].count == 0) release_level(lidx);
            fail_capacity();
            return;
        }
        link_back(lidx, slot);
    }

    // Shared by Cancel (partial, qty > 0) and Delete (qty == 0 = full).
    void cancel_order(std::uint32_t qty, std::uint32_t slot, OrderId id) noexcept {
        const std::uint32_t remaining = orders_[slot].qty;
        if (qty == 0 || qty >= remaining) {
            remove_order(slot, id);
            return;
        }
        LLOrder& o = orders_[slot];
        o.qty = remaining - qty;
        levels_[o.level].qty -= static_cast<std::int64_t>(qty);
    }

    void execute_order(const MarketEvent& ev, std::uint32_t slot) noexcept {
        const std::uint32_t remaining = orders_[slot].qty;
        const std::uint32_t fill = ev.quantity < remaining ? ev.quantity : remaining;
        if (fill == 0) return;  // an execute for 0 shares is a no-op, not a delete
        if (fill == remaining) {
            remove_order(slot, ev.order_id);
            return;
        }
        LLOrder& o = orders_[slot];
        o.qty = remaining - fill;
        levels_[o.level].qty -= static_cast<std::int64_t>(fill);
    }

    void modify_order(const MarketEvent& ev, std::uint32_t slot) noexcept {
        LLOrder& o = orders_[slot];
        const std::uint32_t lidx = o.level;
        const Price old_price = levels_[lidx].price;
        const std::uint32_t old_qty = o.qty;

        if (ev.price == old_price && ev.quantity <= old_qty) {
            // Size-down at the same price keeps FIFO position.
            if (ev.quantity == old_qty) return;
            if (ev.quantity == 0) {
                remove_order(slot, ev.order_id);
                return;
            }
            o.qty = ev.quantity;
            levels_[lidx].qty -= static_cast<std::int64_t>(old_qty - ev.quantity);
            return;
        }

        // Reprice or size-up: to the back of the (possibly new) queue.
        const int side = unpack_side(levels_[lidx].tick_side);
        unlink(slot);
        if (levels_[lidx].count == 0) release_level(lidx);
        if (ev.quantity == 0) {
            free_order(slot);
            index_.erase(ev.order_id);
            return;
        }
        const std::uint32_t nlidx = level_for(side, ev.price);
        if (nlidx == kNull) {
            free_order(slot);
            index_.erase(ev.order_id);
            return;  // capacity already counted
        }
        orders_[slot].qty = ev.quantity;
        link_back(nlidx, slot);
    }

    void replace_order(const MarketEvent& ev, std::uint32_t slot) noexcept {
        const std::uint32_t lidx = orders_[slot].level;
        const int side = unpack_side(levels_[lidx].tick_side);
        remove_order(slot, ev.order_id);

        if (ev.quantity == 0) return;  // replace-to-zero is a delete
        if (ev.new_order_id != ev.order_id &&
            index_.find(ev.new_order_id) != OrderIndex::kNull) {
            ++unknown_order_events_;
            return;
        }
        const std::uint32_t nlidx = level_for(side, ev.price);
        if (nlidx == kNull) return;
        const std::uint32_t nslot = alloc_order();
        if (nslot == kNull) {
            if (levels_[nlidx].count == 0) release_level(nlidx);
            fail_capacity();
            return;
        }
        orders_[nslot].id = ev.new_order_id;
        orders_[nslot].qty = ev.quantity;
        if (!index_.insert(ev.new_order_id, nslot)) {
            free_order(nslot);
            if (levels_[nlidx].count == 0) release_level(nlidx);
            fail_capacity();
            return;
        }
        link_back(nlidx, nslot);
    }

    // --- queries ------------------------------------------------------------

    std::size_t level_count(int side) const noexcept {
        return occupancy_[side].size() + overflow_[side].size();
    }

    Qty qty_at(int side, Price price) const noexcept {
        const std::uint32_t idx = find_level(side, price);
        return idx == kNull ? 0 : levels_[idx].qty;
    }

    // Best level on a side, merging the tick grid with the overflow list.
    std::uint32_t best_level(int side) const noexcept {
        const std::size_t tick =
            side == 0 ? occupancy_[side].highest() : occupancy_[side].lowest();
        const bool has_grid = tick != HierarchicalBitmap::kNone;
        if (overflow_[side].empty()) {
            return has_grid ? grid_[side][tick] : kNull;
        }
        // Overflow is sorted ascending: bids want the back, asks the front.
        const OffGrid& og = side == 0 ? overflow_[side].back() : overflow_[side].front();
        if (!has_grid) return og.level;
        const Price grid_price = price_of_tick(tick);
        const bool grid_wins = side == 0 ? grid_price > og.price : grid_price < og.price;
        return grid_wins ? grid_[side][tick] : og.level;
    }

    // Walks levels best-first, merging grid and overflow in price order.
    template <class Fn>
    void walk_depth(int side, std::size_t n, Fn&& fn) const {
        std::size_t tick = side == 0 ? occupancy_[side].highest() : occupancy_[side].lowest();
        const auto& ov = overflow_[side];
        std::ptrdiff_t oi = side == 0 ? static_cast<std::ptrdiff_t>(ov.size()) - 1 : 0;
        std::size_t emitted = 0;
        while (emitted < n) {
            const bool has_grid = tick != HierarchicalBitmap::kNone;
            const bool has_ov =
                side == 0 ? oi >= 0 : oi < static_cast<std::ptrdiff_t>(ov.size());
            if (!has_grid && !has_ov) return;

            bool take_grid = has_grid;
            if (has_grid && has_ov) {
                const Price gp = price_of_tick(tick);
                const Price op = ov[static_cast<std::size_t>(oi)].price;
                take_grid = side == 0 ? gp > op : gp < op;
            }
            if (take_grid) {
                fn(levels_[grid_[side][tick]]);
                tick = side == 0 ? occupancy_[side].prev_below(tick)
                                 : occupancy_[side].next_above(tick);
            } else {
                fn(levels_[ov[static_cast<std::size_t>(oi)].level]);
                oi += side == 0 ? -1 : 1;
            }
            ++emitted;
        }
    }

    BookConfig cfg_;
    std::vector<LLOrder> orders_;
    std::vector<LLLevel> levels_;
    std::uint32_t order_free_ = kNull;
    std::uint32_t level_free_ = kNull;
    std::size_t orders_used_ = 0;
    std::size_t levels_used_ = 0;

    std::vector<std::uint32_t> grid_[2];
    HierarchicalBitmap occupancy_[2];
    std::vector<OffGrid> overflow_[2];

    OrderIndex index_;

    std::uint64_t events_processed_ = 0;
    std::uint64_t unknown_order_events_ = 0;
    std::uint64_t clears_applied_ = 0;
    bool degraded_ = false;
    mutable BookMetrics metrics_;
};

}  // namespace book
}  // namespace itch
