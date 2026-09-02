// End-to-end pipeline equivalence.
//
//   MarketEvent stream ---------------------------> research OrderBook   (A)
//          |
//          +-> ITCH encode -> MoldUDP64 packets -+-> research OrderBook  (B)
//                                                +-> LowLatencyOrderBook (C)
//
// A is the historical path this repository already validated. B and C go
// through the real transport: bytes on the wire, sequence numbers, packet
// framing, binary decode. All three must end up with the same book.
//
// That equivalence is what lets the low-latency path be developed and
// benchmarked against recorded data and still be the code that will run
// against a live feed - the only thing the capture replaces is the socket.
//
// Also covered here: capture file round trip, injected packet loss (the book
// must be held back, not corrupted), injected duplicate packets (suppressed),
// and reordered packets.

#include <cstdio>
#include <string>
#include <vector>

#include "itch_engine/book/low_latency_book.hpp"
#include "itch_engine/feed/capture.hpp"
#include "itch_engine/feed/itch_encoder.hpp"
#include "itch_engine/feed/itch_parser.hpp"
#include "itch_engine/feed/moldudp64.hpp"
#include "itch_engine/feed/replay_feed.hpp"
#include "itch_engine/order_book.hpp"
#include "itch_engine/runtime/live_engine.hpp"
#include "test_support.hpp"

using namespace itch;
using namespace itch::book;
using namespace itch::feed;

