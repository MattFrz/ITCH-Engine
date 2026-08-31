#pragma once

#include <cstdint>

namespace itch {

// Prices are fixed-point integers (Databento MBO convention: 1e-9 USD units).
// Never floats: float equality on prices is how books silently drift.
using Price = std::int64_t;
using Qty = std::int64_t;
using OrderId = std::uint64_t;
using Timestamp = std::int64_t;  // nanoseconds since epoch

inline constexpr Price PRICE_SCALE = 1'000'000'000;  // 1e-9 USD per unit

enum class Side : std::uint8_t {
    Bid = 0,
    Ask = 1,
};

// Normalized MBO event types (subset of ITCH actions relevant to book state).
enum class EventType : std::uint8_t {
    Add = 0,      // new resting order
    Cancel = 1,   // full or partial cancel (qty = amount removed)
    Modify = 2,   // price/qty replace; loses queue priority on price change or size-up
    Execute = 3,  // trade against resting order (fills from queue front)
    Clear = 4,    // venue wiped the book (session start, post-halt resume)
};

struct Event {
    Timestamp ts;
    OrderId order_id;
    EventType type;
    Side side;
    Price price;
    Qty qty;
};

}  // namespace itch
