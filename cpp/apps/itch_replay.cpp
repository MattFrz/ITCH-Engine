// itch_replay - drive the low-latency pipeline from a recorded capture.
//
//   capture.itchcap -> MoldUDP64 -> ITCH parse -> MarketEvent -> OrderBook
//                                                                    -> Strategy
//
// This is the development and benchmarking entry point for the live path.
// Every layer above the socket is the same code itch_live runs; the capture
// only stands in for the network, so a change that is correct here is correct
// there.
//
// Pacing:
//   --max-speed            as fast as the machine allows (default)
//   --original-speed       reproduce the original inter-packet timing
//   --speed N              original timing divided by N (2 = twice as fast)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "itch_engine/book/low_latency_book.hpp"
#include "itch_engine/feed/capture.hpp"
#include "itch_engine/feed/multicast_publisher.hpp"
#include "itch_engine/feed/replay_feed.hpp"
#include "itch_engine/order_book.hpp"
#include "itch_engine/runtime/affinity.hpp"
#include "itch_engine/runtime/cycles.hpp"
#include "itch_engine/runtime/latency_recorder.hpp"
#include "itch_engine/runtime/live_engine.hpp"
#include "itch_engine/strategy/strategy.hpp"

using namespace itch;
using namespace itch::book;
using namespace itch::feed;
using namespace itch::runtime;

namespace {

void usage() {
    std::printf(
        "itch_replay - replay a capture through the low-latency pipeline\n"
        "\n"
        "  --capture FILE      .itchcap capture to replay (required)\n"
        "  --book WHICH        lowlatency (default) | research\n"
        "  --max-speed         replay as fast as possible (default)\n"
        "  --original-speed    replay at the original packet timing\n"
        "  --speed N           original timing divided by N\n"
        "  --locate N          only decode this ITCH stock locate (0 = all)\n"
        "  --max-events N      stop after N events\n"
        "  --strict-gaps       do NOT resync over a sequence gap; stop instead\n"
        "  --measure           record per-event latency percentiles\n"
        "  --cpu N             pin to logical cpu N\n"
        "  --lock-memory       mlockall (reports if the OS refuses)\n"
        "  --realtime          SCHED_FIFO (reports if the OS refuses)\n"
        "  --quiet             summary only\n"
        "\n"
        "PUBLISH MODE (send the capture as real UDP multicast instead of\n"
        "decoding it, so itch_live can receive it through the socket path):\n"
        "  --publish-group A.B.C.D  group to send to - use one you control\n"
        "  --publish-port N         UDP port\n"
        "  --publish-interface A.B.C.D  local NIC to send from\n"
        "  --publish-ttl N          multicast TTL (default 1: this segment only)\n");
}

template <class Book>
int run(Book& book, PacketReplayFeed& feed, bool measure, std::uint64_t max_events,
        const char* book_name) {
    strategy::QuoteObserver observer;
    auto engine = make_engine(feed, book, observer);
    EngineConfig cfg;
    cfg.notify_strategy = true;
    cfg.measure = measure;
    cfg.max_events = max_events;
    engine.set_config(cfg);

    LatencyRecorder book_rec, total_rec;
    if (measure) {
        // Sized from the capture header so the recorder never allocates during
        // the run and never silently drops the tail.
        const std::size_t cap =
            static_cast<std::size_t>(feed.header().message_count > 0 ? feed.header().message_count
                                                                     : 1000000) +
            1024;
        book_rec.reserve(cap);
        total_rec.reserve(cap);
        engine.set_recorders(&book_rec, &total_rec);
    }

    const auto wall_start = std::chrono::steady_clock::now();
    const EngineStats stats = engine.run();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                          wall_start)
                               .count();

    std::printf("\n--- %s ---\n", book_name);
    std::printf("events applied      : %llu (%llu changed the book)\n",
                static_cast<unsigned long long>(stats.events),
                static_cast<unsigned long long>(stats.book_events));
    std::printf("wall time           : %.3f s -> %.0f events/s\n", seconds,
                seconds > 0 ? static_cast<double>(stats.events) / seconds : 0.0);
    std::printf("packets read        : %llu\n",
                static_cast<unsigned long long>(feed.packets_read()));