namespace {

using Datagram = std::vector<std::uint8_t>;

// Encodes an event stream into MoldUDP64 datagrams, exactly as the historical
// packet simulator does.
std::vector<Datagram> build_packets(const std::vector<MarketEvent>& events,
                                    std::size_t mtu = 1400,
                                    EncoderStats* out_stats = nullptr) {
    std::vector<Datagram> packets;
    EncoderConfig ecfg;
    ecfg.session_epoch_ns = 1'700'000'000'000'000'000LL;
    ecfg.stock_locate = 1;
    ItchEncoder enc(ecfg);
    MoldUDP64Packer packer("TESTCAP", 1, mtu);

    for (const MarketEvent& ev : events) {
        enc.encode(ev, [&](const std::uint8_t* msg, std::uint8_t len) {
            if (packer.would_overflow(len)) {
                const std::uint8_t* out = nullptr;
                const std::size_t n = packer.flush(&out);
                if (n > 0) packets.emplace_back(out, out + n);
            }
            packer.append(msg, len);
        });
    }
    const std::uint8_t* out = nullptr;
    const std::size_t n = packer.flush(&out);
    if (n > 0) packets.emplace_back(out, out + n);
    if (out_stats) *out_stats = enc.stats();
    return packets;
}

template <class BookA, class BookB>
void compare_state(const BookA& a, const BookB& b, const char* label) {
    const BestQuote qa = a.best();
    const BestQuote qb = b.best();
    CHECK_EQ(qb.has_bid, qa.has_bid);
    CHECK_EQ(qb.has_ask, qa.has_ask);
    CHECK_EQ(qb.bid_price, qa.bid_price);
    CHECK_EQ(qb.bid_qty, qa.bid_qty);
    CHECK_EQ(qb.ask_price, qa.ask_price);
    CHECK_EQ(qb.ask_qty, qa.ask_qty);
    CHECK_EQ(b.open_order_count(), a.open_order_count());
    CHECK_EQ(b.bid_level_count(), a.bid_level_count());
    CHECK_EQ(b.ask_level_count(), a.ask_level_count());

    for (Side side : {Side::Bid, Side::Ask}) {
        const std::size_t levels =
            side == Side::Bid ? a.bid_level_count() : a.ask_level_count();
        const auto da = a.depth(side, levels);
        const auto db = b.depth(side, levels);
        CHECK_EQ(db.size(), da.size());
        for (std::size_t i = 0; i < da.size(); ++i) {
            if (da[i].price != db[i].price || da[i].qty != db[i].qty ||
                da[i].order_count != db[i].order_count) {
                std::fprintf(stderr, "%s: level %zu differs: (%lld,%lld,%zu) vs (%lld,%lld,%zu)\n",
                             label, i, static_cast<long long>(da[i].price),
                             static_cast<long long>(da[i].qty), da[i].order_count,
                             static_cast<long long>(db[i].price),
                             static_cast<long long>(db[i].qty), db[i].order_count);
                std::exit(1);
            }
            // Exact FIFO order, not just aggregates.
            const auto qa2 = a.queue_at(side, da[i].price);
            const auto qb2 = b.queue_at(side, db[i].price);
            if (qa2 != qb2) {
                std::fprintf(stderr, "%s: FIFO order differs at price %lld\n", label,
                             static_cast<long long>(da[i].price));
                std::exit(1);
            }
            ++itch_test::checks_run;
        }
    }
}

// Sized for the test streams rather than for a full session, so several of
// these can be alive at once without the test needing a gigabyte.
BookConfig test_book_config() {
    BookConfig cfg;
    cfg.max_orders = 1u << 18;
    cfg.index_capacity = 1u << 19;
    cfg.max_levels = 1u << 14;
    return cfg;
}

struct PipelineResult {
    PipelineResult() : fast(test_book_config()) {}
    OrderBook research;
    LowLatencyOrderBook fast;
    MoldStats mold;
    ParserStats parser;
    SessionState state = SessionState::Normal;
};

void run_packets(const std::vector<Datagram>& packets, PipelineResult& result,
                 bool auto_resync = false) {
    MoldUDP64Decoder decoder;
    decoder.set_auto_resync(auto_resync);
    ParserConfig pcfg;
    pcfg.session_epoch_ns = 1'700'000'000'000'000'000LL;
    ItchParser parser(pcfg);

    MarketEvent ev;
    for (const Datagram& p : packets) {
        decoder.decode(p.data(), p.size(),
                       [&](std::uint64_t, const std::uint8_t* msg, std::uint16_t len) {
                           if (parser.parse(msg, len, ev) != ParseResult::Ok) return;
                           result.research.apply(ev);
                           result.fast.apply(ev);
                       });
    }
    result.mold = decoder.stats();
    result.parser = parser.stats();
    result.state = decoder.state();
}

// The main equivalence test.
void test_capture_replay_equivalence() {
    itch_test::GeneratorConfig gcfg;
    gcfg.event_count = 250000;
    const auto events = itch_test::generate_events(gcfg, 31082026);

    // (A) the historical path: events straight into the research book.
    OrderBook direct;
    for (const MarketEvent& ev : events) direct.apply(ev);

    EncoderStats estats;
    const auto packets = build_packets(events, 1400, &estats);
    CHECK(packets.size() > 100);
    // The generator only produces ITCH-representable prices, so nothing may
    // have been dropped for being unencodable.
    CHECK_EQ(estats.price_not_representable, 0);

    PipelineResult r;
    run_packets(packets, r);
    CHECK(r.state == SessionState::Normal);
    CHECK_EQ(r.mold.gap_events, 0);
    CHECK_EQ(r.mold.duplicate_packets, 0);
    CHECK_EQ(r.mold.malformed_packets, 0);
    CHECK_EQ(r.mold.messages_delivered, estats.messages);

    // (B) vs (C): the two books on the wire path must agree on everything,
    // counters included, because they saw exactly the same events.
    compare_state(r.research, r.fast, "wire research vs wire low-latency");
    CHECK_EQ(r.fast.events_processed(), r.research.events_processed());
    CHECK_EQ(r.fast.unknown_order_events(), r.research.unknown_order_events());
    CHECK_EQ(r.fast.clears_applied(), r.research.clears_applied());

    // (A) vs (C): the historical path and the wire path must reach the same
    // book. Event counters legitimately differ - a zero-quantity add and a
    // no-op modify are events in the parquet stream and simply are not
    // messages on the wire - but no observable book state may.
    compare_state(direct, r.fast, "historical vs wire");
    CHECK_EQ(r.fast.unknown_order_events(), direct.unknown_order_events());
    CHECK_EQ(r.fast.clears_applied(), direct.clears_applied());
    CHECK(!r.fast.degraded());

    std::printf("  equivalence: %zu events -> %zu messages in %zu datagrams (%.1f msgs/packet)\n",
                events.size(), static_cast<std::size_t>(estats.messages), packets.size(),
                static_cast<double>(estats.messages) / static_cast<double>(packets.size()));
}

// A dropped datagram must stop the book rather than corrupt it.
void test_dropped_packet_holds_the_book_back() {
    itch_test::GeneratorConfig gcfg;
    gcfg.event_count = 20000;
    const auto events = itch_test::generate_events(gcfg, 4242);
    auto packets = build_packets(events);
    CHECK(packets.size() > 20);

    // Snapshot the state after the first 9 packets: after the 10th goes
    // missing, no later packet may change the book at all.
    std::vector<Datagram> prefix(packets.begin(), packets.begin() + 9);
    PipelineResult before;
    run_packets(prefix, before);

    std::vector<Datagram> with_hole = prefix;
    for (std::size_t i = 10; i < packets.size(); ++i) with_hole.push_back(packets[i]);
    PipelineResult after;
    run_packets(with_hole, after);

    CHECK(after.state == SessionState::RecoveryRequired);
    CHECK_EQ(after.mold.gap_events, 1);
    CHECK(after.mold.messages_missed > 0);
    CHECK(after.mold.blocked_packets > 0);
    // Nothing after the gap was applied.
    compare_state(before.fast, after.fast, "book frozen after unrecovered gap");
    CHECK_EQ(after.fast.events_processed(), before.fast.events_processed());
}

// With auto-resync the run continues and the loss is reported. The book is
// then knowingly incomplete, which is a different thing from silently wrong.
void test_dropped_packet_with_auto_resync_continues() {
    itch_test::GeneratorConfig gcfg;
    gcfg.event_count = 20000;
    const auto events = itch_test::generate_events(gcfg, 4242);
    auto packets = build_packets(events);

    std::vector<Datagram> with_hole;
    for (std::size_t i = 0; i < packets.size(); ++i) {
        if (i == 10) continue;
        with_hole.push_back(packets[i]);
    }
    PipelineResult r;
    run_packets(with_hole, r, /*auto_resync=*/true);
    CHECK_EQ(r.mold.gap_events, 1);
    CHECK(r.mold.messages_missed > 0);
    CHECK_EQ(r.mold.blocked_packets, 0);
    // Both books saw the same (incomplete) stream, so they must still agree.
    compare_state(r.research, r.fast, "auto-resync research vs low-latency");
}

void test_duplicate_and_reordered_packets() {
    itch_test::GeneratorConfig gcfg;
    gcfg.event_count = 30000;
    const auto events = itch_test::generate_events(gcfg, 909);
    const auto packets = build_packets(events);

    PipelineResult clean;
    run_packets(packets, clean);

    // Every packet delivered twice: a duplicated multicast path. The book must
    // be identical to the clean run.
    std::vector<Datagram> doubled;
    for (const auto& p : packets) {
        doubled.push_back(p);
        doubled.push_back(p);
    }
    PipelineResult dup;
    run_packets(doubled, dup);
    CHECK_EQ(dup.mold.duplicate_packets, packets.size());
    compare_state(clean.fast, dup.fast, "duplicated packets");
    CHECK_EQ(dup.fast.events_processed(), clean.fast.events_processed());

    // A packet arriving after the one that follows it. The out-of-order packet
    // opens a gap; with auto-resync the late arrival is then a duplicate and
    // must be suppressed, so the run is missing exactly that packet's messages
    // and nothing is applied twice.
    std::vector<Datagram> reordered = packets;
    std::swap(reordered[5], reordered[6]);
    PipelineResult ooo;
    run_packets(reordered, ooo, /*auto_resync=*/true);
    CHECK_EQ(ooo.mold.gap_events, 1);
    CHECK(ooo.mold.duplicate_packets >= 1);
    compare_state(ooo.research, ooo.fast, "reordered research vs low-latency");
}

// The capture file the replay tool reads must round trip, and PacketReplayFeed
// driven by the engine must reach the same book as the raw decode loop.
void test_capture_file_round_trip_and_replay_feed() {
    itch_test::GeneratorConfig gcfg;
    gcfg.event_count = 60000;
    const auto events = itch_test::generate_events(gcfg, 5678);
    const auto packets = build_packets(events);

    const std::string path = "test_pipeline_capture.itchcap";
    CaptureHeader hdr;
    hdr.session_epoch_ns = 1'700'000'000'000'000'000LL;
    {
        CaptureWriter w;
        CHECK(w.open(path, hdr));
        std::int64_t ts = hdr.session_epoch_ns;
        for (const auto& p : packets) {
            ts += 1000;
            CHECK(w.write_packet(p.data(), p.size(), ts, 1));
        }
        CHECK(w.close());
    }

    PacketReplayFeed feed;
    ReplayConfig rcfg;
    rcfg.pacing = ReplayPacing::MaxSpeed;
    rcfg.auto_resync = true;
    std::string err;
    CHECK(feed.open(path, rcfg, &err));
    CHECK_EQ(feed.header().packet_count, packets.size());
    CHECK_EQ(feed.header().session_epoch_ns, hdr.session_epoch_ns);

    LowLatencyOrderBook book(test_book_config());
    strategy::QuoteObserver observer;
    auto engine = runtime::make_engine(feed, book, observer);
    runtime::EngineConfig ecfg;
    ecfg.notify_strategy = true;
    engine.set_config(ecfg);
    const auto stats = engine.run();

    CHECK(stats.events > 0);
    CHECK_EQ(stats.strategy_calls, stats.events);
    CHECK_EQ(observer.events(), stats.events);
    CHECK(observer.quote_changes() > 0);
    CHECK_EQ(feed.mold_stats().gap_events, 0);
    CHECK_EQ(feed.overflow_packets(), 0);

    PipelineResult direct;
    run_packets(packets, direct);
    compare_state(direct.fast, book, "capture replay vs direct decode");

    std::remove(path.c_str());
}

// A capture with a different MTU must produce the same book: packet framing is
// transport, not semantics.
void test_mtu_independence() {
    itch_test::GeneratorConfig gcfg;
    gcfg.event_count = 40000;
    const auto events = itch_test::generate_events(gcfg, 60606);

    PipelineResult small, large;
    run_packets(build_packets(events, 100), small);
    run_packets(build_packets(events, 9000), large);
    compare_state(small.fast, large.fast, "mtu 100 vs mtu 9000");
    CHECK_EQ(small.mold.messages_delivered, large.mold.messages_delivered);
    CHECK(small.mold.packets > large.mold.packets);
}

}  // namespace

int main() {
    test_capture_replay_equivalence();
    test_dropped_packet_holds_the_book_back();
    test_dropped_packet_with_auto_resync_continues();
    test_duplicate_and_reordered_packets();
    test_capture_file_round_trip_and_replay_feed();
    test_mtu_independence();
    itch_test::pass("pipeline equivalence");
    return 0;
}
