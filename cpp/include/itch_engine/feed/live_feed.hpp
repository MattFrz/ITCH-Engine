#pragma once

// The live path, assembled:
//
//   NetworkBackend (UDP multicast / DPDK)
//        |  PacketView: a pointer into the receive buffer, no copy
//        v
//   MoldUDP64Decoder      sequence tracking, gap + duplicate detection
//        |  message view: a pointer inside the same buffer
//        v
//   ItchParser            fixed-width decode straight into...
//        v
//   MarketEvent  ->  OrderBook
//
// Interface-compatible with PacketReplayFeed on purpose: the same engine
// template (runtime/live_engine.hpp) drives either, so what runs against a
// capture is literally the same code that runs against the wire.

#include <cstdint>
#include <string>

#include "itch_engine/feed/itch_parser.hpp"
#include "itch_engine/feed/moldudp64.hpp"
#include "itch_engine/feed/network_backend.hpp"
#include "itch_engine/market_event.hpp"

namespace itch {
namespace feed {

struct LiveFeedConfig {
    // Absolute session midnight, ns since epoch, added to the ITCH
    // ns-since-midnight timestamps. Leave 0 to keep them relative.
    std::int64_t session_epoch_ns = 0;
    std::uint16_t stock_locate_filter = 0;

    // A live feed gets the safe default: an unrecoverable gap latches
    // RecoveryRequired and stops delivering rather than skipping ahead. Supply
    // a GapRecoveryPolicy for your venue's retransmission service, or accept
    // that the engine halts and re-snapshots.
    bool auto_resync = false;

    int receive_batch = 32;
    bool capture_timing = true;
};

inline constexpr std::size_t kLiveMaxEventsBuffered = 4096;
inline constexpr int kLiveMaxPacketsPerReceive = 64;

class LiveFeed {
public:
    LiveFeed() = default;

    // The backend is owned by the caller; it must already be open.
    void attach(NetworkBackend& backend, const LiveFeedConfig& cfg) {
        backend_ = &backend;
        cfg_ = cfg;
        ParserConfig pc;
        pc.session_epoch_ns = cfg.session_epoch_ns;
        pc.stock_locate_filter = cfg.stock_locate_filter;
        parser_.set_config(pc);
        decoder_.set_auto_resync(cfg.auto_resync);
        head_ = tail_ = 0;
    }

    void set_recovery_policy(GapRecoveryPolicy* p) noexcept { decoder_.set_recovery_policy(p); }

    void stop() noexcept { stopped_ = true; }
    bool stopped() const noexcept { return stopped_; }

    // Non-blocking: returns false when nothing is queued right now, which for
    // a live feed means "poll again", not "end of stream". Use `stopped()` to
    // tell the two apart.
    bool next_event(MarketEvent& event) {
        while (head_ == tail_) {
            if (stopped_) return false;
            if (!pump_()) return false;
        }
        event = pending_[head_];
        if (cfg_.capture_timing) timing_ = pending_timing_[head_];
        ++head_;
        return true;
    }

    // Blocks (spinning) until an event arrives or the feed is stopped. Spinning
    // rather than sleeping is the point of a busy-polled receiver; the caller
    // is expected to own the core.
    bool next_event_blocking(MarketEvent& event) {
        for (;;) {
            if (next_event(event)) return true;
            if (stopped_) return false;
        }
    }

    const MoldStats& mold_stats() const noexcept { return decoder_.stats(); }
    const ParserStats& parser_stats() const noexcept { return parser_.stats(); }
    const NetworkStats& network_stats() const {
        static const NetworkStats empty;
        return backend_ ? backend_->stats() : empty;
    }
    SessionState session_state() const noexcept { return decoder_.state(); }
    bool healthy() const noexcept { return decoder_.healthy(); }
    std::uint64_t first_missing_sequence() const noexcept {
        return decoder_.first_missing_sequence();
    }
    std::uint64_t missing_count() const noexcept { return decoder_.missing_count(); }
    const EventTiming& last_timing() const noexcept { return timing_; }
    std::uint64_t overflow_events() const noexcept { return overflow_events_; }

    // Explicit operator action after an unrecoverable gap.
    void resync(std::uint64_t next_sequence) noexcept { decoder_.resync(next_sequence); }

private:
    bool pump_() {
        if (backend_ == nullptr) return false;
        const int want = cfg_.receive_batch < kLiveMaxPacketsPerReceive ? cfg_.receive_batch
                                                                       : kLiveMaxPacketsPerReceive;
        const int got = backend_->receive(packets_, want);
        if (got <= 0) return false;

        head_ = tail_ = 0;
        MarketEvent ev;
        for (int i = 0; i < got; ++i) {
            const PacketView& pkt = packets_[i];
            decoder_.decode(pkt.data, pkt.len,
                            [&](std::uint64_t seq, const std::uint8_t* msg, std::uint16_t len) {
                                (void)seq;
                                if (tail_ >= kLiveMaxEventsBuffered) {
                                    ++overflow_events_;
                                    return;
                                }
                                if (parser_.parse(msg, len, ev) != ParseResult::Ok) return;
                                pending_[tail_] = ev;
                                if (cfg_.capture_timing) {
                                    EventTiming t;
                                    t.exchange_timestamp = static_cast<std::uint64_t>(ev.timestamp);
                                    t.receive_timestamp = pkt.hw_timestamp_ns != 0
                                                              ? pkt.hw_timestamp_ns
                                                              : pkt.sw_timestamp_ns;
                                    pending_timing_[tail_] = t;
                                }
                                ++tail_;
                            });
        }
        return tail_ > head_;
    }

    NetworkBackend* backend_ = nullptr;
    LiveFeedConfig cfg_;
    MoldUDP64Decoder decoder_;
    ItchParser parser_;

    PacketView packets_[kLiveMaxPacketsPerReceive];
    MarketEvent pending_[kLiveMaxEventsBuffered];
    EventTiming pending_timing_[kLiveMaxEventsBuffered];
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    EventTiming timing_;
    std::uint64_t overflow_events_ = 0;
    bool stopped_ = false;
};

}  // namespace feed
}  // namespace itch