    const MoldStats& m = feed.mold_stats();
    std::printf("\nMoldUDP64: state=%s next_expected_sequence=%llu\n",
                to_string(feed.session_state()),
                static_cast<unsigned long long>(feed.expected_sequence()));
    std::printf("  packets=%llu heartbeats=%llu delivered=%llu\n",
                static_cast<unsigned long long>(m.packets),
                static_cast<unsigned long long>(m.heartbeats),
                static_cast<unsigned long long>(m.messages_delivered));
    std::printf("  gaps=%llu missed=%llu recovered=%llu blocked=%llu\n",
                static_cast<unsigned long long>(m.gap_events),
                static_cast<unsigned long long>(m.messages_missed),
                static_cast<unsigned long long>(m.messages_recovered),
                static_cast<unsigned long long>(m.blocked_packets));
    std::printf("  duplicates=%llu suppressed=%llu malformed=%llu\n",
                static_cast<unsigned long long>(m.duplicate_packets),
                static_cast<unsigned long long>(m.duplicate_messages_suppressed),
                static_cast<unsigned long long>(m.malformed_packets));

    const ParserStats& p = feed.parser_stats();
    std::printf("\nITCH parser: messages=%llu events=%llu ignored=%llu bad_length=%llu "
                "unknown_type=%llu\n",
                static_cast<unsigned long long>(p.messages),
                static_cast<unsigned long long>(p.events),
                static_cast<unsigned long long>(p.ignored),
                static_cast<unsigned long long>(p.bad_length),
                static_cast<unsigned long long>(p.unknown_type));
    std::printf("  A=%llu E=%llu X=%llu D=%llu U=%llu P/Q=%llu S=%llu\n",
                static_cast<unsigned long long>(p.adds),
                static_cast<unsigned long long>(p.executes),
                static_cast<unsigned long long>(p.cancels),
                static_cast<unsigned long long>(p.deletes),
                static_cast<unsigned long long>(p.replaces),
                static_cast<unsigned long long>(p.trades),
                static_cast<unsigned long long>(p.system_events));

    const BestQuote q = book.best();
    std::printf("\nfinal book: orders=%zu bid_levels=%zu ask_levels=%zu\n",
                book.open_order_count(), book.bid_level_count(), book.ask_level_count());
    if (q.has_bid && q.has_ask) {
        std::printf("  best bid %.4f x %lld   best ask %.4f x %lld\n",
                    static_cast<double>(q.bid_price) / static_cast<double>(PRICE_SCALE),
                    static_cast<long long>(q.bid_qty),
                    static_cast<double>(q.ask_price) / static_cast<double>(PRICE_SCALE),
                    static_cast<long long>(q.ask_qty));
    }
    std::printf("  unknown references=%llu clears=%llu\n",
                static_cast<unsigned long long>(book.unknown_order_events()),
                static_cast<unsigned long long>(book.clears_applied()));
    std::printf("strategy: %llu updates, %llu quote changes, widest spread %.4f\n",
                static_cast<unsigned long long>(observer.events()),
                static_cast<unsigned long long>(observer.quote_changes()),
                static_cast<double>(observer.widest_spread()) /
                    static_cast<double>(PRICE_SCALE));

