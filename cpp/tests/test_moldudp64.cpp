// MoldUDP64 transport tests.
//
// The thing being tested is not "can it read a header" but the gap policy:
// a sequence gap must never silently advance the stream, duplicates must be
// suppressed exactly (including partial overlap after a retransmission), and
// a malformed packet must be rejected whole rather than half-applied.

#include <cstring>
#include <string>
#include <vector>

#include "itch_engine/feed/moldudp64.hpp"
#include "test_support.hpp"

using namespace itch;
using namespace itch::feed;

namespace {

struct Collected {
    std::vector<std::uint64_t> sequences;
    std::vector<std::string> payloads;

    void operator()(std::uint64_t seq, const std::uint8_t* msg, std::uint16_t len) {
        sequences.push_back(seq);
        payloads.emplace_back(reinterpret_cast<const char*>(msg), len);
    }
    void clear() {
        sequences.clear();
        payloads.clear();
    }
};

// Builds a datagram by hand so the tests do not depend on the packer being
// correct as well.
std::vector<std::uint8_t> make_packet(const char* session, std::uint64_t seq,
                                      const std::vector<std::string>& msgs,
                                      int forced_count = -1) {
    std::vector<std::uint8_t> p(kMoldHeaderLen, 0);
    std::memset(p.data(), ' ', kMoldSessionLen);
    for (std::size_t i = 0; i < kMoldSessionLen && session[i] != '\0'; ++i) {
        p[i] = static_cast<std::uint8_t>(session[i]);
    }
    store_be64(p.data() + 10, seq);
    const std::uint16_t count = forced_count >= 0 ? static_cast<std::uint16_t>(forced_count)
                                                  : static_cast<std::uint16_t>(msgs.size());
    store_be16(p.data() + 18, count);
    for (const std::string& m : msgs) {
        std::uint8_t len[2];
        store_be16(len, static_cast<std::uint16_t>(m.size()));
        p.push_back(len[0]);
        p.push_back(len[1]);
        p.insert(p.end(), m.begin(), m.end());
    }
    return p;
}

void test_header_and_delivery() {
    MoldUDP64Decoder d;
    Collected got;
    const auto pkt = make_packet("SESSION01", 1, {"abc", "de", "fghi"});
    CHECK(d.decode(pkt.data(), pkt.size(), got) == DecodeResult::Ok);
    CHECK_EQ(got.sequences.size(), 3);
    CHECK_EQ(got.sequences[0], 1);
    CHECK_EQ(got.sequences[1], 2);
    CHECK_EQ(got.sequences[2], 3);
    CHECK(got.payloads[0] == "abc");
    CHECK(got.payloads[2] == "fghi");
    CHECK_EQ(d.expected_sequence(), 4);
    CHECK(d.state() == SessionState::Normal);
    CHECK_EQ(d.stats().messages_delivered, 3);
    CHECK(std::memcmp(d.session(), "SESSION01 ", 10) == 0);
}

void test_heartbeat_does_not_move_sequence() {
    MoldUDP64Decoder d;
    Collected got;
    const auto p1 = make_packet("S", 10, {"x"});
    CHECK(d.decode(p1.data(), p1.size(), got) == DecodeResult::Ok);
    CHECK_EQ(d.expected_sequence(), 11);

    const auto hb = make_packet("S", 11, {});
    CHECK(d.decode(hb.data(), hb.size(), got) == DecodeResult::Heartbeat);
    CHECK_EQ(d.expected_sequence(), 11);
    CHECK_EQ(d.stats().heartbeats, 1);
    CHECK_EQ(got.sequences.size(), 1);
}

void test_end_of_session() {
    MoldUDP64Decoder d;
    Collected got;
    const auto p = make_packet("S", 1, {}, 0xFFFF);
    CHECK(d.decode(p.data(), p.size(), got) == DecodeResult::EndOfSession);
    CHECK_EQ(d.stats().end_of_session_packets, 1);
}

void test_full_duplicate_is_suppressed() {
    MoldUDP64Decoder d;
    Collected got;
    const auto p = make_packet("S", 1, {"a", "b"});
    CHECK(d.decode(p.data(), p.size(), got) == DecodeResult::Ok);
    CHECK_EQ(got.sequences.size(), 2);

    // The exact same datagram again: a retransmission or a duplicated path.
    CHECK(d.decode(p.data(), p.size(), got) == DecodeResult::Duplicate);
    CHECK_EQ(got.sequences.size(), 2);  // nothing new delivered
    CHECK_EQ(d.stats().duplicate_packets, 1);
    CHECK_EQ(d.stats().duplicate_messages_suppressed, 2);
    CHECK_EQ(d.expected_sequence(), 3);
}

// A retransmission that overlaps what has already been seen must deliver only
// the tail. Delivering the overlap would double-apply orders to the book.
void test_partial_overlap_delivers_only_the_tail() {
    MoldUDP64Decoder d;
    Collected got;
    const auto p1 = make_packet("S", 1, {"a", "b"});
    CHECK(d.decode(p1.data(), p1.size(), got) == DecodeResult::Ok);
    got.clear();

    const auto p2 = make_packet("S", 2, {"b", "c", "d"});
    CHECK(d.decode(p2.data(), p2.size(), got) == DecodeResult::Duplicate);
    CHECK_EQ(got.sequences.size(), 2);
    CHECK_EQ(got.sequences[0], 3);
    CHECK(got.payloads[0] == "c");
    CHECK(got.payloads[1] == "d");
    CHECK_EQ(d.expected_sequence(), 5);
    CHECK_EQ(d.stats().duplicate_messages_suppressed, 1);
}

// The headline behaviour: an unrecoverable gap must NOT deliver the packet and
// must NOT advance the sequence. Anything else silently corrupts the book.
void test_gap_without_recovery_fails_safe() {
    MoldUDP64Decoder d;
    Collected got;
    const auto p1 = make_packet("S", 1, {"a"});
    CHECK(d.decode(p1.data(), p1.size(), got) == DecodeResult::Ok);
    got.clear();

    const auto p2 = make_packet("S", 5, {"e"});  // 2,3,4 missing
    CHECK(d.decode(p2.data(), p2.size(), got) == DecodeResult::Gap);
    CHECK_EQ(got.sequences.size(), 0);
    CHECK(d.state() == SessionState::RecoveryRequired);
    CHECK(!d.healthy());
    CHECK_EQ(d.first_missing_sequence(), 2);
    CHECK_EQ(d.missing_count(), 3);
    CHECK_EQ(d.stats().gap_events, 1);
    CHECK_EQ(d.stats().messages_missed, 3);

    // And it stays latched: later packets are blocked, not quietly applied.
    const auto p3 = make_packet("S", 6, {"f"});
    CHECK(d.decode(p3.data(), p3.size(), got) == DecodeResult::Blocked);
    CHECK_EQ(got.sequences.size(), 0);
    CHECK(d.state() == SessionState::RecoveryRequired);

    // Only an explicit operator decision resumes it.
    d.resync(6);
    CHECK(d.state() == SessionState::Recovered);
    CHECK(d.decode(p3.data(), p3.size(), got) == DecodeResult::Ok);
    CHECK_EQ(got.sequences.size(), 1);
    CHECK(d.state() == SessionState::Normal);
}

void test_auto_resync_is_opt_in() {
    MoldUDP64Decoder d;
    d.set_auto_resync(true);
    Collected got;
    const auto p1 = make_packet("S", 1, {"a"});
    CHECK(d.decode(p1.data(), p1.size(), got) == DecodeResult::Ok);
    const auto p2 = make_packet("S", 5, {"e"});
    CHECK(d.decode(p2.data(), p2.size(), got) == DecodeResult::Gap);
    // With auto-resync the packet IS delivered, the loss is still counted, and
    // the caller can see it in the stats.
    CHECK_EQ(got.sequences.size(), 2);
    CHECK_EQ(got.sequences[1], 5);
    CHECK_EQ(d.stats().messages_missed, 3);
    CHECK(d.healthy());
}

// An injected recovery policy closes the gap and its messages are delivered
// in sequence, before the packet that exposed the gap.
class FakeRecovery : public GapRecoveryPolicy {
public:
    explicit FakeRecovery(bool succeed) : succeed_(succeed) {}
    bool recover(const char*, std::uint64_t first, std::uint64_t count,
                 MessageSink& sink) override {
        ++calls;
        if (!succeed_) return false;
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::string payload = "r" + std::to_string(first + i);
            sink.deliver(first + i, reinterpret_cast<const std::uint8_t*>(payload.data()),
                         static_cast<std::uint16_t>(payload.size()));
        }
        return true;
    }
    int calls = 0;

private:
    bool succeed_;
};

