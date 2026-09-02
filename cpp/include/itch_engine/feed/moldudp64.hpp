#pragma once

// MoldUDP64 transport layer.
//
// Wire format (all big-endian, no padding):
//
//   offset size field
//   ------ ---- -----------------------------------------------------------
//        0   10 Session          alphanumeric, space padded
//       10    8 SequenceNumber   sequence of the FIRST message block below
//       18    2 MessageCount     0 = heartbeat, 0xFFFF = end of session
//       20    - MessageBlock[]   each: uint16 length, then `length` bytes
//
// The decoder is allocation-free and template-dispatched on the handler, so
// the per-message path is a direct call with no indirect branch. Recovery is
// the one place that uses virtual dispatch, and it is by construction cold.
//
// Gap policy: a detected gap NEVER silently advances the sequence. Missing
// messages are either recovered through the injected policy, or the decoder
// latches RecoveryRequired and stops delivering, so a caller that ignores the
// state cannot corrupt its book. Resynchronising is an explicit decision the
// caller makes (resync()), or opt-in via set_auto_resync(true) for replay and
// development where no retransmission service exists.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "itch_engine/byte_order.hpp"

namespace itch {
namespace feed {

inline constexpr std::size_t kMoldSessionLen = 10;
inline constexpr std::size_t kMoldHeaderLen = 20;
inline constexpr std::uint16_t kMoldEndOfSession = 0xFFFF;

// Explicit, inspectable session state. The point of naming these is that a
// caller can branch on "am I allowed to trust the book right now".
enum class SessionState : std::uint8_t {
    Normal = 0,            // in sequence, book is trustworthy
    GapDetected = 1,       // a gap was seen and recovery is in progress
    RecoveryRequired = 2,  // gap could not be recovered; book is NOT trustworthy
    Recovered = 3,         // recovery (or an explicit resync) closed the gap
};

inline const char* to_string(SessionState s) noexcept {
    switch (s) {
        case SessionState::Normal: return "NORMAL";
        case SessionState::GapDetected: return "GAP_DETECTED";
        case SessionState::RecoveryRequired: return "RECOVERY_REQUIRED";
        case SessionState::Recovered: return "RECOVERED";
    }
    return "UNKNOWN";
}

enum class DecodeResult : std::uint8_t {
    Ok = 0,              // all messages in the packet were delivered
    Heartbeat = 1,       // message count 0
    Duplicate = 2,       // wholly or partly already seen; overlap suppressed
    Gap = 3,             // sequence ran ahead of expectation
    Blocked = 4,         // dropped because the decoder is in RecoveryRequired
    EndOfSession = 5,    // message count 0xFFFF
    SessionChanged = 6,  // different session id than the one being tracked
    Malformed = 7,       // truncated header/block, or block count mismatch
};

inline const char* to_string(DecodeResult r) noexcept {
    switch (r) {
        case DecodeResult::Ok: return "OK";
        case DecodeResult::Heartbeat: return "HEARTBEAT";
        case DecodeResult::Duplicate: return "DUPLICATE";
        case DecodeResult::Gap: return "GAP";
        case DecodeResult::Blocked: return "BLOCKED";
        case DecodeResult::EndOfSession: return "END_OF_SESSION";
        case DecodeResult::SessionChanged: return "SESSION_CHANGED";
        case DecodeResult::Malformed: return "MALFORMED";
    }
    return "UNKNOWN";
}

struct MoldStats {
    std::uint64_t packets = 0;
    std::uint64_t heartbeats = 0;
    std::uint64_t messages_delivered = 0;
    std::uint64_t duplicate_packets = 0;
    std::uint64_t duplicate_messages_suppressed = 0;
    std::uint64_t gap_events = 0;
    std::uint64_t messages_missed = 0;     // total sequence numbers skipped
    std::uint64_t messages_recovered = 0;  // supplied by the recovery policy
    std::uint64_t malformed_packets = 0;
    std::uint64_t blocked_packets = 0;  // dropped while RecoveryRequired
    std::uint64_t session_changes = 0;
    std::uint64_t end_of_session_packets = 0;
    std::uint64_t bytes = 0;
};

// Sink handed to a recovery policy so it can push retransmitted messages back
// into the same handler the live path uses. Virtual on purpose: recovery is a
// cold path and the indirection buys a clean injection point.
class MessageSink {
public:
    virtual ~MessageSink() = default;
    virtual void deliver(std::uint64_t sequence, const std::uint8_t* msg, std::uint16_t len) = 0;
};

// Injectable gap recovery. There is deliberately no built-in Nasdaq
// retransmission client here: the endpoints, entitlements and credentials for
// one are venue- and contract-specific, and inventing them would be worse than
// useless. Supply an implementation once you have an authorized feed.
class GapRecoveryPolicy {
public:
    virtual ~GapRecoveryPolicy() = default;