    if (measure) {
        const Percentiles bp = book_rec.compute();
        const Percentiles tp = total_rec.compute();
        const double hz = TscClock::instance().hz();
        auto ns = [&](double c) { return hz > 0 ? c * 1e9 / hz : 0.0; };
        std::printf("\nlatency (ns, TSC calibrated at %.3f GHz, invariant=%s)\n", hz / 1e9,
                    TscClock::instance().invariant() ? "yes" : "NO");
        std::printf("  %-18s %8s %8s %8s %8s %8s %8s\n", "", "p50", "p90", "p99", "p99.9",
                    "p99.99", "max");
        std::printf("  %-18s %8.1f %8.1f %8.1f %8.1f %8.1f %8.1f\n", "book apply", ns(bp.p50),
                    ns(bp.p90), ns(bp.p99), ns(bp.p999), ns(bp.p9999), ns(bp.max));
        std::printf("  %-18s %8.1f %8.1f %8.1f %8.1f %8.1f %8.1f\n", "apply + strategy",
                    ns(tp.p50), ns(tp.p90), ns(tp.p99), ns(tp.p999), ns(tp.p9999), ns(tp.max));
        std::printf("  (samples: %llu; discarded non-monotonic: %llu)\n",
                    static_cast<unsigned long long>(bp.count),
                    static_cast<unsigned long long>(bp.discarded));
    }

    if (!feed.healthy()) {
        std::printf(
            "\nSESSION STATE: %s - %llu messages missing from sequence %llu.\n"
            "The book is INCOMPLETE and must not be trusted. Supply a "
            "GapRecoveryPolicy, or rerun without --strict-gaps to resync and "
            "accept a knowingly incomplete book.\n",
            to_string(feed.session_state()),
            static_cast<unsigned long long>(feed.missing_count()),
            static_cast<unsigned long long>(feed.first_missing_sequence()));
        return 3;
    }
    return 0;
}

// --- historical replay equivalence -----------------------------------------
//
// The claim the whole packet simulator rests on: replaying a day as ITCH
// messages inside MoldUDP64 datagrams reaches the same book as feeding the
// normalized events straight to the validated research book. This checks it on
// whatever data was actually replayed, real or synthetic, rather than only on
// the generated streams in the unit tests.

constexpr char kEvbinMagic[8] = {'I', 'T', 'C', 'H', 'E', 'V', 'B', '1'};
constexpr std::size_t kEvbinHeaderLen = 64;

bool load_evbin(const std::string& path, std::vector<MarketEvent>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return false;
    }
    std::uint8_t head[kEvbinHeaderLen];
    if (std::fread(head, 1, kEvbinHeaderLen, f) != kEvbinHeaderLen ||
        std::memcmp(head, kEvbinMagic, 8) != 0) {
        std::fprintf(stderr, "%s: not an ITCHEVB1 event file\n", path.c_str());
        std::fclose(f);
        return false;
    }
    std::uint32_t version = 0, record_size = 0;
    std::uint64_t count = 0;
    std::memcpy(&version, head + 8, 4);
    std::memcpy(&record_size, head + 12, 4);
    std::memcpy(&count, head + 24, 8);
    if (version != 1 || record_size != sizeof(MarketEvent)) {
        std::fprintf(stderr, "%s: incompatible record layout\n", path.c_str());
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<std::size_t>(count));
    const std::size_t got =
        count == 0 ? 0
                   : std::fread(out.data(), sizeof(MarketEvent),
                                static_cast<std::size_t>(count), f);
    std::fclose(f);
    return got == count;
}

int failures = 0;

void fail(const char* what, long long got, long long want) {
    std::printf("  MISMATCH %-24s replayed=%lld historical=%lld\n", what, got, want);
    ++failures;
}

