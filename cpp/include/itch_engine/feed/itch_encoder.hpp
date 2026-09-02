#pragma once

// MarketEvent -> Nasdaq TotalView-ITCH 5.0 bytes.
//
// This is the *inverse* of itch_parser.hpp and exists for two reasons:
//
//   1. the historical packet simulator, which turns a normalized parquet day
//      into a real MoldUDP64/ITCH capture so the low-latency pipeline can be
//      developed and benchmarked without a live Nasdaq connection;
//   2. unit tests, which need to build exact messages by hand.
//
// It is NOT a hot path and does not pretend to be: it keeps a shadow map of
// resting orders because the ITCH wire, unlike the Databento normalization,
// does not carry a single "modify" action. A Databento 'M' has to become
// either 'X' (size-down at the same price, which keeps queue priority) or 'U'
// (reprice or size-up, which loses it), and only the resting order's current
// price and size decide which. Getting that split right is what makes the
// capture replay to byte-identical book state.

#include <cstdint>
#include <cstring>
#include <unordered_map>

#include "itch_engine/byte_order.hpp"
#include "itch_engine/feed/itch_messages.hpp"
#include "itch_engine/market_event.hpp"

namespace itch {
namespace feed {

struct EncoderStats {
    std::uint64_t events = 0;
    std::uint64_t messages = 0;
    std::uint64_t adds = 0;
    std::uint64_t cancels = 0;   // 'X'
    std::uint64_t deletes = 0;   // 'D'
    std::uint64_t executes = 0;  // 'E'
    std::uint64_t replaces = 0;  // 'U'
    std::uint64_t trades = 0;    // 'P'
    std::uint64_t system_events = 0;
    std::uint64_t skipped_unknown_modify = 0;
    std::uint64_t price_not_representable = 0;
    std::uint64_t qty_not_representable = 0;
};

struct EncoderConfig {
    std::int64_t session_epoch_ns = 0;  // subtracted to get ns-since-midnight
    std::uint16_t stock_locate = 1;
    char stock[8] = {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
};

class ItchEncoder {
public:
    ItchEncoder() = default;
    explicit ItchEncoder(const EncoderConfig& cfg) : cfg_(cfg) {}

    const EncoderStats& stats() const noexcept { return stats_; }
    const EncoderConfig& config() const noexcept { return cfg_; }

    void reset() {
        resting_.clear();
        stats_ = EncoderStats{};
    }

    // Encodes one event, invoking out(bytes, len) once per ITCH message.
    // Returns false if the event could not be represented on the wire (which
    // is counted in stats and never silently dropped).
    template <class Out>
    bool encode(const MarketEvent& ev, Out&& out) {
        ++stats_.events;
        switch (ev.type) {
            case EventType::Add: return encode_add(ev, out);
            case EventType::Cancel: return encode_cancel(ev, out);
            case EventType::Delete: return encode_delete(ev, out);
            case EventType::Execute: return encode_execute(ev, out);
            case EventType::Modify: return encode_modify(ev, out);
            case EventType::Replace: return encode_replace(ev, out);
            case EventType::Clear: return encode_system(ev, kSysStartOfMessages, out);
            case EventType::SystemEvent:
                return encode_system(ev, static_cast<std::uint8_t>(ev.quantity), out);
            case EventType::Trade: return encode_trade(ev, out);
            default: return false;
        }
    }

private:
    struct Resting {
        Price price;
        std::int64_t qty;
        Side side;
    };

    void header(std::uint8_t type, const MarketEvent& ev) noexcept {
        buf_[kOffType] = type;
        store_be16(buf_ + kOffStockLocate, cfg_.stock_locate);
        store_be16(buf_ + kOffTrackingNumber, 0);
        std::int64_t rel = ev.timestamp - cfg_.session_epoch_ns;
        if (rel < 0) rel = 0;
        store_be48(buf_ + kOffTimestamp, static_cast<std::uint64_t>(rel));
    }

