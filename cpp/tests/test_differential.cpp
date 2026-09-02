// Differential test: the research book against the low-latency book.
//
// This is the test that makes the new book trustworthy. The research book
// (std::map / std::list / std::unordered_map) is the validated reference - the
// repository's headline result is that it agrees to zero drift with an
// independently written Python book across a real 14.5M-message session. Any
// observable difference between it and the low-latency book is therefore a bug
// in the new book, and it fails here.
//
// Both books are fed the identical MarketEvent stream. At every checkpoint the
// full observable surface is compared:
//
//   * every counter (events processed, unknown references, clears applied)
//   * open order count and live level count per side
//   * best bid/ask (price and aggregate size) and the mid
//   * the top 20 levels per side: price, aggregate quantity, order count
//   * for every live order: remaining quantity AND exact FIFO queue_ahead
//
// The last one is the strong check. Two books can agree on aggregate level
// quantities while disagreeing about queue order; comparing queue_ahead for
// every resting order pins the exact FIFO position of every order in the book.

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "itch_engine/book/low_latency_book.hpp"
#include "itch_engine/order_book.hpp"
#include "test_support.hpp"

using namespace itch;
using namespace itch::book;
using itch_test::GeneratorConfig;

namespace {

std::size_t compared_orders = 0;

void compare_optional_qty(const std::optional<Qty>& a, const std::optional<Qty>& b,
                          const char* what, OrderId id, std::size_t step) {
    if (a.has_value() != b.has_value() || (a.has_value() && *a != *b)) {
        std::fprintf(stderr,
                     "DIVERGENCE at event %zu: %s(order %llu): research=%s low_latency=%s\n", step,
                     what, static_cast<unsigned long long>(id),
                     a.has_value() ? std::to_string(*a).c_str() : "none",
                     b.has_value() ? std::to_string(*b).c_str() : "none");
        std::exit(1);
    }
}

void compare(const OrderBook& research, const LowLatencyOrderBook& fast, std::size_t step,
             bool full) {
    CHECK_EQ(fast.events_processed(), research.events_processed());
    CHECK_EQ(fast.unknown_order_events(), research.unknown_order_events());
    CHECK_EQ(fast.clears_applied(), research.clears_applied());
    CHECK_EQ(fast.open_order_count(), research.open_order_count());
    CHECK_EQ(fast.bid_level_count(), research.bid_level_count());
    CHECK_EQ(fast.ask_level_count(), research.ask_level_count());

    const BestQuote rq = research.best();
    const BestQuote fq = fast.best();
    CHECK_EQ(fq.has_bid, rq.has_bid);
    CHECK_EQ(fq.has_ask, rq.has_ask);
    CHECK_EQ(fq.bid_price, rq.bid_price);
    CHECK_EQ(fq.bid_qty, rq.bid_qty);
    CHECK_EQ(fq.ask_price, rq.ask_price);
    CHECK_EQ(fq.ask_qty, rq.ask_qty);

    const auto rmid = research.mid_price();
    const auto fmid = fast.mid_price();
    CHECK_EQ(rmid.has_value(), fmid.has_value());
    if (rmid.has_value()) CHECK(*rmid == *fmid);

    for (Side side : {Side::Bid, Side::Ask}) {
        const auto rd = research.depth(side, 20);
        const auto fd = fast.depth(side, 20);
        CHECK_EQ(fd.size(), rd.size());
        for (std::size_t i = 0; i < rd.size(); ++i) {
            if (rd[i].price != fd[i].price || rd[i].qty != fd[i].qty ||
                rd[i].order_count != fd[i].order_count) {
                std::fprintf(stderr,
                             "DIVERGENCE at event %zu: depth[%zu] side=%d research=(%lld,%lld,%zu) "
                             "low_latency=(%lld,%lld,%zu)\n",
                             step, i, static_cast<int>(side),
                             static_cast<long long>(rd[i].price),
                             static_cast<long long>(rd[i].qty), rd[i].order_count,
                             static_cast<long long>(fd[i].price),
                             static_cast<long long>(fd[i].qty), fd[i].order_count);
                std::exit(1);
            }
            ++itch_test::checks_run;
        }
    }

    if (!full) return;

    // Exact FIFO order at every live level, both sides. Comparing the id
    // sequence directly is linear in the number of resting orders; calling
    // queue_ahead once per order would be quadratic and would not check more.
    for (Side side : {Side::Bid, Side::Ask}) {
        const std::size_t levels =
            side == Side::Bid ? research.bid_level_count() : research.ask_level_count();
        const auto rd = research.depth(side, levels);
        for (const LevelView& lv : rd) {
            const auto research_queue = research.queue_at(side, lv.price);
            const auto fast_queue = fast.queue_at(side, lv.price);
            if (research_queue != fast_queue) {
                std::fprintf(stderr,
                             "DIVERGENCE at event %zu: FIFO order differs at side=%d price=%lld "
                             "(research %zu orders, low_latency %zu orders)\n",
                             step, static_cast<int>(side), static_cast<long long>(lv.price),
                             research_queue.size(), fast_queue.size());
                for (std::size_t i = 0; i < research_queue.size() && i < fast_queue.size(); ++i) {
                    if (research_queue[i] != fast_queue[i]) {
                        std::fprintf(stderr, "  first difference at position %zu: %llu vs %llu\n", i,
                                     static_cast<unsigned long long>(research_queue[i]),
                                     static_cast<unsigned long long>(fast_queue[i]));
                        break;
                    }
                }
                std::exit(1);
            }
            ++itch_test::checks_run;
            compared_orders += research_queue.size();
        }
    }

    // Remaining quantity for every live order, plus queue_ahead for a bounded
    // sample (it is O(position), so checking all of them at every checkpoint
    // would dominate the test).
    std::size_t seen = 0;
    std::size_t sampled = 0;
    fast.index().for_each([&](OrderId id, std::uint32_t) {
        ++seen;
        compare_optional_qty(research.order_qty(id), fast.order_qty(id), "order_qty", id, step);
        if (sampled < 200) {
            compare_optional_qty(research.queue_ahead(id), fast.queue_ahead(id), "queue_ahead", id,
                                 step);
            ++sampled;
        }
    });
    // Equal counts plus every id in the fast book present in the research book
    // means the two id sets are identical.
    CHECK_EQ(seen, research.open_order_count());
}

void run_stream(const GeneratorConfig& cfg, std::uint64_t seed, const char* label,
                const BookConfig& book_cfg) {
    const auto events = itch_test::generate_events(cfg, seed);
    OrderBook research;
    LowLatencyOrderBook fast(book_cfg);

    const std::size_t checkpoint_every = 1000;
    const std::size_t full_every = 20000;
    for (std::size_t i = 0; i < events.size(); ++i) {
        research.apply(events[i]);
        fast.apply(events[i]);
        if ((i + 1) % checkpoint_every == 0) {
            compare(research, fast, i, (i + 1) % full_every == 0);
        }
    }
    compare(research, fast, events.size(), true);

    // Capacity must never have been hit: a rejection would make the books
    // legitimately differ and quietly weaken every assertion above.
    CHECK(!fast.degraded());
    CHECK_EQ(fast.metrics().capacity_rejections, 0);

    std::printf(
        "  %-22s events=%zu orders_compared=%zu peak_orders=%zu peak_levels=%zu offgrid=%zu "
        "unknown=%llu clears=%llu\n",
        label, events.size(), compared_orders, fast.metrics().peak_orders,
        fast.metrics().peak_levels, fast.metrics().peak_offgrid,
        static_cast<unsigned long long>(fast.unknown_order_events()),
        static_cast<unsigned long long>(fast.clears_applied()));
    compared_orders = 0;
}

}  // namespace

