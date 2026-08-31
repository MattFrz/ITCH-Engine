#include "itch_engine/price_level.hpp"

#include <algorithm>
#include <cassert>

namespace itch {

PriceLevel::QueueIt PriceLevel::add(OrderId id, Qty qty) {
    assert(qty > 0);
    total_qty_ += qty;
    return queue_.insert(queue_.end(), RestingOrder{id, qty});
}

void PriceLevel::remove(QueueIt it) {
    total_qty_ -= it->qty;
    queue_.erase(it);
}

void PriceLevel::reduce(QueueIt it, Qty by) {
    assert(by > 0 && by <= it->qty);
    it->qty -= by;
    total_qty_ -= by;
    if (it->qty == 0) {
        queue_.erase(it);
    }
}

Qty PriceLevel::qty_ahead_of(QueueIt it) const {
    Qty ahead = 0;
    for (auto cur = queue_.begin(); cur != it; ++cur) {
        ahead += cur->qty;
    }
    return ahead;
}

}  // namespace itch
