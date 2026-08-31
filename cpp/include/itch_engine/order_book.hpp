#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "itch_engine/price_level.hpp"
#include "itch_engine/types.hpp"

namespace itch {

struct LevelView {
    Price price;
    Qty qty;
    std::size_t order_count;
};

struct BestQuote {
    Price bid_price = 0;
    Qty bid_qty = 0;
    Price ask_price = 0;
    Qty ask_qty = 0;
    bool has_bid = false;
    bool has_ask = false;
};

// Limit order book reconstructed from a normalized MBO event stream.
//
// Layout:
//   - one std::map<Price, PriceLevel> per side (bids descending, asks
//     ascending), so best bid/ask is begin() and depth walks are ordered;
//   - a flat unordered_map<OrderId, OrderRef> index storing each order's
//     side, price, and its std::list iterator inside the level queue.
//
// The index + list-iterator combination is what makes cancel and execute of
// an arbitrary resting order O(1) (amortized): hash lookup, then list erase.
// This matters because cancels are the majority message type on a real ITCH
// day. See docs/data_structure_tradeoffs.md for the full argument.
class OrderBook {
public:
    // Applies one normalized event. Unknown order ids on Cancel/Modify/
    // Execute are counted (see unknown_order_events()) and skipped, which is
    // the standard tolerant stance for mid-session feed anomalies.
    void apply(const Event& ev);

    // Drops every resting order and price level. The venue emits this at
    // session start and when a halt resumes; without it, pre-halt orders
    // rest forever in the reconstruction.
    void clear();

    std::uint64_t events_processed() const { return events_processed_; }
    std::uint64_t clears_applied() const { return clears_applied_; }
    std::uint64_t unknown_order_events() const { return unknown_order_events_; }

    // --- state queries -----------------------------------------------------

    BestQuote best() const;
    std::optional<double> mid_price() const;  // in USD, not fixed-point

    // Aggregate quantity resting at an exact price (0 if level absent).
    Qty bid_qty_at(Price price) const;
    Qty ask_qty_at(Price price) const;

    // Top-N levels, best first.
    std::vector<LevelView> depth(Side side, std::size_t n) const;

    std::size_t bid_level_count() const { return bids_.size(); }
    std::size_t ask_level_count() const { return asks_.size(); }
    std::size_t open_order_count() const { return index_.size(); }

    // Remaining quantity of a live order, or nullopt if not resting.
    std::optional<Qty> order_qty(OrderId id) const;

    // Quantity queued ahead of a live order at its price level (FIFO
    // position). nullopt if the order is not resting. Not on the hot path.
    std::optional<Qty> queue_ahead(OrderId id) const;

private:
    struct OrderRef {
        Side side;
        Price price;
        PriceLevel::QueueIt it;
    };

    using BidMap = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskMap = std::map<Price, PriceLevel, std::less<Price>>;

    void add_order(const Event& ev);
    void cancel_order(const Event& ev, OrderRef& ref);
    void modify_order(const Event& ev, OrderRef& ref);
    void execute_order(const Event& ev, OrderRef& ref);

    PriceLevel& level_for(Side side, Price price);
    void erase_if_empty(Side side, Price price);

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<OrderId, OrderRef> index_;
    std::uint64_t events_processed_ = 0;
    std::uint64_t unknown_order_events_ = 0;
    std::uint64_t clears_applied_ = 0;
};

}  // namespace itch
