#pragma once

#include <list>

#include "itch_engine/types.hpp"

namespace itch {

// One resting order in the FIFO queue at a price level.
struct RestingOrder {
    OrderId id;
    Qty qty;
};

// A single price level: aggregate quantity plus a FIFO queue of orders.
//
// The queue is a std::list so that cancel/modify of an order in the middle of
// the queue is O(1) given a stored iterator (list iterators are stable under
// erase of other elements). A std::vector would make cancel-in-the-middle
// O(N) per event, which dominates on real ITCH days where the large majority
// of messages are cancels. See docs/data_structure_tradeoffs.md.
class PriceLevel {
public:
    using QueueIt = std::list<RestingOrder>::iterator;

    // Appends an order to the back of the queue. Returns its iterator, which
    // the book stores in its order index for O(1) cancel later.
    QueueIt add(OrderId id, Qty qty);

    // Removes an order entirely given its iterator. O(1).
    void remove(QueueIt it);

    // Reduces an order in place (partial cancel). Queue position is kept:
    // shrinking an order never costs it priority. O(1).
    void reduce(QueueIt it, Qty by);

    Qty total_qty() const { return total_qty_; }
    std::size_t order_count() const { return queue_.size(); }
    bool empty() const { return queue_.empty(); }

    // Quantity queued ahead of the given order (strictly before it in FIFO).
    // O(position) — used only by analysis queries, never on the hot path.
    Qty qty_ahead_of(QueueIt it) const;

    const std::list<RestingOrder>& queue() const { return queue_; }

private:
    std::list<RestingOrder> queue_;
    Qty total_qty_ = 0;
};

}  // namespace itch