int main() {
    BookConfig book_cfg;  // the shipped defaults

    {
        // The full mix: adds, cancels, deletes, executes, modifies, replaces,
        // clears, trades, unknown references, duplicate adds, zero quantities,
        // off-grid and out-of-window prices.
        GeneratorConfig cfg;
        cfg.event_count = 400000;
        run_stream(cfg, 20260831, "full mix", book_cfg);
    }
    {
        // Cancel-dominated, which is what a real ITCH day looks like.
        GeneratorConfig cfg;
        cfg.event_count = 250000;
        cfg.include_modify = false;
        cfg.include_replace = false;
        run_stream(cfg, 991, "cancel dominated", book_cfg);
    }
    {
        // A single price level, so every event lands in one FIFO queue and the
        // queue_ahead comparison is doing all the work.
        GeneratorConfig cfg;
        cfg.event_count = 120000;
        cfg.levels = 0;
        cfg.include_offgrid_prices = false;
        cfg.include_far_prices = false;
        run_stream(cfg, 12345, "single level", book_cfg);
    }
    {
        // A very wide book: 8000 ticks per side crosses many bitmap words.
        GeneratorConfig cfg;
        cfg.event_count = 250000;
        cfg.levels = 4000;
        run_stream(cfg, 777, "wide book", book_cfg);
    }
    {
        // A tiny tick window, so almost everything is forced through the
        // off-grid overflow path.
        BookConfig narrow;
        narrow.tick_count = 1024;                 // $0.00 - $10.23 on the grid
        narrow.base_price = 0;
        narrow.max_offgrid_levels = 4096;
        GeneratorConfig cfg;
        cfg.event_count = 150000;
        cfg.levels = 30;
        run_stream(cfg, 424242, "overflow heavy", narrow);
    }
    {
        // Clears every few thousand events, exercising teardown and reuse.
        GeneratorConfig cfg;
        cfg.event_count = 200000;
        run_stream(cfg, 5150, "with clears", book_cfg);
    }

    itch_test::pass("differential research vs low-latency");
    return 0;
}