void test_recovery_policy_success() {
    MoldUDP64Decoder d;
    FakeRecovery rec(true);
    d.set_recovery_policy(&rec);
    Collected got;

    const auto p1 = make_packet("S", 1, {"a"});
    CHECK(d.decode(p1.data(), p1.size(), got) == DecodeResult::Ok);
    got.clear();

    const auto p2 = make_packet("S", 4, {"d"});  // 2,3 missing
    CHECK(d.decode(p2.data(), p2.size(), got) == DecodeResult::Gap);
    CHECK_EQ(rec.calls, 1);
    CHECK_EQ(got.sequences.size(), 3);
    CHECK(got.payloads[0] == "r2");
    CHECK(got.payloads[1] == "r3");
    CHECK(got.payloads[2] == "d");
    CHECK_EQ(d.stats().messages_recovered, 2);
    CHECK(d.healthy());
    CHECK(d.state() == SessionState::Normal);
}

void test_recovery_policy_failure_latches() {
    MoldUDP64Decoder d;
    FakeRecovery rec(false);
    d.set_recovery_policy(&rec);
    Collected got;
    const auto p1 = make_packet("S", 1, {"a"});
    CHECK(d.decode(p1.data(), p1.size(), got) == DecodeResult::Ok);
    const auto p2 = make_packet("S", 4, {"d"});
    CHECK(d.decode(p2.data(), p2.size(), got) == DecodeResult::Gap);
    CHECK_EQ(rec.calls, 1);
    CHECK_EQ(got.sequences.size(), 1);  // only the first packet's message
    CHECK(d.state() == SessionState::RecoveryRequired);
}