    bool to_itch_price(Price p, std::uint32_t& out) noexcept {
        if (p < 0 || p % ITCH_PRICE_UNIT != 0) {
            ++stats_.price_not_representable;
            return false;
        }
        const Price units = p / ITCH_PRICE_UNIT;
        if (units > 0xFFFFFFFFll) {
            ++stats_.price_not_representable;
            return false;
        }
        out = static_cast<std::uint32_t>(units);
        return true;
    }

    template <class Out>
    void emit(std::uint8_t len, Out& out) {
        ++stats_.messages;
        out(buf_, len);
    }

    template <class Out>
    bool encode_add(const MarketEvent& ev, Out& out) {
        std::uint32_t px = 0;
        if (!to_itch_price(ev.price, px)) return false;
        if (ev.quantity == 0) return false;  // the book drops these anyway
        header(kMsgAddOrder, ev);
        store_be64(buf_ + add_order::kOrderRef, ev.order_id);
        buf_[add_order::kBuySell] = ev.side == Side::Bid ? 'B' : 'S';
        store_be32(buf_ + add_order::kShares, ev.quantity);
        std::memcpy(buf_ + add_order::kStock, cfg_.stock, 8);
        store_be32(buf_ + add_order::kPrice, px);
        emit(36, out);
        ++stats_.adds;
        // A duplicate add for a live reference is an anomaly both books ignore,
        // so the shadow must keep the ORIGINAL order - overwriting it here
        // would mis-encode a later modify against that reference.
        resting_.emplace(ev.order_id,
                         Resting{ev.price, static_cast<std::int64_t>(ev.quantity), ev.side});
        return true;
    }

    template <class Out>
    bool encode_cancel(const MarketEvent& ev, Out& out) {
        auto it = resting_.find(ev.order_id);
        const std::int64_t remaining = it == resting_.end() ? 0 : it->second.qty;
        // qty 0 or "at least everything left" is a full delete on the wire.
        if (ev.quantity == 0 || (it != resting_.end() &&
                                 static_cast<std::int64_t>(ev.quantity) >= remaining)) {
            return emit_delete(ev, out);
        }
        header(kMsgOrderCancel, ev);
        store_be64(buf_ + order_cancel::kOrderRef, ev.order_id);
        store_be32(buf_ + order_cancel::kCancelledShares, ev.quantity);
        emit(23, out);
        ++stats_.cancels;
        if (it != resting_.end()) {
            it->second.qty -= static_cast<std::int64_t>(ev.quantity);
            if (it->second.qty <= 0) resting_.erase(it);
        }
        return true;
    }

    template <class Out>
    bool emit_delete(const MarketEvent& ev, Out& out) {
        header(kMsgOrderDelete, ev);
        store_be64(buf_ + order_delete::kOrderRef, ev.order_id);
        emit(19, out);
        ++stats_.deletes;
        resting_.erase(ev.order_id);
        return true;
    }

    template <class Out>
    bool encode_delete(const MarketEvent& ev, Out& out) {
        return emit_delete(ev, out);
    }

    template <class Out>
    bool encode_execute(const MarketEvent& ev, Out& out) {
        header(kMsgOrderExecuted, ev);
        store_be64(buf_ + order_executed::kOrderRef, ev.order_id);
        store_be32(buf_ + order_executed::kExecutedShares, ev.quantity);
        store_be64(buf_ + order_executed::kMatchNumber, ++match_number_);
        emit(31, out);
        ++stats_.executes;
        auto it = resting_.find(ev.order_id);
        if (it != resting_.end()) {
            it->second.qty -= static_cast<std::int64_t>(ev.quantity);
            if (it->second.qty <= 0) resting_.erase(it);
        }
        return true;
    }

