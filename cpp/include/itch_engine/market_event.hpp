#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "itch_engine/types.hpp"

namespace itch {

// The single internal market-data event, produced by *both* feed paths:
//
//   HistoricalFeed (parquet -> MarketEvent)  --+
//                                              +--> MarketEvent --> OrderBook
//   LiveFeed (UDP -> MoldUDP64 -> ITCH)      --+
//
// Layout rationale (verified by the static_asserts below):
//
//   offset size field
//   ------ ---- --------------------------------------------------------
//        0    8 timestamp     exchange/message time, ns
//        8    8 order_id      ITCH order reference (Replace: the original)
//       16    8 new_order_id  Replace only; 0 otherwise
//       24    8 price         fixed point, 1e-9 USD
//       32    4 quantity      shares; uint32 covers ITCH's 32-bit share field
//       36    2 stock_locate  ITCH symbol index (0 for historical replay)
//       38    1 type
//       39    1 side
//   ------ ---- 40 bytes, alignment 8, no padding holes
//
// 40 bytes is the smallest layout that keeps Replace atomic. Dropping
// new_order_id would reach 32 (two per cache line), but the ITCH 'U' message
// carries two order references and the parser cannot split it into
// Delete+Add without knowing the resting order's side - which only the book
// knows. Paying 8 bytes here keeps the parser stateless, which is worth more
// than the packing; see docs/low_latency_architecture.md.
//
// Deliberately absent: strings, containers, pointers, timing fields. Timing
// lives in the parallel EventTiming struct so the hot path only pays for it
// when measurement is switched on.
struct MarketEvent {
    Timestamp timestamp = 0;
    OrderId order_id = 0;
    OrderId new_order_id = 0;
    Price price = 0;
    std::uint32_t quantity = 0;
    std::uint16_t stock_locate = 0;
    EventType type = EventType::Unknown;
    Side side = Side::Unknown;
};

static_assert(sizeof(MarketEvent) == 40, "MarketEvent must stay 40 bytes");
static_assert(alignof(MarketEvent) == 8, "MarketEvent must stay 8-byte aligned");
static_assert(std::is_trivially_copyable<MarketEvent>::value,
              "MarketEvent must be memcpy-able (it is written straight into ring buffers)");
static_assert(std::is_standard_layout<MarketEvent>::value,
              "MarketEvent must be standard layout (offsets are part of the ABI)");
static_assert(offsetof(MarketEvent, timestamp) == 0, "MarketEvent layout changed");
static_assert(offsetof(MarketEvent, order_id) == 8, "MarketEvent layout changed");
static_assert(offsetof(MarketEvent, new_order_id) == 16, "MarketEvent layout changed");
static_assert(offsetof(MarketEvent, price) == 24, "MarketEvent layout changed");
static_assert(offsetof(MarketEvent, quantity) == 32, "MarketEvent layout changed");
static_assert(offsetof(MarketEvent, stock_locate) == 36, "MarketEvent layout changed");

// Four clocks, deliberately not merged. Confusing an exchange timestamp with a
// local one is the classic way to publish latency numbers that mean nothing:
// exchange_timestamp is the venue's clock, everything else is this host's.
//
// Kept OUT of MarketEvent: carrying it inline would take the event from 40 to
// 72 bytes on every message. Feeds fill it only when timing is enabled.
struct EventTiming {
    std::uint64_t exchange_timestamp = 0;  // from the ITCH message body
    std::uint64_t receive_timestamp = 0;   // NIC/kernel/app receive of the packet
    std::uint64_t parsed_timestamp = 0;    // after ITCH decode into MarketEvent
    std::uint64_t book_timestamp = 0;      // after the book applied it
};

static_assert(sizeof(EventTiming) == 32, "EventTiming must stay 32 bytes");
static_assert(std::is_trivially_copyable<EventTiming>::value,
              "EventTiming must be trivially copyable");

// --- conversions between the legacy Event and MarketEvent -------------------
//
// The historical path (parquet -> normalize.py -> pybind) still speaks Event.
// These keep the two representations exactly interchangeable for the 0-4
// action set, so the research path is unchanged by the new one.

inline MarketEvent from_legacy(const Event& ev) noexcept {
    MarketEvent out;
    out.timestamp = ev.ts;
    out.order_id = ev.order_id;
    out.new_order_id = 0;
    out.price = ev.price;
    out.quantity = ev.qty <= 0 ? 0u : static_cast<std::uint32_t>(ev.qty);
    out.stock_locate = 0;
    out.type = ev.type;
    out.side = ev.side;
    return out;
}

// Lossy for Replace (which needs two legacy events) - callers that care use
// OrderBook::apply(const MarketEvent&) instead, which handles it natively.
inline Event to_legacy(const MarketEvent& ev) noexcept {
    Event out{};
    out.ts = ev.timestamp;
    out.order_id = ev.order_id;
    out.type = ev.type == EventType::Delete ? EventType::Cancel : ev.type;
    out.side = ev.side;
    out.price = ev.price;
    // Delete is a full removal, which the legacy book spells "cancel qty 0".
    out.qty = ev.type == EventType::Delete ? 0 : static_cast<Qty>(ev.quantity);
    return out;
}

// True for events that change per-order book state. Trade/SystemEvent/Unknown
// are observable but do not touch resting orders.
inline constexpr bool mutates_book(EventType t) noexcept {
    return t == EventType::Add || t == EventType::Cancel || t == EventType::Modify ||
           t == EventType::Execute || t == EventType::Clear || t == EventType::Delete ||
           t == EventType::Replace;
}

}  // namespace itch