void test_malformed_packets_are_rejected_whole() {
    MoldUDP64Decoder d;
    Collected got;

    // Too short for a header.
    std::uint8_t tiny[10] = {0};
    CHECK(d.decode(tiny, sizeof(tiny), got) == DecodeResult::Malformed);

    // Count says 3, only 2 blocks present.
    auto p = make_packet("S", 1, {"aa", "bb"}, 3);
    CHECK(d.decode(p.data(), p.size(), got) == DecodeResult::Malformed);
    CHECK_EQ(got.sequences.size(), 0);  // nothing half-applied

    // A block length that runs past the end of the datagram.
    auto bad = make_packet("S", 1, {"aa"});
    store_be16(bad.data() + kMoldHeaderLen, 9000);
    CHECK(d.decode(bad.data(), bad.size(), got) == DecodeResult::Malformed);
    CHECK_EQ(got.sequences.size(), 0);

    // A zero-length block is not a valid message.
    auto zero = make_packet("S", 1, {"aa"});
    store_be16(zero.data() + kMoldHeaderLen, 0);
    CHECK(d.decode(zero.data(), zero.size(), got) == DecodeResult::Malformed);

    CHECK_EQ(d.stats().malformed_packets, 4);
    // A rejected packet must not have moved the sequence.
    CHECK_EQ(d.expected_sequence(), 1);
}

void test_session_change_rebaselines() {
    MoldUDP64Decoder d;
    Collected got;
    const auto p1 = make_packet("SESSION_A", 100, {"a"});
    CHECK(d.decode(p1.data(), p1.size(), got) == DecodeResult::Ok);
    CHECK_EQ(d.expected_sequence(), 101);

    // A different session id is a different numbering space; treating 5 as a
    // duplicate of the old stream would be wrong.
    const auto p2 = make_packet("SESSION_B", 5, {"b"});
    CHECK(d.decode(p2.data(), p2.size(), got) == DecodeResult::SessionChanged);
    CHECK_EQ(d.expected_sequence(), 6);
    CHECK_EQ(got.sequences.size(), 2);
    CHECK_EQ(d.stats().session_changes, 1);
}

// The packer and the decoder must agree, since the replay simulator uses one
// to feed the other.
void test_packer_round_trip() {
    MoldUDP64Packer packer("CAPTURE1", 1, 64);
    MoldUDP64Decoder d;
    Collected got;

    std::vector<std::string> sent;
    for (int i = 0; i < 40; ++i) {
        const std::string msg = "msg" + std::to_string(i) + std::string(i % 7, 'x');
        sent.push_back(msg);
        const std::uint16_t len = static_cast<std::uint16_t>(msg.size());
        if (packer.would_overflow(len)) {
            const std::uint8_t* out = nullptr;
            const std::size_t n = packer.flush(&out);
            CHECK(n > 0);
            CHECK(d.decode(out, n, got) == DecodeResult::Ok);
        }
        packer.append(reinterpret_cast<const std::uint8_t*>(msg.data()), len);
    }
    const std::uint8_t* out = nullptr;
    const std::size_t n = packer.flush(&out);
    if (n > 0) CHECK(d.decode(out, n, got) == DecodeResult::Ok);

    CHECK_EQ(got.payloads.size(), sent.size());
    for (std::size_t i = 0; i < sent.size(); ++i) {
        CHECK(got.payloads[i] == sent[i]);
        CHECK_EQ(got.sequences[i], i + 1);
    }
    CHECK_EQ(d.stats().gap_events, 0);
    CHECK_EQ(d.stats().duplicate_packets, 0);
}

}  // namespace

int main() {
    test_header_and_delivery();
    test_heartbeat_does_not_move_sequence();
    test_end_of_session();
    test_full_duplicate_is_suppressed();
    test_partial_overlap_delivers_only_the_tail();
    test_gap_without_recovery_fails_safe();
    test_auto_resync_is_opt_in();
    test_recovery_policy_success();
    test_recovery_policy_failure_latches();
    test_malformed_packets_are_rejected_whole();
    test_session_change_rebaselines();
    test_packer_round_trip();
    itch_test::pass("moldudp64");
    return 0;
}
