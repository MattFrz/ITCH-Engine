#pragma once

// The feed abstraction.
//
//   HistoricalFeed --+
//                    +--> MarketEvent --> OrderBook
//   LiveFeed --------+
//
// Two forms of the same contract, deliberately:
//
//   * A *static* one - any type with `bool next_event(MarketEvent&)`. This is
//     what the low-latency runtime uses (runtime/live_engine.hpp is a
//     template), so the compiler can inline the whole feed -> parse -> book
//     chain with no indirect call anywhere on the per-message path.
//
//   * A *virtual* one - MarketDataFeed below, for tools, tests and anything
//     that needs to hold "some feed" behind a pointer. FeedAdapter lifts any
//     static feed into it.
//
// Measured cost of the virtual form on this machine (bench_pipeline
// --dispatch), so the choice is a number and not a preference: see
// docs/low_latency_architecture.md. The virtual call is not free but it is
// small next to the book work; it is avoided in the runtime because it also
// blocks inlining of the decode loop, which is the larger effect.

#include "itch_engine/market_event.hpp"

namespace itch {
namespace feed {

// Virtual feed interface. Not used on the hot path.
class MarketDataFeed {
public:
    virtual ~MarketDataFeed() = default;

    // Fills `event` with the next market event. Returns false when the feed is
    // exhausted (historical) or has been stopped (live).
    virtual bool next_event(MarketEvent& event) = 0;
};

// Lifts a concrete feed (anything with next_event) into MarketDataFeed.
template <class Impl>
class FeedAdapter final : public MarketDataFeed {
public:
    explicit FeedAdapter(Impl& impl) : impl_(impl) {}
    bool next_event(MarketEvent& event) override { return impl_.next_event(event); }

private:
    Impl& impl_;
};

// A feed over a contiguous array of already-decoded events. This is the
// historical path expressed as a feed: normalize.py -> parquet -> array ->
// MarketEvent, with no packets involved. It is also the reference input for
// the differential tests, which need the same stream to reach both books.
class ArrayFeed {
public:
    ArrayFeed() = default;
    ArrayFeed(const MarketEvent* events, std::size_t count) : events_(events), count_(count) {}

    bool next_event(MarketEvent& event) noexcept {
        if (pos_ >= count_) return false;
        event = events_[pos_++];
        return true;
    }

    void rewind() noexcept { pos_ = 0; }
    std::size_t position() const noexcept { return pos_; }
    std::size_t size() const noexcept { return count_; }

private:
    const MarketEvent* events_ = nullptr;
    std::size_t count_ = 0;
    std::size_t pos_ = 0;
};

}  // namespace feed
}  // namespace itch