// Compares two books that are meant to be at the same point in the session.
// Returns the number of mismatches found.
template <class BookA, class BookB>
int compare_books(const BookA& replayed, const BookB& historical, const char* where,
                  std::size_t* levels_checked, std::size_t* orders_checked) {
    int bad = 0;
    auto note = [&](const char* what, long long got, long long want) {
        std::printf("  MISMATCH at %s: %-22s replayed=%lld historical=%lld\n", where, what, got,
                    want);
        ++bad;
    };

    if (replayed.open_order_count() != historical.open_order_count()) {
        note("open orders", static_cast<long long>(replayed.open_order_count()),
             static_cast<long long>(historical.open_order_count()));
    }
    if (replayed.bid_level_count() != historical.bid_level_count()) {
        note("bid levels", static_cast<long long>(replayed.bid_level_count()),
             static_cast<long long>(historical.bid_level_count()));
    }
    if (replayed.ask_level_count() != historical.ask_level_count()) {
        note("ask levels", static_cast<long long>(replayed.ask_level_count()),
             static_cast<long long>(historical.ask_level_count()));
    }

    const BestQuote a = replayed.best();
    const BestQuote b = historical.best();
    if (a.has_bid != b.has_bid || a.bid_price != b.bid_price || a.bid_qty != b.bid_qty) {
        note("best bid", static_cast<long long>(a.bid_price), static_cast<long long>(b.bid_price));
    }
    if (a.has_ask != b.has_ask || a.ask_price != b.ask_price || a.ask_qty != b.ask_qty) {
        note("best ask", static_cast<long long>(a.ask_price), static_cast<long long>(b.ask_price));
    }

    // Every live level, both sides: price, aggregate size, order count, and the
    // exact FIFO order of the ids resting there. The last part is what makes
    // this a real equivalence check rather than an aggregate one.
    for (Side side : {Side::Bid, Side::Ask}) {
        const std::size_t n =
            side == Side::Bid ? historical.bid_level_count() : historical.ask_level_count();
        const auto ha = historical.depth(side, n);
        const auto ra = replayed.depth(side, n);
        if (ha.size() != ra.size()) {
            note("depth size", static_cast<long long>(ra.size()),
                 static_cast<long long>(ha.size()));
            continue;
        }
        for (std::size_t i = 0; i < ha.size(); ++i) {
            ++*levels_checked;
            if (ha[i].price != ra[i].price || ha[i].qty != ra[i].qty ||
                ha[i].order_count != ra[i].order_count) {
                std::printf("  MISMATCH at %s: level %zu side %d replayed=(%lld,%lld,%zu) "
                            "historical=(%lld,%lld,%zu)\n",
                            where, i, static_cast<int>(side),
                            static_cast<long long>(ra[i].price),
                            static_cast<long long>(ra[i].qty), ra[i].order_count,
                            static_cast<long long>(ha[i].price),
                            static_cast<long long>(ha[i].qty), ha[i].order_count);
                ++bad;
                continue;
            }
            const auto hq = historical.queue_at(side, ha[i].price);
            const auto rq = replayed.queue_at(side, ra[i].price);
            *orders_checked += hq.size();
            if (hq != rq) {
                std::printf("  MISMATCH at %s: FIFO order at price %lld\n", where,
                            static_cast<long long>(ha[i].price));
                ++bad;
            }
        }
    }
    return bad;
}

