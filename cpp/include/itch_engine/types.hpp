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

// Nasdaq ITCH quotes prices as uint32 in 1/10000 USD. One ITCH price unit is
// therefore 100'000 internal units.
inline constexpr Price ITCH_PRICE_UNIT = PRICE_SCALE / 10'000;  // 100'000

enum class Side : std::uint8_t {
    Bid = 0,      // aka Buy
    Ask = 1,      // aka Sell
    Unknown = 2,  // messages with no side (Clear, SystemEvent, some Trades)
};

// Normalized event types.
//
// 0-4 are the original Databento-MBO-derived set and are a wire contract with
// python/itch_engine/ingest/normalize.py and every events.parquet already on
// disk: their numeric values must not change. 5+ are the additional
// ITCH-native actions the binary parser produces; the research book maps them
// onto the original set (Delete -> full Cancel, Replace -> Cancel + Add).
enum class EventType : std::uint8_t {
    Add = 0,      // new resting order
    Cancel = 1,   // partial or full cancel (qty = amount removed; 0 = full)
    Modify = 2,   // Databento 'M': same order id, new price/qty
    Execute = 3,  // trade against a resting order (fills from queue front)
    Clear = 4,    // venue wiped the book (session start, post-halt resume)

    Delete = 5,       // ITCH 'D': remove the whole resting order
    Replace = 6,      // ITCH 'U': delete order_id, add new_order_id at price/qty
    Trade = 7,        // ITCH 'P'/'Q': print with no book impact
    SystemEvent = 8,  // ITCH 'S': session state transition

    Unknown = 255,
};

// Legacy per-event struct used by the research order book and the pybind11
// bindings. Kept byte-identical to the original definition so nothing built
// on top of it changes; new code should prefer MarketEvent (market_event.hpp),
// which both feed paths produce.
struct Event {
    Timestamp ts;
    OrderId order_id;
    EventType type;
    Side side;
    Price price;
    Qty qty;
};

}  // namespace itch
