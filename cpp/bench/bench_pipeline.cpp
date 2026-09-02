// Full network-to-book pipeline benchmark.
//
//   datagram -> MoldUDP64 decode -> ITCH parse -> MarketEvent -> OrderBook
//
// Every stage is measured on its own and in combination, so the cost of each
// is a subtraction rather than a guess:
//
//   mold decode      decode() with a handler that only counts
//   + parse          the same, with the parser attached
//   + book           the same, with the low-latency book attached
//
// The per-message distributions are measured with fenced TSC pairs around the
// individual calls; throughput comes from separate clean passes with no timer
// in the loop. Both are reported, and the timer overhead is printed so a
// reader can judge how much of a 20-cycle number is the ruler.
//
// What this does NOT measure: anything on the wire. There is no network here,
// so these are host processing numbers - packet-in-memory to book-updated.
// Network-to-book latency additionally includes NIC, driver and kernel time
// and can only be measured against a real feed with hardware timestamps.

#include <cstdio>
#include <string>
#include <vector>

#include "bench_common.hpp"
#include "itch_engine/book/low_latency_book.hpp"
#include "itch_engine/feed/capture.hpp"
#include "itch_engine/feed/itch_encoder.hpp"
#include "itch_engine/feed/itch_parser.hpp"
#include "itch_engine/feed/market_data_feed.hpp"
#include "itch_engine/feed/moldudp64.hpp"
#include "itch_engine/order_book.hpp"
#include "itch_engine/runtime/live_engine.hpp"
#include "itch_engine/strategy/strategy.hpp"
#include "test_support.hpp"

using namespace itch;
using namespace itch::book;
using namespace itch::feed;
using namespace itch::runtime;

namespace {

constexpr std::int64_t kSessionEpoch = 1'700'000'000'000'000'000LL;

struct PacketSet {
    // One contiguous arena holding every datagram back to back, plus offsets.
    // Laid out this way so the decode loop streams through memory the way a
    // receive ring does, instead of chasing a vector of vectors.
    std::vector<std::uint8_t> arena;
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> lengths;
    std::uint64_t messages = 0;