// Replays the capture and the normalized event file side by side, stopping
// both at the same session timestamps and comparing the whole book each time.
//
// The two streams do not correspond event for event and are not supposed to: a
// no-op modify and a zero-quantity add are rows in the parquet and are not
// messages on the wire, and a Databento modify becomes either 'X' or 'U'
// depending on the resting order. What must correspond is the BOOK, at every
// point in the day. Timestamps are the common clock, so the comparison is
// driven off those.
int verify_equivalence(const std::string& capture_path, const std::string& evbin_path,
                       int checkpoints, const BookConfig& bcfg) {
    std::vector<MarketEvent> events;
    if (!load_evbin(evbin_path, events)) return 1;
    if (events.empty()) {
        std::fprintf(stderr, "%s: no events\n", evbin_path.c_str());
        return 1;
    }

    auto feed = std::unique_ptr<PacketReplayFeed>(new PacketReplayFeed());
    ReplayConfig rcfg;
    rcfg.pacing = ReplayPacing::MaxSpeed;
    rcfg.auto_resync = true;
    std::string err;
    if (!feed->open(capture_path, rcfg, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    auto replayed = std::unique_ptr<LowLatencyOrderBook>(new LowLatencyOrderBook(bcfg));
    OrderBook historical;

    std::printf("\n--- historical replay equivalence ---\n");
    std::printf("historical events   : %zu (normalized parquet -> research book)\n",
                events.size());
    std::printf("checkpoints         : %d, spread evenly across the session\n", checkpoints);

    // Checkpoint timestamps, evenly spaced through the event stream.
    std::vector<Timestamp> stops;
    stops.reserve(static_cast<std::size_t>(checkpoints));
    for (int c = 1; c <= checkpoints; ++c) {
        const std::size_t idx =
            static_cast<std::size_t>((static_cast<double>(c) / checkpoints) *
                                     static_cast<double>(events.size() - 1));
        const Timestamp t = events[idx].timestamp;
        if (stops.empty() || t > stops.back()) stops.push_back(t);
    }

    std::size_t hist_pos = 0;
    std::size_t levels_checked = 0, orders_checked = 0;
    int bad = 0;
    MarketEvent wire_ev;
    bool wire_pending = false;

    for (std::size_t s = 0; s < stops.size(); ++s) {
        const Timestamp stop = stops[s];
        while (hist_pos < events.size() && events[hist_pos].timestamp <= stop) {
            historical.apply(events[hist_pos]);
            ++hist_pos;
        }
        for (;;) {
            if (!wire_pending) {
                if (!feed->next_event(wire_ev)) break;
                wire_pending = true;
            }
            if (wire_ev.timestamp > stop) break;
            replayed->apply(wire_ev);
            wire_pending = false;
        }
        char label[64];
        std::snprintf(label, sizeof(label), "checkpoint %zu/%zu", s + 1, stops.size());
        bad += compare_books(*replayed, historical, label, &levels_checked, &orders_checked);
        if (bad > 20) {
            std::printf("  (stopping after 20 mismatches)\n");
            break;
        }
    }

    // Drain both to the end and compare the final state.
    while (hist_pos < events.size()) {
        historical.apply(events[hist_pos]);
        ++hist_pos;
    }
    if (wire_pending) {
        replayed->apply(wire_ev);
        wire_pending = false;
    }
    while (feed->next_event(wire_ev)) replayed->apply(wire_ev);
    bad += compare_books(*replayed, historical, "end of session", &levels_checked,
                         &orders_checked);

    if (replayed->unknown_order_events() != historical.unknown_order_events()) {
        std::printf("  MISMATCH unknown references: replayed=%llu historical=%llu\n",
                    static_cast<unsigned long long>(replayed->unknown_order_events()),
                    static_cast<unsigned long long>(historical.unknown_order_events()));
        ++bad;
    }
    if (replayed->clears_applied() != historical.clears_applied()) {
        std::printf("  MISMATCH clears applied: replayed=%llu historical=%llu\n",
                    static_cast<unsigned long long>(replayed->clears_applied()),
                    static_cast<unsigned long long>(historical.clears_applied()));
        ++bad;
    }
    if (replayed->degraded()) {
        std::printf("  BOOK DEGRADED during verification (%llu capacity rejections)\n",
                    static_cast<unsigned long long>(replayed->metrics().capacity_rejections));
        ++bad;
    }

    std::printf("compared            : %zu level snapshots, %zu resting orders in exact FIFO "
                "order\n",
                levels_checked, orders_checked);
    std::printf("unknown references  : %llu (both books)\n",
                static_cast<unsigned long long>(historical.unknown_order_events()));
    std::printf("clears applied      : %llu (both books)\n",
                static_cast<unsigned long long>(historical.clears_applied()));
    if (bad == 0) {
        std::printf("RESULT              : PASS - 0 divergence between the packet pipeline and "
                    "the historical path\n");
        return 0;
    }
    std::printf("RESULT              : FAIL - %d mismatches\n", bad);
    return 5;
}


// Publish mode: put the capture on the wire as real multicast datagrams.
//
// This is what makes the receive path testable without an exchange feed. The
// bytes sent are exactly the bytes in the capture, so a receiver sees the same
// MoldUDP64 stream it would see from a venue - including the sequence numbers,
// so a dropped datagram on a loaded loopback shows up as a real gap.
int publish(const std::string& capture_path, const PublisherConfig& pcfg, ReplayPacing pacing,
            double speed, std::uint64_t max_packets) {
    CaptureReader reader;
    std::string err;
    if (!reader.open(capture_path, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    MulticastPublisher pub;
    if (!pub.open(pcfg, &err)) {
        std::fprintf(stderr, "cannot open publisher: %s\n", err.c_str());
        return 1;
    }
    std::printf("publishing %s -> %s:%u (ttl %d, %llu datagrams)\n", capture_path.c_str(),
                pcfg.multicast_group.c_str(), pcfg.port, pcfg.ttl,
                static_cast<unsigned long long>(reader.header().packet_count));

    const auto wall_start = std::chrono::steady_clock::now();
    std::int64_t base_capture = 0;
    bool started = false;
    const std::uint8_t* data = nullptr;
    std::size_t len = 0;
    std::int64_t ts = 0;
    std::uint64_t sent = 0;

    while (reader.next(data, len, ts)) {
        if (pacing != ReplayPacing::MaxSpeed) {
            const double div = pacing == ReplayPacing::Scaled && speed > 0.0 ? speed : 1.0;
            if (!started) {
                base_capture = ts;
                started = true;
            } else {
                const auto target = wall_start + std::chrono::nanoseconds(static_cast<std::int64_t>(
                                                     static_cast<double>(ts - base_capture) / div));
                while (std::chrono::steady_clock::now() < target) {
                    // Spin: the last microseconds of a sleep are exactly the
                    // jitter a paced replay is trying not to introduce.
                }
            }
        }
        if (!pub.send(data, len)) {
            std::fprintf(stderr, "send failed after %llu datagrams\n",
                         static_cast<unsigned long long>(sent));
            return 1;
        }
        ++sent;
        if (max_packets != 0 && sent >= max_packets) break;
    }

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
    std::printf("sent %llu datagrams, %llu bytes in %.3f s (%.0f pkt/s), send-buffer stalls %llu, "
                "errors %llu\n",
                static_cast<unsigned long long>(pub.stats().packets),
                static_cast<unsigned long long>(pub.stats().bytes), seconds,
                seconds > 0 ? static_cast<double>(pub.stats().packets) / seconds : 0.0,
                static_cast<unsigned long long>(pub.stats().would_block),
                static_cast<unsigned long long>(pub.stats().errors));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string capture;
    std::string verify;
    int verify_checkpoints = 200;
    std::string which = "lowlatency";
    ReplayConfig rcfg;
    rcfg.pacing = ReplayPacing::MaxSpeed;
    rcfg.auto_resync = true;
    bool measure = false;
    std::uint64_t max_events = 0;
    RuntimeTuning tuning;
    bool quiet = false;
    PublisherConfig pcfg;
    std::uint64_t max_packets = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", k.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (k == "--capture") {
            capture = next();
        } else if (k == "--verify") {
            verify = next();
        } else if (k == "--verify-checkpoints") {
            verify_checkpoints = std::atoi(next().c_str());
        } else if (k == "--book") {
            which = next();
        } else if (k == "--max-speed") {
            rcfg.pacing = ReplayPacing::MaxSpeed;
        } else if (k == "--original-speed") {
            rcfg.pacing = ReplayPacing::Original;
        } else if (k == "--speed") {
            rcfg.pacing = ReplayPacing::Scaled;
            rcfg.speed = std::atof(next().c_str());
        } else if (k == "--locate") {
            rcfg.stock_locate_filter = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        } else if (k == "--max-events") {
            max_events = std::strtoull(next().c_str(), nullptr, 10);
        } else if (k == "--strict-gaps") {
            rcfg.auto_resync = false;
        } else if (k == "--measure") {
            measure = true;
        } else if (k == "--cpu") {
            tuning.cpu = std::atoi(next().c_str());
        } else if (k == "--lock-memory") {
            tuning.lock_memory = true;
        } else if (k == "--realtime") {
            tuning.realtime = true;
        } else if (k == "--publish-group") {
            pcfg.multicast_group = next();
        } else if (k == "--publish-port") {
            pcfg.port = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        } else if (k == "--publish-interface") {
            pcfg.interface_address = next();
        } else if (k == "--publish-ttl") {
            pcfg.ttl = std::atoi(next().c_str());
        } else if (k == "--max-packets") {
            max_packets = std::strtoull(next().c_str(), nullptr, 10);
        } else if (k == "--quiet") {
            quiet = true;
        } else if (k == "--help" || k == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n", k.c_str());
            usage();
            return 2;
        }
    }

    if (capture.empty()) {
        std::fprintf(stderr, "--capture is required\n\n");
        usage();
        return 2;
    }

    if (!pcfg.multicast_group.empty()) {
        return publish(capture, pcfg, rcfg.pacing, rcfg.speed, max_packets);
    }

    const TuningReport report = apply_tuning(tuning);
    TscClock::instance().calibrate(200);
    if (!quiet) {
        std::printf("platform: %s\n", describe_runtime_support().c_str());
        for (const std::string& note : report.notes) std::printf("runtime : %s\n", note.c_str());
    }

    // The feed buffers a packet's worth of events and is a few tens of KiB;
    // heap-allocate it rather than putting that on the stack.
    auto feed = std::unique_ptr<PacketReplayFeed>(new PacketReplayFeed());
    std::string err;
    if (!feed->open(capture, rcfg, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (!quiet) {
        std::printf("capture : %s\n", capture.c_str());
        std::printf("  packets=%llu messages=%llu session_epoch_ns=%lld\n",
                    static_cast<unsigned long long>(feed->header().packet_count),
                    static_cast<unsigned long long>(feed->header().message_count),
                    static_cast<long long>(feed->header().session_epoch_ns));
        std::printf("  pacing=%s\n",
                    rcfg.pacing == ReplayPacing::MaxSpeed
                        ? "max speed"
                        : (rcfg.pacing == ReplayPacing::Original ? "original" : "scaled"));
    }

    if (which == "research") {
        OrderBook book;
        const int rc = run(book, *feed, measure, max_events, "research OrderBook");
        if (!verify.empty()) {
            const int vrc = verify_equivalence(capture, verify, verify_checkpoints, BookConfig{});
            if (vrc != 0) return vrc;
        }
        return rc;
    }
    if (which != "lowlatency") {
        std::fprintf(stderr, "--book must be lowlatency or research\n");
        return 2;
    }
    BookConfig bcfg;
    auto book = std::unique_ptr<LowLatencyOrderBook>(new LowLatencyOrderBook(bcfg));
    int rc = run(*book, *feed, measure, max_events, "LowLatencyOrderBook");
    if (!verify.empty()) {
        const int vrc = verify_equivalence(capture, verify, verify_checkpoints, bcfg);
        if (vrc != 0) rc = vrc;
    }
    if (book->degraded()) {
        std::printf("\nBOOK DEGRADED: %llu events dropped for capacity. Raise "
                    "BookConfig limits (max_orders=%zu index_capacity=%zu max_levels=%zu "
                    "max_offgrid_levels=%zu).\n",
                    static_cast<unsigned long long>(book->metrics().capacity_rejections),
                    bcfg.max_orders, bcfg.index_capacity, bcfg.max_levels,
                    bcfg.max_offgrid_levels);
        return 4;
    }
    if (!quiet) {
        std::printf("\ncapacity: peak orders=%zu (of %zu), peak levels=%zu (of %zu), "
                    "peak off-grid=%zu (of %zu), book memory=%.1f MiB\n",
                    book->metrics().peak_orders, bcfg.max_orders, book->metrics().peak_levels,
                    bcfg.max_levels, book->metrics().peak_offgrid, bcfg.max_offgrid_levels,
                    static_cast<double>(book->memory_bytes()) / (1024.0 * 1024.0));
        std::printf("order index: avg probes=%.3f max probe=%u\n", book->index().average_probes(),
                    book->index().max_probe());
    }
    return rc;
}
