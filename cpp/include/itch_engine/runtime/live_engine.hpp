#pragma once

// The engine: feed -> book -> strategy, on one thread.
//
// Threading model, stated as a decision rather than an omission: this is
// single-threaded and stays that way until a measurement says otherwise. A
// pipeline that hands events between a receive thread and a book thread pays
// a cache-line transfer and a queue round trip per handoff - tens to hundreds
// of nanoseconds - to overlap work that takes about the same. It buys
// throughput on a saturated link and costs end-to-end latency, which is the
// wrong trade for a book that already keeps up with the feed. There are no
// mutexes here, no shared mutable state, and no queues, because none of them
// earned their place. When one does, the benchmark will say so and an SPSC
// ring is the first thing to try.
//
// Dispatch is static throughout: Feed, Book and Strategy are template
// parameters, so the whole loop inlines into one function and a NullStrategy
// costs literally nothing.

#include <cstdint>

#include "itch_engine/market_event.hpp"
#include "itch_engine/runtime/cycles.hpp"
#include "itch_engine/runtime/latency_recorder.hpp"
#include "itch_engine/strategy/strategy.hpp"

namespace itch {
namespace runtime {

struct EngineConfig {
    // Feed the strategy after every event. Off makes the engine a pure
    // feed-to-book measurement.
    bool notify_strategy = true;

    // Fill EventTiming and record per-event latency. Off by default: the
    // measurement is worth its cost in a benchmark and not in production.
    bool measure = false;

    // Stop after this many events (0 = run to the end of the feed).
    std::uint64_t max_events = 0;
};

struct EngineStats {
    std::uint64_t events = 0;
    std::uint64_t book_events = 0;      // events that mutated the book
    std::uint64_t strategy_calls = 0;
    std::uint64_t unhealthy_events = 0; // delivered while the feed was degraded
    std::uint64_t start_cycles = 0;
    std::uint64_t end_cycles = 0;
};

// `Feed` needs next_event(MarketEvent&) and healthy().
// `Book` needs apply(const MarketEvent&) and best().
// `Strategy` needs on_market_update(const strategy::MarketState&).
template <class Feed, class Book, class Strategy>
class Engine {
public:
    Engine(Feed& feed, Book& book, Strategy& strategy)
        : feed_(feed), book_(book), strategy_(strategy) {}

    void set_config(const EngineConfig& cfg) noexcept { cfg_ = cfg; }
    const EngineConfig& config() const noexcept { return cfg_; }

    // Optional per-event latency recorders. Left null, the engine does not
    // touch the timestamp counter at all.
    void set_recorders(LatencyRecorder* book_apply, LatencyRecorder* end_to_end) noexcept {
        rec_book_ = book_apply;
        rec_total_ = end_to_end;
    }

    const EngineStats& stats() const noexcept { return stats_; }

    EngineStats run() {
        MarketEvent ev;
        stats_ = EngineStats{};
        stats_.start_cycles = cycles_begin();

        strategy::MarketState state;
        while (feed_.next_event(ev)) {
            const std::uint64_t t0 = cfg_.measure ? cycles_begin() : 0;

            const bool mutating = mutates_book(ev.type);
            book_.apply(ev);
            if (mutating) ++stats_.book_events;

            const std::uint64_t t1 = cfg_.measure ? cycles_end() : 0;
            if (rec_book_ != nullptr && cfg_.measure) rec_book_->record_delta(t0, t1);

            if (cfg_.notify_strategy) {
                state.event = &ev;
                state.quote = book_.best();
                state.event_index = stats_.events;
                state.book_healthy = feed_.healthy();
                state.timing = nullptr;
                strategy_.on_market_update(state);
                ++stats_.strategy_calls;
            }

            if (rec_total_ != nullptr && cfg_.measure) rec_total_->record_delta(t0, cycles_end());

            if (!feed_.healthy()) ++stats_.unhealthy_events;

            ++stats_.events;
            if (cfg_.max_events != 0 && stats_.events >= cfg_.max_events) break;
        }

        stats_.end_cycles = cycles_end();
        return stats_;
    }

private:
    Feed& feed_;
    Book& book_;
    Strategy& strategy_;
    EngineConfig cfg_;
    EngineStats stats_;
    LatencyRecorder* rec_book_ = nullptr;
    LatencyRecorder* rec_total_ = nullptr;
};

template <class Feed, class Book, class Strategy>
Engine<Feed, Book, Strategy> make_engine(Feed& f, Book& b, Strategy& s) {
    return Engine<Feed, Book, Strategy>(f, b, s);
}

}  // namespace runtime
}  // namespace itch
