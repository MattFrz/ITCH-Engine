#pragma once

// Nasdaq TotalView-ITCH 5.0 wire layout.
//
// Every message begins with an 11-byte header:
//
//   offset size field
//   ------ ---- ------------------------------------------------------
//        0    1 MessageType
//        1    2 StockLocate       venue symbol index
//        3    2 TrackingNumber    internal to Nasdaq
//        5    6 Timestamp         nanoseconds since midnight, 48-bit
//
// Bodies start at offset 11. Sizes below are the total message length
// including the header, taken from the published specification. Fixed sizes
// are the whole reason a parser like this can be branch-light: length is a
// property of the type, not something to be discovered while decoding.
//
// Prices are uint32 in units of 1/10000 USD. Internally the engine uses
// 1e-9 USD, so an ITCH price scales by ITCH_PRICE_UNIT (100000).

#include <cstddef>
#include <cstdint>

namespace itch {
namespace feed {

inline constexpr std::size_t kItchHeaderLen = 11;

// Offsets inside the common header.
inline constexpr std::size_t kOffType = 0;
inline constexpr std::size_t kOffStockLocate = 1;
inline constexpr std::size_t kOffTrackingNumber = 3;
inline constexpr std::size_t kOffTimestamp = 5;

enum : std::uint8_t {
    kMsgSystemEvent = 'S',
    kMsgStockDirectory = 'R',
    kMsgTradingAction = 'H',
    kMsgRegSho = 'Y',
    kMsgParticipantPosition = 'L',
    kMsgMwcbDeclineLevel = 'V',
    kMsgMwcbStatus = 'W',
    kMsgIpoQuotingPeriod = 'K',
    kMsgLuldAuctionCollar = 'J',
    kMsgOperationalHalt = 'h',
    kMsgAddOrder = 'A',
    kMsgAddOrderMpid = 'F',
    kMsgOrderExecuted = 'E',
    kMsgOrderExecutedPrice = 'C',
    kMsgOrderCancel = 'X',
    kMsgOrderDelete = 'D',
    kMsgOrderReplace = 'U',
    kMsgTrade = 'P',
    kMsgCrossTrade = 'Q',
    kMsgBrokenTrade = 'B',
    kMsgNoii = 'I',
    kMsgRpii = 'N',
};

// Total message length by type, or 0 for types this build does not know.
// A lookup table beats a switch here: one load, no branch chain.
inline constexpr std::uint8_t message_length(std::uint8_t type) noexcept {
    return type == kMsgSystemEvent          ? 12
           : type == kMsgStockDirectory     ? 39
           : type == kMsgTradingAction      ? 25
           : type == kMsgRegSho             ? 20
           : type == kMsgParticipantPosition ? 26
           : type == kMsgMwcbDeclineLevel   ? 35
           : type == kMsgMwcbStatus         ? 12
           : type == kMsgIpoQuotingPeriod   ? 28
           : type == kMsgLuldAuctionCollar  ? 35
           : type == kMsgOperationalHalt    ? 21
           : type == kMsgAddOrder           ? 36
           : type == kMsgAddOrderMpid       ? 40
           : type == kMsgOrderExecuted      ? 31
           : type == kMsgOrderExecutedPrice ? 36
           : type == kMsgOrderCancel        ? 23
           : type == kMsgOrderDelete        ? 19
           : type == kMsgOrderReplace       ? 35
           : type == kMsgTrade              ? 44
           : type == kMsgCrossTrade         ? 40
           : type == kMsgBrokenTrade        ? 19
           : type == kMsgNoii               ? 50
           : type == kMsgRpii               ? 20
                                            : 0;
};

// System event codes (message 'S').
enum : std::uint8_t {
    kSysStartOfMessages = 'O',
    kSysStartOfSystemHours = 'S',
    kSysStartOfMarketHours = 'Q',
    kSysEndOfMarketHours = 'M',
    kSysEndOfSystemHours = 'E',
    kSysEndOfMessages = 'C',
};

// Body offsets, per message type.
namespace add_order {
inline constexpr std::size_t kOrderRef = 11;
inline constexpr std::size_t kBuySell = 19;
inline constexpr std::size_t kShares = 20;
inline constexpr std::size_t kStock = 24;
inline constexpr std::size_t kPrice = 32;
inline constexpr std::size_t kAttribution = 36;  // 'F' only
}  // namespace add_order

namespace order_executed {
inline constexpr std::size_t kOrderRef = 11;
inline constexpr std::size_t kExecutedShares = 19;
inline constexpr std::size_t kMatchNumber = 23;
inline constexpr std::size_t kPrintable = 31;      // 'C' only
inline constexpr std::size_t kExecutionPrice = 32; // 'C' only
}  // namespace order_executed

namespace order_cancel {
inline constexpr std::size_t kOrderRef = 11;
inline constexpr std::size_t kCancelledShares = 19;
}  // namespace order_cancel

namespace order_delete {
inline constexpr std::size_t kOrderRef = 11;
}  // namespace order_delete

namespace order_replace {
inline constexpr std::size_t kOriginalOrderRef = 11;
inline constexpr std::size_t kNewOrderRef = 19;
inline constexpr std::size_t kShares = 27;
inline constexpr std::size_t kPrice = 31;
}  // namespace order_replace

namespace trade {
inline constexpr std::size_t kOrderRef = 11;
inline constexpr std::size_t kBuySell = 19;
inline constexpr std::size_t kShares = 20;
inline constexpr std::size_t kStock = 24;
inline constexpr std::size_t kPrice = 32;
inline constexpr std::size_t kMatchNumber = 36;
}  // namespace trade

namespace cross_trade {
inline constexpr std::size_t kShares = 11;
inline constexpr std::size_t kStock = 19;
inline constexpr std::size_t kCrossPrice = 27;
inline constexpr std::size_t kMatchNumber = 31;
inline constexpr std::size_t kCrossType = 39;
}  // namespace cross_trade

namespace system_event {
inline constexpr std::size_t kEventCode = 11;
}  // namespace system_event

}  // namespace feed
}  // namespace itch