    // Fetch sequences [first_missing, first_missing + count) and hand them to
    // `sink` in order. Return true only if ALL of them were delivered.
    virtual bool recover(const char* session, std::uint64_t first_missing, std::uint64_t count,
                         MessageSink& sink) = 0;
};

class MoldUDP64Decoder {
public:
    MoldUDP64Decoder() { std::memset(session_, ' ', kMoldSessionLen); }

    void set_recovery_policy(GapRecoveryPolicy* policy) noexcept { recovery_ = policy; }

    // Replay and development have no retransmission service. With auto-resync
    // on, an unrecoverable gap is counted, the state moves to Recovered, and
    // the packet is delivered so a capture with holes still replays. It is off
    // by default because on a live feed that is exactly how a book silently
    // goes wrong.
    void set_auto_resync(bool on) noexcept { auto_resync_ = on; }
    bool auto_resync() const noexcept { return auto_resync_; }

    SessionState state() const noexcept { return state_; }
    bool healthy() const noexcept { return state_ != SessionState::RecoveryRequired; }
    std::uint64_t expected_sequence() const noexcept { return expected_; }
    std::uint64_t first_missing_sequence() const noexcept { return first_missing_; }
    std::uint64_t missing_count() const noexcept { return missing_count_; }
    const char* session() const noexcept { return session_; }
    const MoldStats& stats() const noexcept { return stats_; }

    // Explicit operator decision: abandon the missing messages and continue
    // from wherever the feed is now. Names what happened rather than hiding it.
    void resync(std::uint64_t next_sequence) noexcept {
        expected_ = next_sequence;
        first_missing_ = 0;
        missing_count_ = 0;
        state_ = SessionState::Recovered;
    }

    void reset() noexcept {
        expected_ = 0;
        have_session_ = false;
        first_missing_ = 0;
        missing_count_ = 0;
        state_ = SessionState::Normal;
        std::memset(session_, ' ', kMoldSessionLen);
    }

