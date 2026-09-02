// itch_make_capture - build a MoldUDP64/ITCH packet capture.
//
// This is the historical packet simulator's producer half:
//
//   normalized parquet --(scripts/export_events.py)--> events.evbin
//                                                          |
//                                              itch_make_capture
//                                                          |
//                                                    day.itchcap
//                                                          |
//                                   itch_replay / bench_pipeline
//
// The capture holds real Nasdaq TotalView-ITCH 5.0 messages inside real
// MoldUDP64 datagrams. Nothing about it is a mock: the decoder, parser and
// book that read it are the same ones the live feed drives, and the only
// component the capture replaces is the socket.
//
// It can also generate a synthetic day directly, so the pipeline is runnable
// from a clean clone with no data at all.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "itch_engine/feed/capture.hpp"
#include "itch_engine/feed/itch_encoder.hpp"
#include "itch_engine/feed/moldudp64.hpp"
#include "itch_engine/market_event.hpp"
#include "test_support.hpp"

using namespace itch;
using namespace itch::feed;

namespace {

// The interchange format between the Python historical path and this tool: a
// 64-byte header followed by MarketEvent records written verbatim. It is a
// local artifact, so it is host-endian by design; the wire format it produces
// is the one that has to be portable.
constexpr char kEvbinMagic[8] = {'I', 'T', 'C', 'H', 'E', 'V', 'B', '1'};
constexpr std::size_t kEvbinHeaderLen = 64;

struct Options {
    std::string input;
    std::string output = "capture.itchcap";
    std::size_t mtu = 1400;
    std::size_t synthetic_events = 0;
    std::uint64_t seed = 20260831;
    std::string symbol = "AAPL";
    std::uint16_t stock_locate = 1;
    std::string session = "ITCHCAP";
    // Nanoseconds of wall time per packet when the events carry no usable
    // timestamps. Only used for the capture's own arrival clock.
    std::int64_t default_gap_ns = 1000;
};

void usage() {
    std::printf(
        "itch_make_capture - build a MoldUDP64/ITCH capture for replay\n"
        "\n"
        "  --input FILE        events.evbin from scripts/export_events.py\n"
        "  --synthetic N       instead of --input, generate N synthetic events\n"
        "  --seed S            synthetic generator seed (default 20260831)\n"
        "  --output FILE       capture path (default capture.itchcap)\n"
        "  --mtu N             datagram payload budget in bytes (default 1400)\n"
        "  --symbol SYM        stock symbol written into the ITCH messages\n"
        "  --locate N          ITCH stock locate code (default 1)\n"
        "  --session NAME      MoldUDP64 session id, max 10 chars\n"
        "\n"
        "Examples:\n"
        "  itch_make_capture --synthetic 2000000 --output demo.itchcap\n"
        "  itch_make_capture --input data/capture/AAPL_2026-07-01.evbin \\\n"
        "                    --output data/capture/AAPL_2026-07-01.itchcap\n");
}

bool read_evbin(const std::string& path, std::vector<MarketEvent>& out, std::int64_t* epoch) {
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
    std::memcpy(epoch, head + 16, 8);
    std::memcpy(&count, head + 24, 8);
    if (version != 1 || record_size != sizeof(MarketEvent)) {
        std::fprintf(stderr, "%s: version %u record size %u (expected 1 / %zu)\n", path.c_str(),
                     version, record_size, sizeof(MarketEvent));
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<std::size_t>(count));
    const std::size_t got = count == 0 ? 0 : std::fread(out.data(), sizeof(MarketEvent),
                                                        static_cast<std::size_t>(count), f);
    std::fclose(f);
    if (got != count) {
        std::fprintf(stderr, "%s: short read (%zu of %llu records)\n", path.c_str(), got,
                     static_cast<unsigned long long>(count));
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", k.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (k == "--input") {
            opt.input = next();
        } else if (k == "--output") {
            opt.output = next();
        } else if (k == "--mtu") {
            opt.mtu = std::strtoull(next().c_str(), nullptr, 10);
        } else if (k == "--synthetic") {
            opt.synthetic_events = std::strtoull(next().c_str(), nullptr, 10);
        } else if (k == "--seed") {
            opt.seed = std::strtoull(next().c_str(), nullptr, 10);
        } else if (k == "--symbol") {
            opt.symbol = next();
        } else if (k == "--locate") {
            opt.stock_locate = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        } else if (k == "--session") {
            opt.session = next();
        } else if (k == "--help" || k == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n", k.c_str());
            usage();
            return 2;
        }
    }

    std::vector<MarketEvent> events;
    std::int64_t epoch = 0;

