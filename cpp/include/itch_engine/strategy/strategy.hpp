#pragma once

// Strategy interface.
//
// Deliberately observation-only at this stage. There is no order submission
// from a strategy anywhere in this build: the order path exists as an
// interface plus a simulated implementation (execution/order_gateway.hpp) and
// a risk layer in front of it (risk/risk.hpp), and nothing connects any of it
// to a venue. Live trading is not a default anything should have.
//
// Two dispatch forms, same reason as the feed:
//   * static - any type with on_market_update(const MarketState&). The engine
//     template calls it directly and the compiler inlines it into the book
//     update, so an empty strategy really does cost nothing.
//   * virtual - Strategy below, for tools and tests.

#include <cstdint>

#include "itch_engine/market_event.hpp"
#include "itch_engine/order_book.hpp"  // BestQuote

namespace itch {
namespace strategy {

// What a strategy is shown after each event. A view, not a copy of the book:
// `book` points at the live book so a strategy that needs depth can ask for
// it, while the common case (top of book) is already materialised.
struct MarketState {
    const MarketEvent* event = nullptr;  // the event just applied
    BestQuote quote;                     // top of book after applying it
    const EventTiming* timing = nullptr; // nullptr when timing is disabled
    std::uint64_t event_index = 0;
    bool book_healthy = true;            // false after an unrecovered gap
};

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual void on_market_update(const MarketState& state) = 0;
};

// Costs nothing and compiles away entirely under the static form. Used as the
// benchmark's baseline so "with a strategy" and "without" are comparable.
struct NullStrategy {
    void on_market_update(const MarketState&) noexcept {}
};

// A minimal observing strategy: counts events and top-of-book changes and
// tracks the widest spread seen. Enough to prove the wiring and to give the
// benchmark something that actually reads the quote.
class QuoteObserver {
public:
    void on_market_update(const MarketState& state) noexcept {
        ++events_;
        const BestQuote& q = state.quote;
        if (q.has_bid && q.has_ask) {
            if (q.bid_price != last_bid_ || q.ask_price != last_ask_) {
                ++quote_changes_;
                last_bid_ = q.bid_price;
                last_ask_ = q.ask_price;
                const Price spread = q.ask_price - q.bid_price;
                if (spread > widest_spread_) widest_spread_ = spread;
                if (spread <= 0) ++crossed_or_locked_;
            }
        }
    }

    std::uint64_t events() const noexcept { return events_; }
    std::uint64_t quote_changes() const noexcept { return quote_changes_; }
    std::uint64_t crossed_or_locked() const noexcept { return crossed_or_locked_; }
    Price widest_spread() const noexcept { return widest_spread_; }
    Price last_bid() const noexcept { return last_bid_; }
    Price last_ask() const noexcept { return last_ask_; }

private:
    std::uint64_t events_ = 0;
    std::uint64_t quote_changes_ = 0;
    std::uint64_t crossed_or_locked_ = 0;
    Price widest_spread_ = 0;
    Price last_bid_ = 0;
    Price last_ask_ = 0;
};

// Lifts a static strategy into the virtual interface.
template <class Impl>
class StrategyAdapter final : public Strategy {
public:
    explicit StrategyAdapter(Impl& impl) : impl_(impl) {}
    void on_market_update(const MarketState& state) override { impl_.on_market_update(state); }

private:
    Impl& impl_;
};

}  // namespace strategy
}  // namespace itch
