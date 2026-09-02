#pragma once

// Nasdaq TotalView-ITCH 5.0 binary parser, hot-path shaped.
//
// Constraints this file holds itself to:
//   * no heap allocation, no strings, no dynamic containers, no exceptions
//   * every field read is bounds-checked against the declared message length
//   * every multi-byte field goes through an explicit big-endian load
//   * decoding writes straight into a caller-owned MarketEvent - no
//     intermediate per-message objects, no copies of the packet payload
//
// The decoder is a single switch on the message type. The length table is a
// constexpr array so validating a message costs one load and one compare
// before any field is touched.

#include <cstddef>
#include <cstdint>

#include "itch_engine/byte_order.hpp"
#include "itch_engine/feed/itch_messages.hpp"
#include "itch_engine/market_event.hpp"

namespace itch {
namespace feed {

enum class ParseResult : std::uint8_t {
    Ok = 0,           // a MarketEvent was produced
    Ignored = 1,      // known message type that carries no MarketEvent
    BadLength = 2,    // length disagrees with the specification
    UnknownType = 3,  // message type not in ITCH 5.0
};

inline const char* to_string(ParseResult r) noexcept {
    switch (r) {
        case ParseResult::Ok: return "OK";
        case ParseResult::Ignored: return "IGNORED";
        case ParseResult::BadLength: return "BAD_LENGTH";
        case ParseResult::UnknownType: return "UNKNOWN_TYPE";
    }
    return "UNKNOWN";
}

struct ParserStats {
    std::uint64_t messages = 0;
    std::uint64_t events = 0;
    std::uint64_t ignored = 0;
    std::uint64_t bad_length = 0;
    std::uint64_t unknown_type = 0;
    std::uint64_t adds = 0;
    std::uint64_t executes = 0;
    std::uint64_t cancels = 0;
    std::uint64_t deletes = 0;
    std::uint64_t replaces = 0;
    std::uint64_t trades = 0;
    std::uint64_t system_events = 0;
};

namespace detail {
// One 256-entry table, filled at compile time from message_length(). Indexing
// it is a single load; the alternative (a switch on type just to learn the
// length) is a branch chain executed before every message.
struct LengthTable {
    std::uint8_t len[256];
    constexpr LengthTable() : len() {
        for (int i = 0; i < 256; ++i) {
            len[i] = message_length(static_cast<std::uint8_t>(i));
        }
    }
};
inline constexpr LengthTable kLengths{};
}  // namespace detail

// Configuration that cannot be derived from the wire.
struct ParserConfig {
    // ITCH timestamps are nanoseconds since midnight. Add the session's
    // midnight (ns since epoch) to get an absolute timestamp that lines up
    // with the historical parquet path. 0 leaves them relative.
    std::int64_t session_epoch_ns = 0;

    // 0 accepts every symbol; otherwise only this stock locate is decoded.
    // Filtering here rather than downstream keeps single-symbol replays from
    // paying for the rest of the feed.
    std::uint16_t stock_locate_filter = 0;

    // Nasdaq's 'S' with code 'O' (Start of Messages) is the one point in the
    // day where the book is known empty. The historical path spells the same
    // thing "Clear", so mapping it keeps both feeds producing one event
    // stream. Turn it off to see System Events verbatim.
    bool start_of_messages_is_clear = true;
};

class ItchParser {
public:
    ItchParser() = default;
    explicit ItchParser(const ParserConfig& cfg) : cfg_(cfg) {}

    const ParserConfig& config() const noexcept { return cfg_; }
    void set_config(const ParserConfig& cfg) noexcept { cfg_ = cfg; }
    const ParserStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = ParserStats{}; }