    if (!opt.input.empty()) {
        if (!read_evbin(opt.input, events, &epoch)) return 1;
        std::printf("read %zu events from %s\n", events.size(), opt.input.c_str());
    } else if (opt.synthetic_events > 0) {
        itch_test::GeneratorConfig gcfg;
        gcfg.event_count = opt.synthetic_events;
        events = itch_test::generate_events(gcfg, opt.seed);
        // Round down to the start of the synthetic day so ITCH's 48-bit
        // nanoseconds-since-midnight field is in range.
        epoch = events.empty() ? 0 : (events.front().timestamp / 86'400'000'000'000LL) *
                                         86'400'000'000'000LL;
        std::printf("generated %zu synthetic events (seed %llu)\n", events.size(),
                    static_cast<unsigned long long>(opt.seed));
    } else {
        std::fprintf(stderr, "need --input or --synthetic\n\n");
        usage();
        return 2;
    }
    if (events.empty()) {
        std::fprintf(stderr, "no events\n");
        return 1;
    }

    EncoderConfig ecfg;
    ecfg.session_epoch_ns = epoch;
    ecfg.stock_locate = opt.stock_locate;
    std::memset(ecfg.stock, ' ', sizeof(ecfg.stock));
    std::memcpy(ecfg.stock, opt.symbol.data(),
                opt.symbol.size() < sizeof(ecfg.stock) ? opt.symbol.size() : sizeof(ecfg.stock));

    CaptureHeader hdr;
    hdr.session_epoch_ns = epoch;
    hdr.stock_locate = opt.stock_locate;
    std::memcpy(hdr.stock, ecfg.stock, sizeof(hdr.stock));
    std::memset(hdr.session, ' ', sizeof(hdr.session));
    std::memcpy(hdr.session, opt.session.data(),
                opt.session.size() < sizeof(hdr.session) ? opt.session.size()
                                                         : sizeof(hdr.session));

    CaptureWriter writer;
    if (!writer.open(opt.output, hdr)) {
        std::fprintf(stderr, "cannot write %s\n", opt.output.c_str());
        return 1;
    }

    ItchEncoder encoder(ecfg);
    MoldUDP64Packer packer(opt.session.c_str(), 1, opt.mtu);

    // The capture's arrival clock is the exchange timestamp of the first
    // message in each datagram. That reproduces the original day's burst
    // structure under paced replay, which is what makes "replay at real speed"
    // mean anything.
    std::int64_t packet_ts = events.front().timestamp;
    std::uint32_t packet_messages = 0;
    std::uint64_t packets = 0;
    bool packet_started = false;

    auto flush_packet = [&]() {
        const std::uint8_t* out = nullptr;
        const std::size_t n = packer.flush(&out);
        if (n == 0) return;
        writer.write_packet(out, n, packet_ts, packet_messages);
        ++packets;
        packet_messages = 0;
        packet_started = false;
    };

    for (const MarketEvent& ev : events) {
        encoder.encode(ev, [&](const std::uint8_t* msg, std::uint8_t len) {
            if (packer.would_overflow(len)) flush_packet();
            if (!packet_started) {
                packet_ts = ev.timestamp;
                packet_started = true;
            }
            packer.append(msg, len);
            ++packet_messages;
        });
    }
    flush_packet();

    // Close the session the way the venue does.
    {
        const std::uint8_t* out = nullptr;
        const std::size_t n = packer.end_of_session(&out);
        writer.write_packet(out, n, packet_ts + opt.default_gap_ns, 0);
        ++packets;
    }

    if (!writer.close()) {
        std::fprintf(stderr, "error closing %s\n", opt.output.c_str());
        return 1;
    }

    const EncoderStats& s = encoder.stats();
    std::printf("wrote %s\n", opt.output.c_str());
    std::printf("  datagrams          : %llu (mtu %zu)\n", static_cast<unsigned long long>(packets),
                opt.mtu);
    std::printf("  ITCH messages      : %llu\n", static_cast<unsigned long long>(s.messages));
    std::printf("    A add            : %llu\n", static_cast<unsigned long long>(s.adds));
    std::printf("    X cancel         : %llu\n", static_cast<unsigned long long>(s.cancels));
    std::printf("    D delete         : %llu\n", static_cast<unsigned long long>(s.deletes));
    std::printf("    E executed       : %llu\n", static_cast<unsigned long long>(s.executes));
    std::printf("    U replace        : %llu\n", static_cast<unsigned long long>(s.replaces));
    std::printf("    P trade          : %llu\n", static_cast<unsigned long long>(s.trades));
    std::printf("    S system event   : %llu\n", static_cast<unsigned long long>(s.system_events));
    if (s.price_not_representable > 0) {
        // ITCH prices are uint32 in 1/10000 USD. Anything finer, negative, or
        // past $429,496.7295 cannot go on the wire, and is reported rather
        // than rounded into something plausible.
        std::printf("  DROPPED (price not representable as ITCH uint32/1e-4): %llu\n",
                    static_cast<unsigned long long>(s.price_not_representable));
    }
    std::printf("  session epoch (ns) : %lld\n", static_cast<long long>(epoch));
    return 0;
}