    // The one genuinely interesting case. See the file header.
    template <class Out>
    bool encode_modify(const MarketEvent& ev, Out& out) {
        auto it = resting_.find(ev.order_id);
        if (it == resting_.end()) {
            // Unknown reference: both books ignore it, so the faithful
            // encoding is a replace that will also be ignored.
            ++stats_.skipped_unknown_modify;
            return emit_replace(ev, ev.order_id, out);
        }
        const Resting before = it->second;
        if (ev.quantity == 0) {
            return emit_delete(ev, out);
        }
        if (ev.price == before.price && static_cast<std::int64_t>(ev.quantity) <= before.qty) {
            // Size-down at the same price keeps FIFO priority: that is 'X'.
            const std::uint32_t cancelled =
                static_cast<std::uint32_t>(before.qty - static_cast<std::int64_t>(ev.quantity));
            if (cancelled == 0) {
                return true;  // no-op modify; nothing goes on the wire
            }
            header(kMsgOrderCancel, ev);
            store_be64(buf_ + order_cancel::kOrderRef, ev.order_id);
            store_be32(buf_ + order_cancel::kCancelledShares, cancelled);
            emit(23, out);
            ++stats_.cancels;
            it->second.qty = static_cast<std::int64_t>(ev.quantity);
            return true;
        }
        // Reprice or size-up loses priority: that is 'U'.
        return emit_replace(ev, ev.order_id, out);
    }

    template <class Out>
    bool encode_replace(const MarketEvent& ev, Out& out) {
        return emit_replace(ev, ev.new_order_id, out);
    }

    template <class Out>
    bool emit_replace(const MarketEvent& ev, OrderId new_ref, Out& out) {
        std::uint32_t px = 0;
        if (!to_itch_price(ev.price, px)) return false;
        header(kMsgOrderReplace, ev);
        store_be64(buf_ + order_replace::kOriginalOrderRef, ev.order_id);
        store_be64(buf_ + order_replace::kNewOrderRef, new_ref);
        store_be32(buf_ + order_replace::kShares, ev.quantity);
        store_be32(buf_ + order_replace::kPrice, px);
        emit(35, out);
        ++stats_.replaces;

        auto it = resting_.find(ev.order_id);
        Side side = it == resting_.end() ? ev.side : it->second.side;
        if (it != resting_.end()) resting_.erase(it);
        if (ev.quantity > 0) {
            resting_[new_ref] = Resting{ev.price, static_cast<std::int64_t>(ev.quantity), side};
        }
        return true;
    }

    // 'P', a print against non-displayed liquidity. Carried so a capture is a
    // faithful recording of the feed, even though it never moves the book.
    template <class Out>
    bool encode_trade(const MarketEvent& ev, Out& out) {
        std::uint32_t px = 0;
        if (!to_itch_price(ev.price, px)) return false;
        header(kMsgTrade, ev);
        store_be64(buf_ + trade::kOrderRef, ev.order_id);
        buf_[trade::kBuySell] = ev.side == Side::Ask ? 'S' : 'B';
        store_be32(buf_ + trade::kShares, ev.quantity);
        std::memcpy(buf_ + trade::kStock, cfg_.stock, 8);
        store_be32(buf_ + trade::kPrice, px);
        store_be64(buf_ + trade::kMatchNumber, ++match_number_);
        emit(44, out);
        ++stats_.trades;
        return true;
    }

    template <class Out>
    bool encode_system(const MarketEvent& ev, std::uint8_t code, Out& out) {
        header(kMsgSystemEvent, ev);
        buf_[system_event::kEventCode] = code;
        emit(12, out);
        ++stats_.system_events;
        if (code == kSysStartOfMessages) {
            resting_.clear();
        }
        return true;
    }

    EncoderConfig cfg_;
    EncoderStats stats_;
    std::uint8_t buf_[64] = {};
    std::uint64_t match_number_ = 0;
    std::unordered_map<OrderId, Resting> resting_;
};

}  // namespace feed
}  // namespace itch
