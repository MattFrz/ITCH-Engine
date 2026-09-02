// itch_live - run the pipeline against a live UDP multicast market-data feed.
//
//   NIC -> kernel -> UDP multicast socket -> MoldUDP64 -> ITCH -> book -> strategy
//
// Read this before pointing it anywhere
// -------------------------------------
// There are no addresses, ports, sessions or credentials compiled into this
// program, and there is no default feed. Nasdaq TotalView-ITCH is licensed
// market data delivered over a private connection; running this against a
// feed requires an agreement with the venue and the network to receive it.
// Supply your own group, port and interface, for a feed you are entitled to.
//
// It also does not trade. The strategy interface observes the book; the order
// gateway in this build is a simulator and is not wired up here at all.
//
// Gap handling defaults to strict: an unrecoverable sequence gap stops
// delivery and the program reports it, rather than skipping ahead and leaving
// a book that looks fine and is not. Supply a GapRecoveryPolicy for your
// venue's retransmission service to do better than stopping.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "itch_engine/book/low_latency_book.hpp"
#include "itch_engine/feed/dpdk_backend.hpp"
#include "itch_engine/feed/live_feed.hpp"
#include "itch_engine/feed/udp_multicast_backend.hpp"
#include "itch_engine/runtime/affinity.hpp"
#include "itch_engine/runtime/cycles.hpp"
#include "itch_engine/runtime/latency_recorder.hpp"
#include "itch_engine/strategy/strategy.hpp"

using namespace itch;
using namespace itch::book;
using namespace itch::feed;
using namespace itch::runtime;

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

