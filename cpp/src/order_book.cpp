#include "itch_engine/order_book.hpp"

#include <algorithm>

namespace itch {

void OrderBook::apply(const Event& ev) {
    ++events_processed_;

    if (ev.type == EventType::Add) {
        add_order(ev);
        return;
    }

    // Clear references no order id, so it must be handled before the index
    // lookup below (which would otherwise count it as an unknown order).
    if (ev.type == EventType::Clear) {
        clear();
        return;
    }

    auto found = index_.find(ev.order_id);
    if (found == index_.end()) {
        ++unknown_order_events_;
        return;
    }
    OrderRef& ref = found->second;

    switch (ev.type) {
        case EventType::Cancel:
            cancel_order(ev, ref);
            break;
        case EventType::Modify:
            modify_order(ev, ref);
            break;
        case EventType::Execute:
            execute_order(ev, ref);
            break;
        case EventType::Add:
        case EventType::Clear:
            break;  // handled above
    }
}

void OrderBook::apply(const MarketEvent& ev) {
    // Non-mutating actions (Trade prints, SystemEvent, Unknown) are counted as
    // seen but leave the book alone - matching the historical path, which drops
    // them at normalize time.
    if (!mutates_book(ev.type)) {
        return;
    }

    if (ev.type == EventType::Replace) {
        ++events_processed_;
        auto found = index_.find(ev.order_id);
        if (found == index_.end()) {
            ++unknown_order_events_;
            return;
        }
        replace_order(ev, found->second);
        return;
    }

    apply(to_legacy(ev));
}

// ITCH 'U': the original reference is removed outright and a fresh order is
// added at the back of the (possibly new) level. Side is inherited from the
// resting order, because the wire message does not carry one.
void OrderBook::replace_order(const MarketEvent& ev, OrderRef& ref) {
    const Side side = ref.side;
    const Price old_price = ref.price;

    level_for(side, old_price).remove(ref.it);
    index_.erase(ev.order_id);
    erase_if_empty(side, old_price);

    if (ev.quantity == 0) {
        return;  // replace-to-zero is just a delete
    }
    // A replace onto a live id would corrupt the index; treat as an anomaly.
    if (ev.new_order_id != ev.order_id && index_.count(ev.new_order_id) != 0) {
        ++unknown_order_events_;
        return;
    }
    PriceLevel& new_level = level_for(side, ev.price);
    auto it = new_level.add(ev.new_order_id, static_cast<Qty>(ev.quantity));
    index_.emplace(ev.new_order_id, OrderRef{side, ev.price, it});
}

void OrderBook::clear() {
    bids_.clear();
    asks_.clear();
    index_.clear();
    ++clears_applied_;
}

void OrderBook::add_order(const Event& ev) {
    if (ev.qty <= 0) {
        return;
    }
    // Duplicate add for a live id would corrupt the index; treat as anomaly.
    if (index_.count(ev.order_id) != 0) {
        ++unknown_order_events_;
        return;
    }
    PriceLevel& level = level_for(ev.side, ev.price);
    auto it = level.add(ev.order_id, ev.qty);
    index_.emplace(ev.order_id, OrderRef{ev.side, ev.price, it});
}

void OrderBook::cancel_order(const Event& ev, OrderRef& ref) {
    PriceLevel& level = level_for(ref.side, ref.price);
    // qty <= 0 or qty >= remaining means full cancel.
    const Qty remaining = ref.it->qty;
    const Qty reduce_by = (ev.qty <= 0 || ev.qty >= remaining) ? remaining : ev.qty;
    const bool fully_removed = reduce_by == remaining;

    level.reduce(ref.it, reduce_by);  // erases the node when it hits zero

    const Side side = ref.side;
    const Price price = ref.price;
    if (fully_removed) {
        index_.erase(ev.order_id);
    }
    erase_if_empty(side, price);
}

void OrderBook::modify_order(const Event& ev, OrderRef& ref) {
    const Price old_price = ref.price;
    const Side side = ref.side;
    const Qty old_qty = ref.it->qty;

    if (ev.price == old_price && ev.qty <= old_qty) {
        // Size-down at the same price keeps queue priority.
        if (ev.qty < old_qty) {
            level_for(side, old_price).reduce(ref.it, old_qty - ev.qty);
            if (ev.qty == 0) {
                index_.erase(ev.order_id);
                erase_if_empty(side, old_price);
            }
        }
        return;
    }

    // Price change or size-up: order goes to the back of the (new) queue.
    level_for(side, old_price).remove(ref.it);
    erase_if_empty(side, old_price);
    if (ev.qty <= 0) {
        index_.erase(ev.order_id);
        return;
    }
    PriceLevel& new_level = level_for(side, ev.price);
    ref.price = ev.price;
    ref.it = new_level.add(ev.order_id, ev.qty);
}

void OrderBook::execute_order(const Event& ev, OrderRef& ref) {
    PriceLevel& level = level_for(ref.side, ref.price);
    const Qty remaining = ref.it->qty;
    const Qty fill = std::min(std::max<Qty>(ev.qty, 0), remaining);
    if (fill <= 0) {
        return;
    }
    const Side side = ref.side;
    const Price price = ref.price;
    level.reduce(ref.it, fill);
    if (fill == remaining) {
        index_.erase(ev.order_id);
    }
    erase_if_empty(side, price);
}

PriceLevel& OrderBook::level_for(Side side, Price price) {
    if (side == Side::Bid) {
        return bids_[price];
    }
    return asks_[price];
}

void OrderBook::erase_if_empty(Side side, Price price) {
    if (side == Side::Bid) {
        auto it = bids_.find(price);
        if (it != bids_.end() && it->second.empty()) {
            bids_.erase(it);
        }
    } else {
        auto it = asks_.find(price);
        if (it != asks_.end() && it->second.empty()) {
            asks_.erase(it);
        }
    }
}

BestQuote OrderBook::best() const {
    BestQuote q;
    if (!bids_.empty()) {
        q.has_bid = true;
        q.bid_price = bids_.begin()->first;
        q.bid_qty = bids_.begin()->second.total_qty();
    }
    if (!asks_.empty()) {
        q.has_ask = true;
        q.ask_price = asks_.begin()->first;
        q.ask_qty = asks_.begin()->second.total_qty();
    }
    return q;
}

std::optional<double> OrderBook::mid_price() const {
    if (bids_.empty() || asks_.empty()) {
        return std::nullopt;
    }
    const double bid = static_cast<double>(bids_.begin()->first);
    const double ask = static_cast<double>(asks_.begin()->first);
    return (bid + ask) / 2.0 / static_cast<double>(PRICE_SCALE);
}

Qty OrderBook::bid_qty_at(Price price) const {
    auto it = bids_.find(price);
    return it == bids_.end() ? 0 : it->second.total_qty();
}

Qty OrderBook::ask_qty_at(Price price) const {
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.total_qty();
}

std::vector<LevelView> OrderBook::depth(Side side, std::size_t n) const {
    std::vector<LevelView> out;
    out.reserve(n);
    if (side == Side::Bid) {
        for (auto it = bids_.begin(); it != bids_.end() && out.size() < n; ++it) {
            out.push_back({it->first, it->second.total_qty(), it->second.order_count()});
        }
    } else {
        for (auto it = asks_.begin(); it != asks_.end() && out.size() < n; ++it) {
            out.push_back({it->first, it->second.total_qty(), it->second.order_count()});
        }
    }
    return out;
}

std::optional<Qty> OrderBook::order_qty(OrderId id) const {
    auto it = index_.find(id);
    if (it == index_.end()) {
        return std::nullopt;
    }
    return it->second.it->qty;
}

std::optional<Qty> OrderBook::queue_ahead(OrderId id) const {
    auto it = index_.find(id);
    if (it == index_.end()) {
        return std::nullopt;
    }
    const OrderRef& ref = it->second;
    const PriceLevel* level = nullptr;
    if (ref.side == Side::Bid) {
        auto lv = bids_.find(ref.price);
        if (lv != bids_.end()) level = &lv->second;
    } else {
        auto lv = asks_.find(ref.price);
        if (lv != asks_.end()) level = &lv->second;
    }
    if (level == nullptr) {
        return std::nullopt;
    }
    return level->qty_ahead_of(ref.it);
}

std::vector<OrderId> OrderBook::queue_at(Side side, Price price) const {
    std::vector<OrderId> out;
    const PriceLevel* level = nullptr;
    if (side == Side::Bid) {
        auto it = bids_.find(price);
        if (it != bids_.end()) level = &it->second;
    } else {
        auto it = asks_.find(price);
        if (it != asks_.end()) level = &it->second;
    }
    if (level == nullptr) {
        return out;
    }
    out.reserve(level->order_count());
    for (const RestingOrder& o : level->queue()) {
        out.push_back(o.id);
    }
    return out;
}

}  // namespace itch
