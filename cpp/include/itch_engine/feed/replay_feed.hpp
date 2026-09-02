#pragma once

// Historical packet-level replay: the whole live pipeline, minus the network.
//
//   capture (.itchcap)
//        |  same bytes a UDP socket would deliver
//        v
//   MoldUDP64Decoder  ->  ItchParser  ->  MarketEvent  ->  OrderBook
//
// This is what makes the low-latency path developable and benchmarkable
// without a live Nasdaq connection or any market-data entitlement. The only
// component it replaces is the socket; every layer above it is the same code
// the live feed runs.
//
// Pacing:
//   MaxSpeed  - as fast as the machine goes; the mode benchmarks use
//   Original  - reproduces the original inter-packet gaps
//   Scaled    - Original divided by `speed` (2.0 = twice as fast)

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "itch_engine/feed/capture.hpp"
#include "itch_engine/feed/itch_parser.hpp"
#include "itch_engine/feed/moldudp64.hpp"
#include "itch_engine/market_event.hpp"

namespace itch {
namespace feed {

enum class ReplayPacing : std::uint8_t {
    MaxSpeed = 0,
    Original = 1,
    Scaled = 2,
};

struct ReplayConfig {
    ReplayPacing pacing = ReplayPacing::MaxSpeed;
    double speed = 1.0;  // Scaled only; >1 is faster than the original day

    // A capture is a complete, in-order recording: there is no retransmission
    // service to ask, so an injected gap (the gap tests do inject some) is
    // resynchronised rather than latching RecoveryRequired forever. On a live
    // feed this defaults the other way - see moldudp64.hpp.
    bool auto_resync = true;

    std::uint16_t stock_locate_filter = 0;
    bool capture_timing = false;  // fill EventTiming per event
};

// Upper bound on messages buffered from one datagram. The smallest ITCH
// message is 19 bytes plus a 2-byte block length, so a 1500-byte Ethernet MTU
// tops out around 70; 1024 leaves a wide margin without making this object
// large enough to matter for stack placement. Datagrams carrying more are
// truncated and counted (`overflow_packets()`), never silently accepted.
inline constexpr std::size_t kMaxMessagesPerPacket = 1024;

class PacketReplayFeed {
public:
    bool open(const std::string& path, const ReplayConfig& cfg, std::string* error = nullptr) {
        cfg_ = cfg;
        if (!reader_.open(path, error)) return false;
        ParserConfig pc;
        pc.session_epoch_ns = reader_.header().session_epoch_ns;
        pc.stock_locate_filter = cfg.stock_locate_filter;
        parser_.set_config(pc);
        decoder_.set_auto_resync(cfg.auto_resync);
        started_ = false;
        head_ = tail_ = 0;
        return true;
    }

    void set_recovery_policy(GapRecoveryPolicy* p) noexcept { decoder_.set_recovery_policy(p); }

    // The static feed contract. Returns false at end of capture.
    bool next_event(MarketEvent& event) {
        while (head_ == tail_) {
            if (!fill_()) return false;
        }
        event = pending_[head_++];
        if (cfg_.capture_timing) {
            timing_ = pending_timing_[head_ - 1];
            timing_.parsed_timestamp = now_ns_();
        }
        return true;
    }

    const CaptureHeader& header() const noexcept { return reader_.header(); }
    const MoldStats& mold_stats() const noexcept { return decoder_.stats(); }
    const ParserStats& parser_stats() const noexcept { return parser_.stats(); }
    SessionState session_state() const noexcept { return decoder_.state(); }
    bool healthy() const noexcept { return decoder_.healthy(); }
    std::uint64_t first_missing_sequence() const noexcept {
        return decoder_.first_missing_sequence();
    }
    std::uint64_t missing_count() const noexcept { return decoder_.missing_count(); }
    std::uint64_t expected_sequence() const noexcept { return decoder_.expected_sequence(); }
    const EventTiming& last_timing() const noexcept { return timing_; }
    std::uint64_t packets_read() const noexcept { return packets_; }
    std::uint64_t overflow_packets() const noexcept { return overflow_packets_; }

    void rewind() {
        reader_.rewind();
        decoder_.reset();
        decoder_.set_auto_resync(cfg_.auto_resync);
        parser_.reset_stats();
        head_ = tail_ = 0;
        packets_ = 0;
        overflow_packets_ = 0;
        started_ = false;
    }

private:
    static std::uint64_t now_ns_() noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    void pace_(std::int64_t capture_ts_ns) {
        if (cfg_.pacing == ReplayPacing::MaxSpeed) return;
        const double speed = cfg_.pacing == ReplayPacing::Scaled && cfg_.speed > 0.0 ? cfg_.speed
                                                                                     : 1.0;
        if (!started_) {
            base_capture_ns_ = capture_ts_ns;
            base_real_ns_ = now_ns_();
            started_ = true;
            return;
        }
        const double delta = static_cast<double>(capture_ts_ns - base_capture_ns_) / speed;
        const std::uint64_t target = base_real_ns_ + static_cast<std::uint64_t>(delta);
        for (;;) {
            const std::uint64_t now = now_ns_();
            if (now >= target) return;
            const std::uint64_t remaining = target - now;
            // Sleep only while there is real time to give back; the last
            // ~50 us is spun so the wake-up jitter does not land in the
            // measurement.
            if (remaining > 50000) {
                std::this_thread::sleep_for(std::chrono::nanoseconds(remaining - 50000));
            }
        }
    }

    bool fill_() {
        const std::uint8_t* data = nullptr;
        std::size_t len = 0;
        std::int64_t capture_ts = 0;
        if (!reader_.next(data, len, capture_ts)) return false;
        ++packets_;
        pace_(capture_ts);

        const std::uint64_t recv_ns = cfg_.capture_timing ? now_ns_() : 0;
        head_ = tail_ = 0;
        MarketEvent ev;
        decoder_.decode(data, len, [&](std::uint64_t seq, const std::uint8_t* msg,
                                       std::uint16_t msg_len) {
            (void)seq;
            if (tail_ >= kMaxMessagesPerPacket) {
                ++overflow_packets_;
                return;
            }
            if (parser_.parse(msg, msg_len, ev) != ParseResult::Ok) return;
            pending_[tail_] = ev;
            if (cfg_.capture_timing) {
                EventTiming t;
                t.exchange_timestamp = static_cast<std::uint64_t>(ev.timestamp);
                t.receive_timestamp = recv_ns;
                pending_timing_[tail_] = t;
            }
            ++tail_;
        });
        return true;
    }

    CaptureReader reader_;
    MoldUDP64Decoder decoder_;
    ItchParser parser_;
    ReplayConfig cfg_;

    MarketEvent pending_[kMaxMessagesPerPacket];
    EventTiming pending_timing_[kMaxMessagesPerPacket];
    std::size_t head_ = 0;
    std::size_t tail_ = 0;

    EventTiming timing_;
    std::uint64_t packets_ = 0;
    std::uint64_t overflow_packets_ = 0;
    bool started_ = false;
    std::int64_t base_capture_ns_ = 0;
    std::uint64_t base_real_ns_ = 0;
};

}  // namespace feed
}  // namespace itch