void usage() {
    std::printf(
        "itch_live - low-latency pipeline over a UDP multicast market-data feed\n"
        "\n"
        "REQUIRED (nothing is defaulted; there is no built-in feed address):\n"
        "  --multicast-group A.B.C.D   multicast group to join\n"
        "  --port N                    UDP port\n"
        "\n"
        "OPTIONAL\n"
        "  --backend udp|dpdk          receive path (default udp; dpdk needs a\n"
        "                              -DITCH_ENABLE_DPDK=ON build - see docs/dpdk.md)\n"
        "  --dpdk-port N               DPDK port id (default 0)\n"
        "  --dpdk-eal ARG              one EAL argument; repeat the flag for each\n"
        "  --interface A.B.C.D         local NIC address that joins the group\n"
        "  --buffer-size BYTES         SO_RCVBUF request (default 33554432)\n"
        "  --batch N                   datagrams per receive call (default 32)\n"
        "  --locate N                  decode only this ITCH stock locate\n"
        "  --session-epoch NS          midnight of the session, ns since epoch,\n"
        "                              added to ITCH ns-since-midnight timestamps\n"
        "  --relaxed-gaps              resync over an unrecoverable gap instead of\n"
        "                              stopping (the book is then INCOMPLETE)\n"
        "  --hardware-timestamps       request NIC receive timestamps (Linux)\n"
        "  --busy-poll US              SO_BUSY_POLL microseconds (Linux)\n"
        "  --cpu N                     pin to logical cpu N\n"
        "  --lock-memory               mlockall (reports if the OS refuses)\n"
        "  --realtime                  SCHED_FIFO (reports if the OS refuses)\n"
        "  --duration S                stop after S seconds (0 = until Ctrl-C)\n"
        "  --stats-interval S          print counters every S seconds (default 5)\n"
        "\n"
        "You are responsible for holding the market-data entitlement for whatever\n"
        "feed you point this at.\n");
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

void print_stats(const LiveFeed& feed, const LowLatencyOrderBook& book, std::uint64_t events,
                 double seconds) {
    const NetworkStats& n = feed.network_stats();
    const MoldStats& m = feed.mold_stats();
    const BestQuote q = book.best();
    std::printf(
        "[%6.1fs] pkts=%llu drops=%llu msgs=%llu events=%llu (%.0f/s) state=%s orders=%zu",
        seconds, static_cast<unsigned long long>(n.packets),
        static_cast<unsigned long long>(n.kernel_drops),
        static_cast<unsigned long long>(m.messages_delivered),
        static_cast<unsigned long long>(events), seconds > 0 ? events / seconds : 0.0,
        to_string(feed.session_state()), book.open_order_count());
    if (q.has_bid && q.has_ask) {
        std::printf(" bid=%.4f ask=%.4f",
                    static_cast<double>(q.bid_price) / static_cast<double>(PRICE_SCALE),
                    static_cast<double>(q.ask_price) / static_cast<double>(PRICE_SCALE));
    }
    std::printf("\n");
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    NetworkConfig net;
    LiveFeedConfig live;
    live.auto_resync = false;  // strict by default on a live feed
    RuntimeTuning tuning;
    double duration = 0;
    double stats_interval = 5.0;
    std::string backend_name = "udp";
    DpdkConfig dpdk;

    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", k.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (k == "--multicast-group") {
            net.multicast_group = next();
        } else if (k == "--port") {
            net.port = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        } else if (k == "--backend") {
            backend_name = next();
        } else if (k == "--dpdk-port") {
            dpdk.port_id = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        } else if (k == "--dpdk-eal") {
            dpdk.eal_args.push_back(next());
        } else if (k == "--interface") {
            net.interface_address = next();
        } else if (k == "--buffer-size") {
            net.recv_buffer_bytes = std::atoi(next().c_str());
        } else if (k == "--batch") {
            net.batch_size = std::atoi(next().c_str());
            live.receive_batch = net.batch_size;
        } else if (k == "--locate") {
            live.stock_locate_filter = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        } else if (k == "--session-epoch") {
            live.session_epoch_ns = std::strtoll(next().c_str(), nullptr, 10);
        } else if (k == "--relaxed-gaps") {
            live.auto_resync = true;
        } else if (k == "--hardware-timestamps") {
            net.hardware_timestamps = true;
        } else if (k == "--busy-poll") {
            net.busy_poll_us = std::atoi(next().c_str());
        } else if (k == "--cpu") {
            tuning.cpu = std::atoi(next().c_str());
        } else if (k == "--lock-memory") {
            tuning.lock_memory = true;
        } else if (k == "--realtime") {
            tuning.realtime = true;
        } else if (k == "--duration") {
            duration = std::atof(next().c_str());
        } else if (k == "--stats-interval") {
            stats_interval = std::atof(next().c_str());
        } else if (k == "--help" || k == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n", k.c_str());
            usage();
            return 2;
        }
    }

    if (net.multicast_group.empty() || net.port == 0) {
        std::fprintf(stderr, "--multicast-group and --port are required\n\n");
        usage();
        return 2;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const TuningReport report = apply_tuning(tuning);
    TscClock::instance().calibrate(200);
    std::printf("platform: %s\n", describe_runtime_support().c_str());
    for (const std::string& note : report.notes) std::printf("runtime : %s\n", note.c_str());
    std::printf("backend : udp hardware timestamps %s, batched receive %s; dpdk %s\n",
                LinuxUdpBackend::supports_hardware_timestamps() ? "available" : "unavailable",
                LinuxUdpBackend::supports_batched_receive() ? "available" : "unavailable",
                DpdkBackend::available() ? "built in" : "not built (see docs/dpdk.md)");

    // The backend is the only thing that differs between the kernel and the
    // poll-mode receive paths. Everything above it - MoldUDP64, the ITCH
    // parser, MarketEvent, the book - is identical, which is the whole reason
    // it sits behind an interface.
    std::unique_ptr<NetworkBackend> backend;
    if (backend_name == "dpdk") {
        auto d = std::unique_ptr<DpdkBackend>(new DpdkBackend());
        dpdk.multicast_group = net.multicast_group;
        dpdk.udp_port = net.port;
        dpdk.hardware_timestamps = net.hardware_timestamps;
        d->set_dpdk_config(dpdk);
        backend = std::move(d);
    } else if (backend_name == "udp") {
        backend = std::unique_ptr<NetworkBackend>(new LinuxUdpBackend());
    } else {
        std::fprintf(stderr, "--backend must be udp or dpdk\n");
        return 2;
    }

    std::string err;
    if (!backend->open(net, &err)) {
        std::fprintf(stderr, "cannot open feed: %s\n", err.c_str());
        return 1;
    }
    std::printf("joined  : %s:%u on interface %s (%s)\n", net.multicast_group.c_str(), net.port,
                net.interface_address.empty() ? "default" : net.interface_address.c_str(),
                backend->name());
    std::printf("SO_RCVBUF requested %d, kernel granted %d\n", net.recv_buffer_bytes,
                backend->stats().effective_recv_buffer);
    std::printf("gap policy: %s\n",
                live.auto_resync ? "RELAXED - resync over gaps, book may be incomplete"
                                 : "STRICT - stop on an unrecoverable gap");

    auto feed = std::unique_ptr<LiveFeed>(new LiveFeed());
    feed->attach(*backend, live);
    auto book = std::unique_ptr<LowLatencyOrderBook>(new LowLatencyOrderBook(BookConfig{}));
    strategy::QuoteObserver observer;
    strategy::MarketState state;

    const std::uint64_t start = now_ns();
    std::uint64_t next_stats = start + static_cast<std::uint64_t>(stats_interval * 1e9);
    std::uint64_t events = 0;
    MarketEvent ev;

    // Single thread, spinning. See runtime/live_engine.hpp for why there is no
    // second thread here until a measurement asks for one.
    while (!g_stop.load(std::memory_order_relaxed)) {
        while (feed->next_event(ev)) {
            book->apply(ev);
            state.event = &ev;
            state.quote = book->best();
            state.event_index = events;
            state.book_healthy = feed->healthy();
            observer.on_market_update(state);
            ++events;
        }

        if (!feed->healthy()) {
            std::printf(
                "\nSEQUENCE GAP: %llu messages missing from sequence %llu. State is %s.\n"
                "Delivery is stopped so the book cannot be silently wrong. Provide a "
                "GapRecoveryPolicy for your venue's retransmission service, or rerun with "
                "--relaxed-gaps to continue with a knowingly incomplete book.\n",
                static_cast<unsigned long long>(feed->missing_count()),
                static_cast<unsigned long long>(feed->first_missing_sequence()),
                to_string(feed->session_state()));
            break;
        }

        const std::uint64_t now = now_ns();
        if (stats_interval > 0 && now >= next_stats) {
            print_stats(*feed, *book, events, static_cast<double>(now - start) / 1e9);
            next_stats = now + static_cast<std::uint64_t>(stats_interval * 1e9);
        }
        if (duration > 0 && static_cast<double>(now - start) / 1e9 >= duration) break;
        if (book->degraded()) {
            std::printf("\nBOOK DEGRADED: %llu events dropped for capacity. Stopping.\n",
                        static_cast<unsigned long long>(book->metrics().capacity_rejections));
            break;
        }
    }

    const double seconds = static_cast<double>(now_ns() - start) / 1e9;
    std::printf("\n--- final ---\n");
    print_stats(*feed, *book, events, seconds);
    const NetworkStats& n = feed->network_stats();
    std::printf("network : packets=%llu bytes=%llu kernel_drops=%llu errors=%llu "
                "would_block=%llu truncated=%llu\n",
                static_cast<unsigned long long>(n.packets),
                static_cast<unsigned long long>(n.bytes),
                static_cast<unsigned long long>(n.kernel_drops),
                static_cast<unsigned long long>(n.errors),
                static_cast<unsigned long long>(n.would_block),
                static_cast<unsigned long long>(n.truncated));
    const MoldStats& m = feed->mold_stats();
    std::printf("mold    : delivered=%llu gaps=%llu missed=%llu duplicates=%llu malformed=%llu "
                "blocked=%llu\n",
                static_cast<unsigned long long>(m.messages_delivered),
                static_cast<unsigned long long>(m.gap_events),
                static_cast<unsigned long long>(m.messages_missed),
                static_cast<unsigned long long>(m.duplicate_packets),
                static_cast<unsigned long long>(m.malformed_packets),
                static_cast<unsigned long long>(m.blocked_packets));
    std::printf("strategy: %llu updates, %llu quote changes\n",
                static_cast<unsigned long long>(observer.events()),
                static_cast<unsigned long long>(observer.quote_changes()));

    backend->close();
    return feed->healthy() ? 0 : 3;
}