    // Decodes one ITCH message straight out of the packet buffer.
    // `msg` points into the receive buffer and is never retained.
    ParseResult parse(const std::uint8_t* msg, std::uint16_t len, MarketEvent& out) noexcept {
        ++stats_.messages;
        if (len < kItchHeaderLen) {
            ++stats_.bad_length;
            return ParseResult::BadLength;
        }

        const std::uint8_t type = msg[kOffType];
        const std::uint8_t expected = detail::kLengths.len[type];
        if (expected == 0) {
            ++stats_.unknown_type;
            return ParseResult::UnknownType;
        }
        if (len != expected) {
            ++stats_.bad_length;
            return ParseResult::BadLength;
        }

        const std::uint16_t locate = load_be16(msg + kOffStockLocate);
        if (cfg_.stock_locate_filter != 0 && locate != cfg_.stock_locate_filter &&
            type != kMsgSystemEvent) {
            ++stats_.ignored;
            return ParseResult::Ignored;
        }

        out.timestamp = cfg_.session_epoch_ns + static_cast<std::int64_t>(load_be48(msg + kOffTimestamp));
        out.stock_locate = locate;
        out.new_order_id = 0;

        switch (type) {
            case kMsgAddOrder:
            case kMsgAddOrderMpid: {
                out.type = EventType::Add;
                out.order_id = load_be64(msg + add_order::kOrderRef);
                out.side = msg[add_order::kBuySell] == 'B' ? Side::Bid : Side::Ask;
                out.quantity = load_be32(msg + add_order::kShares);
                out.price = static_cast<Price>(load_be32(msg + add_order::kPrice)) * ITCH_PRICE_UNIT;
                ++stats_.adds;
                ++stats_.events;
                return ParseResult::Ok;
            }
            case kMsgOrderExecuted:
            case kMsgOrderExecutedPrice: {
                // Price on 'C' is the execution print, not the resting order's
                // level, so the book must not take it: it looks the order up by
                // reference and fills at whatever price that order rests at.
                out.type = EventType::Execute;
                out.order_id = load_be64(msg + order_executed::kOrderRef);
                out.quantity = load_be32(msg + order_executed::kExecutedShares);
                out.price = 0;
                out.side = Side::Unknown;
                ++stats_.executes;
                ++stats_.events;
                return ParseResult::Ok;
            }
            case kMsgOrderCancel: {
                out.type = EventType::Cancel;
                out.order_id = load_be64(msg + order_cancel::kOrderRef);
                out.quantity = load_be32(msg + order_cancel::kCancelledShares);
                out.price = 0;
                out.side = Side::Unknown;
                ++stats_.cancels;
                ++stats_.events;
                return ParseResult::Ok;
            }
            case kMsgOrderDelete: {
                out.type = EventType::Delete;
                out.order_id = load_be64(msg + order_delete::kOrderRef);
                out.quantity = 0;
                out.price = 0;
                out.side = Side::Unknown;
                ++stats_.deletes;
                ++stats_.events;
                return ParseResult::Ok;
            }
            case kMsgOrderReplace: {
                out.type = EventType::Replace;
                out.order_id = load_be64(msg + order_replace::kOriginalOrderRef);
                out.new_order_id = load_be64(msg + order_replace::kNewOrderRef);
                out.quantity = load_be32(msg + order_replace::kShares);
                out.price = static_cast<Price>(load_be32(msg + order_replace::kPrice)) * ITCH_PRICE_UNIT;
                out.side = Side::Unknown;  // inherited from the resting order
                ++stats_.replaces;
                ++stats_.events;
                return ParseResult::Ok;
            }
            case kMsgTrade: {
                // Non-cross trade: a print against non-displayed liquidity. It
                // never touches the displayed book, which is why the historical
                // path drops it - but the live path surfaces it so a strategy
                // can see the tape.
                out.type = EventType::Trade;
                out.order_id = load_be64(msg + trade::kOrderRef);
                out.side = msg[trade::kBuySell] == 'B' ? Side::Bid : Side::Ask;
                out.quantity = load_be32(msg + trade::kShares);
                out.price = static_cast<Price>(load_be32(msg + trade::kPrice)) * ITCH_PRICE_UNIT;
                ++stats_.trades;
                ++stats_.events;
                return ParseResult::Ok;
            }
            case kMsgCrossTrade: {
                out.type = EventType::Trade;
                out.order_id = 0;
                out.side = Side::Unknown;
                // Cross shares are 64-bit on the wire; saturate rather than
                // silently truncate into the 32-bit quantity field.
                const std::uint64_t shares = load_be64(msg + cross_trade::kShares);
                out.quantity = shares > 0xFFFFFFFFull ? 0xFFFFFFFFu
                                                      : static_cast<std::uint32_t>(shares);
                out.price =
                    static_cast<Price>(load_be32(msg + cross_trade::kCrossPrice)) * ITCH_PRICE_UNIT;
                ++stats_.trades;
                ++stats_.events;
                return ParseResult::Ok;
            }
            case kMsgSystemEvent: {
                const std::uint8_t code = msg[system_event::kEventCode];
                out.order_id = 0;
                out.new_order_id = 0;
                out.quantity = code;  // event code, so the caller can branch on it
                out.price = 0;
                out.side = Side::Unknown;
                out.type = (cfg_.start_of_messages_is_clear && code == kSysStartOfMessages)
                               ? EventType::Clear
                               : EventType::SystemEvent;
                ++stats_.system_events;
                ++stats_.events;
                return ParseResult::Ok;
            }
            default:
                // Reference-data and auction messages are well-formed and
                // understood; they simply do not move per-order book state.
                ++stats_.ignored;
                return ParseResult::Ignored;
        }
    }

private:
    ParserConfig cfg_;
    ParserStats stats_;
};

}  // namespace feed
}  // namespace itch