    std::size_t size() const { return offsets.size(); }
    const std::uint8_t* data(std::size_t i) const { return arena.data() + offsets[i]; }
};

PacketSet build_packets(const std::vector<MarketEvent>& events, std::size_t mtu) {
    PacketSet ps;
    EncoderConfig ecfg;
    ecfg.session_epoch_ns = kSessionEpoch;
    ItchEncoder enc(ecfg);
    MoldUDP64Packer packer("BENCHCAP", 1, mtu);

    auto store = [&](const std::uint8_t* p, std::size_t n) {
        ps.offsets.push_back(static_cast<std::uint32_t>(ps.arena.size()));
        ps.lengths.push_back(static_cast<std::uint32_t>(n));
        ps.arena.insert(ps.arena.end(), p, p + n);
    };

    for (const MarketEvent& ev : events) {
        enc.encode(ev, [&](const std::uint8_t* msg, std::uint8_t len) {
            if (packer.would_overflow(len)) {
                const std::uint8_t* out = nullptr;
                const std::size_t n = packer.flush(&out);
                if (n > 0) store(out, n);
            }
            packer.append(msg, len);
        });
    }
    const std::uint8_t* out = nullptr;
    const std::size_t n = packer.flush(&out);
    if (n > 0) store(out, n);
    ps.messages = enc.stats().messages;
    return ps;
}

PacketSet load_capture(const std::string& path, std::int64_t* epoch) {
    PacketSet ps;
    CaptureReader reader;
    std::string err;
    if (!reader.open(path, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        std::exit(2);
    }
    *epoch = reader.header().session_epoch_ns;
    const std::uint8_t* data = nullptr;
    std::size_t len = 0;
    std::int64_t ts = 0;
    while (reader.next(data, len, ts)) {
        ps.offsets.push_back(static_cast<std::uint32_t>(ps.arena.size()));
        ps.lengths.push_back(static_cast<std::uint32_t>(len));
        ps.arena.insert(ps.arena.end(), data, data + len);
    }
    ps.messages = reader.header().message_count;
    return ps;
}

ParserConfig parser_config(std::int64_t epoch) {
    ParserConfig cfg;
    cfg.session_epoch_ns = epoch;
    return cfg;
}

// --- clean throughput passes (no timer inside the loop) --------------------

double pass_mold_only(const PacketSet& ps, std::uint64_t* messages) {
    MoldUDP64Decoder dec;
    dec.set_auto_resync(true);
    std::uint64_t n = 0;
    const std::uint64_t c0 = cycles_begin();
    for (std::size_t i = 0; i < ps.size(); ++i) {
        dec.decode(ps.data(i), ps.lengths[i],
                   [&](std::uint64_t, const std::uint8_t* m, std::uint16_t len) {
                       n += m[0] + len;  // touch the payload so it is not elided
                   });
    }
    const std::uint64_t c1 = cycles_end();
    bench::keep(n);
    *messages = dec.stats().messages_delivered;
    return static_cast<double>(c1 - c0);
}

double pass_mold_parse(const PacketSet& ps, std::int64_t epoch, std::uint64_t* events) {
    MoldUDP64Decoder dec;
    dec.set_auto_resync(true);
    ItchParser parser(parser_config(epoch));
    MarketEvent ev;
    std::uint64_t sink = 0;
    const std::uint64_t c0 = cycles_begin();
    for (std::size_t i = 0; i < ps.size(); ++i) {
        dec.decode(ps.data(i), ps.lengths[i],
                   [&](std::uint64_t, const std::uint8_t* m, std::uint16_t len) {
                       if (parser.parse(m, len, ev) == ParseResult::Ok) sink += ev.quantity;
                   });
    }
    const std::uint64_t c1 = cycles_end();
    bench::keep(sink);
    *events = parser.stats().events;
    return static_cast<double>(c1 - c0);
}

template <class Book>
double pass_full(const PacketSet& ps, std::int64_t epoch, Book& book, std::uint64_t* events) {
    MoldUDP64Decoder dec;
    dec.set_auto_resync(true);
    ItchParser parser(parser_config(epoch));
    MarketEvent ev;
    const std::uint64_t c0 = cycles_begin();
    for (std::size_t i = 0; i < ps.size(); ++i) {
        dec.decode(ps.data(i), ps.lengths[i],
                   [&](std::uint64_t, const std::uint8_t* m, std::uint16_t len) {
                       if (parser.parse(m, len, ev) == ParseResult::Ok) book.apply(ev);
                   });
    }
    const std::uint64_t c1 = cycles_end();
    bench::keep(book);
    *events = parser.stats().events;
    return static_cast<double>(c1 - c0);
}

// --- sampled pass (fenced timers around each stage) ------------------------

struct Distributions {
    LatencyRecorder packet_decode;  // whole decode() call, per datagram
    LatencyRecorder parse;          // one ITCH message -> MarketEvent
    LatencyRecorder book_apply;     // one MarketEvent -> book
    LatencyRecorder message_total;  // parse + apply for one message
    LatencyRecorder packet_total;   // decode + parse + apply for one datagram
};

void sampled_pass(const PacketSet& ps, std::int64_t epoch, LowLatencyOrderBook& book,
                  Distributions& d) {
    MoldUDP64Decoder dec;
    dec.set_auto_resync(true);
    ItchParser parser(parser_config(epoch));
    MarketEvent ev;

    for (std::size_t i = 0; i < ps.size(); ++i) {
        dec.decode(ps.data(i), ps.lengths[i],
                   [&](std::uint64_t, const std::uint8_t* m, std::uint16_t len) {
                       const std::uint64_t m0 = cycles_begin();
                       const ParseResult r = parser.parse(m, len, ev);
                       const std::uint64_t m1 = cycles_end();
                       d.parse.record_delta(m0, m1);
                       if (r != ParseResult::Ok) return;
                       const std::uint64_t b0 = cycles_begin();
                       book.apply(ev);
                       const std::uint64_t b1 = cycles_end();
                       d.book_apply.record_delta(b0, b1);
                       d.message_total.record_delta(m0, b1);
                   });
    }

    // Whole-packet latency gets its own pass with a fresh book and no nested
    // timers. Measuring it around the loop above would have counted two fenced
    // timer pairs per message inside it - about 90 cycles x ~46 messages per
    // datagram, which is most of what such a number would report.
    LowLatencyOrderBook packet_book(book.config());
    MoldUDP64Decoder dec3;
    dec3.set_auto_resync(true);
    ItchParser parser3(parser_config(epoch));
    MarketEvent ev3;
    for (std::size_t i = 0; i < ps.size(); ++i) {
        const std::uint64_t p0 = cycles_begin();
        dec3.decode(ps.data(i), ps.lengths[i],
                    [&](std::uint64_t, const std::uint8_t* m, std::uint16_t len) {
                        if (parser3.parse(m, len, ev3) == ParseResult::Ok) packet_book.apply(ev3);
                    });
        const std::uint64_t p1 = cycles_end();
        d.packet_total.record_delta(p0, p1);
    }

    // A second pass for the transport cost on its own, so the mold number is
    // not inflated by the parse and book work nested inside it.
    MoldUDP64Decoder dec2;
    dec2.set_auto_resync(true);
    std::uint64_t sink = 0;
    for (std::size_t i = 0; i < ps.size(); ++i) {
        const std::uint64_t t0 = cycles_begin();
        dec2.decode(ps.data(i), ps.lengths[i],
                    [&](std::uint64_t, const std::uint8_t* m, std::uint16_t len) {
                        sink += m[0] + len;
                    });
        const std::uint64_t t1 = cycles_end();
        d.packet_decode.record_delta(t0, t1);
    }
    bench::keep(sink);
}

// --- dispatch comparison ----------------------------------------------------
//
// The runtime uses templates rather than virtual calls. That is a claim about
// cost, so it gets measured on the same stream instead of asserted.

struct StaticFeed {
    const std::vector<MarketEvent>* events = nullptr;
    std::size_t pos = 0;
    bool next_event(MarketEvent& out) {
        if (pos >= events->size()) return false;
        out = (*events)[pos++];
        return true;
    }
    bool healthy() const { return true; }
};

class VirtualFeed : public MarketDataFeed {
public:
    explicit VirtualFeed(const std::vector<MarketEvent>& e) : events_(e) {}
    bool next_event(MarketEvent& out) override {
        if (pos_ >= events_.size()) return false;
        out = events_[pos_++];
        return true;
    }
    bool healthy() const { return true; }

private:
    const std::vector<MarketEvent>& events_;
    std::size_t pos_ = 0;
};

struct VirtualFeedShim {
    MarketDataFeed* feed = nullptr;
    bool next_event(MarketEvent& out) { return feed->next_event(out); }
    bool healthy() const { return true; }
};

double dispatch_static(const std::vector<MarketEvent>& events, LowLatencyOrderBook& book) {
    StaticFeed feed;
    feed.events = &events;
    strategy::QuoteObserver observer;
    auto engine = make_engine(feed, book, observer);
    EngineConfig cfg;
    cfg.notify_strategy = true;
    engine.set_config(cfg);
    const std::uint64_t c0 = cycles_begin();
    const auto stats = engine.run();
    const std::uint64_t c1 = cycles_end();
    bench::keep(stats);
    bench::keep(observer);
    return static_cast<double>(c1 - c0);
}

double dispatch_virtual(const std::vector<MarketEvent>& events, LowLatencyOrderBook& book) {
    VirtualFeed vfeed(events);
    VirtualFeedShim shim;
    shim.feed = &vfeed;
    strategy::QuoteObserver observer;
    strategy::StrategyAdapter<strategy::QuoteObserver> adapter(observer);
    strategy::Strategy* sp = &adapter;
    struct VirtualStrategyShim {
        strategy::Strategy* s;
        void on_market_update(const strategy::MarketState& st) { s->on_market_update(st); }
    } sshim{sp};

    auto engine = make_engine(shim, book, sshim);
    EngineConfig cfg;
    cfg.notify_strategy = true;
    engine.set_config(cfg);
    const std::uint64_t c0 = cycles_begin();
    const auto stats = engine.run();
    const std::uint64_t c1 = cycles_end();
    bench::keep(stats);
    bench::keep(observer);
    return static_cast<double>(c1 - c0);
}

void report_stage(const char* label, double cycles, std::uint64_t units, const char* unit_name) {
    const double per = cycles / static_cast<double>(units);
    std::printf("  %-30s %9.2f ticks  %8.2f ns  %12.0f %s/s\n", label, per,
                bench::cycles_to_ns(per),
                bench::cycles_to_ns(per) > 0 ? 1e9 / bench::cycles_to_ns(per) : 0.0, unit_name);
}

}  // namespace

int main(int argc, char** argv) {
    bench::Args args = bench::Args::parse(argc, argv);
    bench::announce(args, "network-to-book pipeline benchmark");

    std::int64_t epoch = kSessionEpoch;
    PacketSet ps;
    std::vector<MarketEvent> events;

    if (!args.capture.empty()) {
        ps = load_capture(args.capture, &epoch);
        std::printf("source        : capture %s\n", args.capture.c_str());
    } else {
        itch_test::GeneratorConfig gcfg;
        gcfg.event_count = args.events;
        events = itch_test::generate_events(gcfg, args.seed);
        ps = build_packets(events, args.mtu);
        std::printf("source        : synthetic, seed %llu, mtu %zu\n",
                    static_cast<unsigned long long>(args.seed), args.mtu);
    }
    std::printf("packets       : %zu (%.2f MiB, %.1f messages/packet)\n", ps.size(),
                static_cast<double>(ps.arena.size()) / (1024.0 * 1024.0),
                ps.size() ? static_cast<double>(ps.messages) / static_cast<double>(ps.size()) : 0.0);

    BookConfig bcfg;
    if (args.max_orders) bcfg.max_orders = args.max_orders;
    if (args.index_capacity) bcfg.index_capacity = args.index_capacity;
    if (args.max_levels) bcfg.max_levels = args.max_levels;
    if (args.tick_count) bcfg.tick_count = args.tick_count;

    // Warm up every stage before measuring any of it.
    {
        std::uint64_t n = 0;
        pass_mold_only(ps, &n);
        pass_mold_parse(ps, epoch, &n);
        LowLatencyOrderBook warm(bcfg);
        pass_full(ps, epoch, warm, &n);
    }

    std::uint64_t messages = 0, parsed = 0, applied = 0;
    double best_mold = 0, best_parse = 0, best_full = 0, best_research = 0;
    bench::AllocStats full_alloc{};

    for (int i = 0; i < args.repeats; ++i) {
        const double m = pass_mold_only(ps, &messages);
        if (best_mold == 0 || m < best_mold) best_mold = m;

        const double p = pass_mold_parse(ps, epoch, &parsed);
        if (best_parse == 0 || p < best_parse) best_parse = p;

        LowLatencyOrderBook book(bcfg);
        bench::AllocScope scope;
        const double f = pass_full(ps, epoch, book, &applied);
        if (best_full == 0 || f < best_full) {
            best_full = f;
            full_alloc = scope.delta();
        }

        OrderBook research;
        const double rr = pass_full(ps, epoch, research, &applied);
        if (best_research == 0 || rr < best_research) best_research = rr;
    }

    std::printf("messages      : %llu delivered, %llu decoded to events\n",
                static_cast<unsigned long long>(messages),
                static_cast<unsigned long long>(parsed));

    std::printf("\nstage throughput (clean passes, best of %d)\n", args.repeats);
    report_stage("UDP buffer -> MoldUDP64", best_mold, ps.size(), "packets");
    report_stage("  ... per message", best_mold, messages, "msgs");
    report_stage("+ ITCH decode -> MarketEvent", best_parse, messages, "msgs");
    report_stage("+ LowLatencyOrderBook", best_full, messages, "msgs");
    report_stage("+ research OrderBook", best_research, messages, "msgs");

    const double mold_per = best_mold / static_cast<double>(messages);
    const double parse_per = (best_parse - best_mold) / static_cast<double>(messages);
    const double book_per = (best_full - best_parse) / static_cast<double>(messages);
    const double research_book_per = (best_research - best_parse) / static_cast<double>(messages);
    std::printf("\nstage cost by subtraction (ticks / ns per message)\n");
    std::printf("  %-30s %9.2f  %8.2f\n", "MoldUDP64 transport", mold_per,
                bench::cycles_to_ns(mold_per));
    std::printf("  %-30s %9.2f  %8.2f\n", "ITCH parse", parse_per,
                bench::cycles_to_ns(parse_per));
    std::printf("  %-30s %9.2f  %8.2f\n", "low-latency book apply", book_per,
                bench::cycles_to_ns(book_per));
    std::printf("  %-30s %9.2f  %8.2f\n", "research book apply", research_book_per,
                bench::cycles_to_ns(research_book_per));
    const double total_per = best_full / static_cast<double>(messages);
    std::printf("  %-30s %9.2f  %8.2f   (%.0f messages/s end to end)\n", "END TO END", total_per,
                bench::cycles_to_ns(total_per),
                bench::cycles_to_ns(total_per) > 0 ? 1e9 / bench::cycles_to_ns(total_per) : 0.0);

    std::printf("\nallocations in the measured region: %llu (%.6f per message)\n",
                static_cast<unsigned long long>(full_alloc.allocations),
                static_cast<double>(full_alloc.allocations) / static_cast<double>(messages));

    // Distributions.
    Distributions d;
    d.packet_decode.reserve(ps.size());
    d.packet_total.reserve(ps.size());
    d.parse.reserve(messages + 16);
    d.book_apply.reserve(messages + 16);
    d.message_total.reserve(messages + 16);
    {
        LowLatencyOrderBook book(bcfg);
        sampled_pass(ps, epoch, book, d);
    }

    bench::print_percentile_header();
    bench::print_percentiles("mold decode (per packet)", d.packet_decode.compute());
    bench::print_percentiles("itch parse (per msg)", d.parse.compute());
    bench::print_percentiles("book apply (per msg)", d.book_apply.compute());
    bench::print_percentiles("parse+book (per msg)", d.message_total.compute());
    bench::print_percentiles("whole datagram", d.packet_total.compute());

    std::printf("\n%-26s %10s %10s %10s %10s %10s %10s %10s\n", "stage (ns)", "p50", "p90", "p99",
                "p99.9", "p99.99", "max", "mean");
    bench::print_percentiles_ns("mold decode (per packet)", d.packet_decode.compute());
    bench::print_percentiles_ns("itch parse (per msg)", d.parse.compute());
    bench::print_percentiles_ns("book apply (per msg)", d.book_apply.compute());
    bench::print_percentiles_ns("parse+book (per msg)", d.message_total.compute());
    bench::print_percentiles_ns("whole datagram", d.packet_total.compute());

    // Dispatch: static vs virtual, over the decoded event stream.
    if (!events.empty()) {
        LowLatencyOrderBook b1(bcfg), b2(bcfg);
        dispatch_static(events, b1);   // warm
        dispatch_virtual(events, b2);  // warm
        LowLatencyOrderBook b3(bcfg), b4(bcfg);
        const double s = dispatch_static(events, b3);
        const double v = dispatch_virtual(events, b4);
        const double sp = s / static_cast<double>(events.size());
        const double vp = v / static_cast<double>(events.size());
        std::printf("\nfeed+strategy dispatch, same events (%zu)\n", events.size());
        std::printf("  static (templates) : %6.2f ticks/event  %6.2f ns\n", sp,
                    bench::cycles_to_ns(sp));
        std::printf("  virtual (two vcalls): %6.2f ticks/event  %6.2f ns  (+%.1f%%)\n", vp,
                    bench::cycles_to_ns(vp), sp > 0 ? (vp - sp) / sp * 100.0 : 0.0);
    }

    if (args.json) {
        const auto md = d.message_total.compute();
        std::printf(
            "\nJSON {\"messages\":%llu,\"packets\":%zu,\"mold_ns\":%.3f,\"parse_ns\":%.3f,"
            "\"book_ns\":%.3f,\"end_to_end_ns\":%.3f,\"p50_ns\":%.3f,\"p99_ns\":%.3f,"
            "\"p999_ns\":%.3f,\"allocs_per_msg\":%.6f}\n",
            static_cast<unsigned long long>(messages), ps.size(), bench::cycles_to_ns(mold_per),
            bench::cycles_to_ns(parse_per), bench::cycles_to_ns(book_per),
            bench::cycles_to_ns(total_per), bench::cycles_to_ns(md.p50),
            bench::cycles_to_ns(md.p99), bench::cycles_to_ns(md.p999),
            static_cast<double>(full_alloc.allocations) / static_cast<double>(messages));
    }
    return 0;
}