    // Decode one datagram. `on_message(sequence, payload, len)` is invoked once
    // per ITCH message block, in sequence order.
    template <class Handler>
    DecodeResult decode(const std::uint8_t* data, std::size_t len, Handler&& on_message) {
        ++stats_.packets;
        stats_.bytes += len;

        if (len < kMoldHeaderLen) {
            ++stats_.malformed_packets;
            return DecodeResult::Malformed;
        }

        const std::uint64_t sequence = load_be64(data + kMoldSessionLen);
        const std::uint16_t count = load_be16(data + kMoldSessionLen + 8);

        DecodeResult session_result = DecodeResult::Ok;
        if (!have_session_) {
            std::memcpy(session_, data, kMoldSessionLen);
            have_session_ = true;
            expected_ = sequence;
        } else if (std::memcmp(session_, data, kMoldSessionLen) != 0) {
            // A new session id means a new numbering space. Re-baselining is
            // the only correct response, and it is surfaced to the caller.
            std::memcpy(session_, data, kMoldSessionLen);
            expected_ = sequence;
            first_missing_ = 0;
            missing_count_ = 0;
            state_ = SessionState::Normal;
            ++stats_.session_changes;
            session_result = DecodeResult::SessionChanged;
        }

        if (count == kMoldEndOfSession) {
            ++stats_.end_of_session_packets;
            return DecodeResult::EndOfSession;
        }

        // Validate every block before delivering any of them: a half-applied
        // packet is worse than a rejected one.
        std::size_t offset = kMoldHeaderLen;
        for (std::uint16_t i = 0; i < count; ++i) {
            if (offset + 2 > len) {
                ++stats_.malformed_packets;
                return DecodeResult::Malformed;
            }
            const std::uint16_t msg_len = load_be16(data + offset);
            offset += 2;
            if (msg_len == 0 || offset + msg_len > len) {
                ++stats_.malformed_packets;
                return DecodeResult::Malformed;
            }
            offset += msg_len;
        }

        if (count == 0) {
            ++stats_.heartbeats;
            // A heartbeat carries the next expected sequence and never moves it.
            return session_result == DecodeResult::SessionChanged ? session_result
                                                                  : DecodeResult::Heartbeat;
        }

        if (state_ == SessionState::RecoveryRequired) {
            if (!auto_resync_) {
                ++stats_.blocked_packets;
                return DecodeResult::Blocked;
            }
            resync(sequence);
        }

        const std::uint64_t last = sequence + count;  // exclusive

        // Already seen in full.
        if (last <= expected_) {
            ++stats_.duplicate_packets;
            stats_.duplicate_messages_suppressed += count;
            return DecodeResult::Duplicate;
        }

        std::uint64_t skip = 0;
        DecodeResult result = session_result;
        if (sequence < expected_) {
            // Partial overlap after a retransmission: deliver only the tail.
            skip = expected_ - sequence;
            ++stats_.duplicate_packets;
            stats_.duplicate_messages_suppressed += skip;
            if (result == DecodeResult::Ok) result = DecodeResult::Duplicate;
        } else if (sequence > expected_) {
            const std::uint64_t missing = sequence - expected_;
            ++stats_.gap_events;
            stats_.messages_missed += missing;
            first_missing_ = expected_;
            missing_count_ = missing;
            state_ = SessionState::GapDetected;

            bool recovered = false;
            if (recovery_ != nullptr) {
                ForwardingSink<Handler> sink(on_message, stats_);
                recovered = recovery_->recover(session_, expected_, missing, sink);
            }
            if (recovered) {
                state_ = SessionState::Recovered;
                first_missing_ = 0;
                missing_count_ = 0;
                expected_ = sequence;
                if (result == DecodeResult::Ok) result = DecodeResult::Gap;
            } else if (auto_resync_) {
                state_ = SessionState::Recovered;
                first_missing_ = 0;
                missing_count_ = 0;
                expected_ = sequence;
                deliver_(data, sequence, count, 0, on_message);
                state_ = SessionState::Normal;
                return DecodeResult::Gap;
            } else {
                // Fail safe: hold the messages back and make the caller decide.
                state_ = SessionState::RecoveryRequired;
                ++stats_.blocked_packets;
                return DecodeResult::Gap;
            }
        }

        deliver_(data, sequence, count, skip, on_message);
        if (state_ == SessionState::Recovered && missing_count_ == 0) {
            state_ = SessionState::Normal;
        }
        return result;
    }

private:
    template <class Handler>
    class ForwardingSink : public MessageSink {
    public:
        ForwardingSink(Handler& h, MoldStats& stats) : h_(h), stats_(stats) {}
        void deliver(std::uint64_t sequence, const std::uint8_t* msg, std::uint16_t len) override {
            ++stats_.messages_recovered;
            ++stats_.messages_delivered;
            h_(sequence, msg, len);
        }

    private:
        Handler& h_;
        MoldStats& stats_;
    };

    template <class Handler>
    void deliver_(const std::uint8_t* data, std::uint64_t sequence, std::uint16_t count,
                  std::uint64_t skip, Handler& on_message) {
        std::size_t offset = kMoldHeaderLen;
        std::uint64_t seq = sequence;
        for (std::uint16_t i = 0; i < count; ++i) {
            const std::uint16_t msg_len = load_be16(data + offset);
            offset += 2;
            if (i >= skip) {
                ++stats_.messages_delivered;
                on_message(seq, data + offset, msg_len);
            }
            offset += msg_len;
            ++seq;
        }
        expected_ = sequence + count;
    }

    char session_[kMoldSessionLen];
    bool have_session_ = false;
    bool auto_resync_ = false;
    std::uint64_t expected_ = 0;
    std::uint64_t first_missing_ = 0;
    std::uint64_t missing_count_ = 0;
    SessionState state_ = SessionState::Normal;
    GapRecoveryPolicy* recovery_ = nullptr;
    MoldStats stats_;
};

// --- packet building (replay simulator + tests) -----------------------------

// Packs ITCH messages into MoldUDP64 datagrams. Used by the historical packet
// simulator and by the unit tests; never on the receive path.
class MoldUDP64Packer {
public:
    MoldUDP64Packer(const char* session, std::uint64_t first_sequence, std::size_t mtu = 1400)
        : sequence_(first_sequence), mtu_(mtu) {
        std::memset(session_, ' ', kMoldSessionLen);
        for (std::size_t i = 0; i < kMoldSessionLen && session[i] != '\0'; ++i) {
            session_[i] = session[i];
        }
    }

    std::size_t mtu() const noexcept { return mtu_; }
    std::uint64_t next_sequence() const noexcept { return sequence_; }
    std::uint16_t pending_messages() const noexcept { return count_; }

    // Returns true if the message would not fit and the caller should flush()
    // first. Caller-driven flushing keeps this allocation-free.
    bool would_overflow(std::uint16_t msg_len) const noexcept {
        if (!open_) return false;
        return used_ + 2u + msg_len > mtu_ || count_ >= kMoldEndOfSession - 1;
    }

    void append(const std::uint8_t* msg, std::uint16_t msg_len) noexcept {
        if (!open_) start_packet();
        store_be16(buf_ + used_, msg_len);
        std::memcpy(buf_ + used_ + 2, msg, msg_len);
        used_ += 2u + msg_len;
        ++count_;
    }

    // Finalises the current datagram and returns its length (0 if empty). The
    // returned pointer stays valid until the next append(), which is what
    // starts the following packet: rewinding the buffer here instead would
    // hand the caller a pointer to a header that had already been overwritten.
    std::size_t flush(const std::uint8_t** out) noexcept {
        if (!open_ || count_ == 0) {
            *out = nullptr;
            return 0;
        }
        store_be16(buf_ + kMoldSessionLen + 8, count_);
        *out = buf_;
        sequence_ += count_;
        open_ = false;
        return used_;
    }

    // An end-of-session datagram (count 0xFFFF) as the venue sends at close.
    std::size_t end_of_session(const std::uint8_t** out) noexcept {
        start_packet();
        store_be16(buf_ + kMoldSessionLen + 8, kMoldEndOfSession);
        open_ = false;
        *out = buf_;
        return kMoldHeaderLen;
    }

private:
    void start_packet() noexcept {
        std::memcpy(buf_, session_, kMoldSessionLen);
        store_be64(buf_ + kMoldSessionLen, sequence_);
        store_be16(buf_ + kMoldSessionLen + 8, 0);
        used_ = kMoldHeaderLen;
        count_ = 0;
        open_ = true;
    }

    char session_[kMoldSessionLen];
    std::uint64_t sequence_;
    std::size_t mtu_;
    std::uint8_t buf_[65536];
    std::size_t used_ = 0;
    std::uint16_t count_ = 0;
    bool open_ = false;
};

}  // namespace feed
}  // namespace itch
