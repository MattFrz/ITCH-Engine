// Order-book benchmark: research book vs low-latency book, same event stream.
//
// The comparison that matters is not "is the new book fast" but "is it faster
// than the one it is replacing, on the same data, measured the same way". Both
// books are driven through the identical MarketEvent array, warmed up, and
// measured on separate clean and sampled passes.
//
// Reported per book: throughput, ns and TSC ticks per event, the full latency
// distribution out to p99.99, allocations per event, and the resident memory
// the book holds.

#include <cstdio>
#include <string>
#include <vector>

#include "bench_common.hpp"
#include "itch_engine/book/low_latency_book.hpp"
#include "itch_engine/order_book.hpp"
#include "test_support.hpp"

using namespace itch;
using namespace itch::book;
using namespace itch::runtime;

namespace {

struct Result {
    std::string name;
    double ns_per_event = 0;
    double cycles_per_event = 0;
    double events_per_second = 0;
    Percentiles dist;
    double allocations_per_event = 0;
    double bytes_per_event = 0;
    std::uint64_t total_allocations = 0;
    std::size_t memory_bytes = 0;
    std::uint64_t events = 0;
};

// Clean pass: no timer inside the loop, so this is the honest throughput.
template <class Book>
double timed_pass(Book& book, const std::vector<MarketEvent>& events) {
    const std::uint64_t c0 = cycles_begin();
    for (const MarketEvent& ev : events) {
        book.apply(ev);
    }
    const std::uint64_t c1 = cycles_end();
    bench::keep(book);
    return static_cast<double>(c1 - c0);
}

// Sampled pass: a fenced timer pair around every apply(). Slower by
// construction, which is why the mean above comes from the clean pass.
template <class Book>
void sampled_pass(Book& book, const std::vector<MarketEvent>& events, LatencyRecorder& rec) {
    for (const MarketEvent& ev : events) {
        const std::uint64_t t0 = cycles_begin();
        book.apply(ev);
        const std::uint64_t t1 = cycles_end();
        rec.record_delta(t0, t1);
    }
}

template <class Book, class Make>
Result measure(const char* name, Make make, const std::vector<MarketEvent>& events, int repeats) {
    Result r;
    r.name = name;
    r.events = events.size();

    // Warm up: page faults, branch predictors, and an empty book are all
    // one-off costs that would otherwise land in the first measurement.
    {
        Book warm = make();
        timed_pass(warm, events);
    }

    double best_cycles = 0;
    for (int i = 0; i < repeats; ++i) {
        Book book = make();
        // Construction allocates; the measured region must not include it.
        bench::AllocScope scope;
        const double cycles = timed_pass(book, events);
        const bench::AllocStats delta = scope.delta();
        if (best_cycles == 0 || cycles < best_cycles) {
            best_cycles = cycles;
            r.total_allocations = delta.allocations;
            r.allocations_per_event =
                static_cast<double>(delta.allocations) / static_cast<double>(events.size());
            r.bytes_per_event =
                static_cast<double>(delta.bytes) / static_cast<double>(events.size());
            r.memory_bytes = book.memory_bytes();
        }
    }

    r.cycles_per_event = best_cycles / static_cast<double>(events.size());
    r.ns_per_event = bench::cycles_to_ns(r.cycles_per_event);
    r.events_per_second = r.ns_per_event > 0 ? 1e9 / r.ns_per_event : 0;

    {
        Book book = make();
        LatencyRecorder rec(events.size());
        sampled_pass(book, events, rec);
        r.dist = rec.compute();
    }
    return r;
}

// std::map/list/unordered_map have no memory_bytes(); estimate the research
// book's footprint from its node counts instead of pretending it is free.
struct ResearchBookWrapper {
    OrderBook book;
    void apply(const MarketEvent& ev) { book.apply(ev); }
    std::size_t memory_bytes() const {
        // list node (2 pointers + RestingOrder) + unordered_map node
        // (hash, next pointer, key, OrderRef) per resting order, plus a
        // red-black tree node per level. Approximate and stated as such.
        const std::size_t per_order = 2 * sizeof(void*) + 16 + sizeof(void*) + 8 + 32;
        const std::size_t per_level = 3 * sizeof(void*) + 8 + 40;
        return book.open_order_count() * per_order +
               (book.bid_level_count() + book.ask_level_count()) * per_level;
    }
};

void print_result(const Result& r) {
    std::printf("\n%s\n", r.name.c_str());
    std::printf("  throughput          : %10.0f events/s\n", r.events_per_second);
    std::printf("  mean                : %10.1f ns/event   (%.1f TSC ticks)\n", r.ns_per_event,
                r.cycles_per_event);
    std::printf("  allocations         : %10.4f per event  (%llu total, %.1f bytes/event)\n",
                r.allocations_per_event, static_cast<unsigned long long>(r.total_allocations),
                r.bytes_per_event);
    std::printf("  resident book memory: %10.2f MiB\n",
                static_cast<double>(r.memory_bytes) / (1024.0 * 1024.0));
}

}  // namespace

int main(int argc, char** argv) {
    bench::Args args = bench::Args::parse(argc, argv);
    bench::announce(args, "order book benchmark");

    itch_test::GeneratorConfig gcfg;
    gcfg.event_count = args.events;
    const auto events = itch_test::generate_events(gcfg, args.seed);
    std::printf("events        : %zu (synthetic, seed %llu)\n", events.size(),
                static_cast<unsigned long long>(args.seed));
    std::printf("order node    : %d bytes (ITCH_LL_ORDER_PAD32=%d)\n",
                static_cast<int>(sizeof(LLOrder)), ITCH_LL_ORDER_PAD32);

    BookConfig cfg;
    if (args.max_orders) cfg.max_orders = args.max_orders;
    if (args.index_capacity) cfg.index_capacity = args.index_capacity;
    if (args.max_levels) cfg.max_levels = args.max_levels;
    if (args.tick_count) cfg.tick_count = args.tick_count;

    const Result research = measure<ResearchBookWrapper>(
        "research OrderBook (std::map + std::list + std::unordered_map)",
        [] { return ResearchBookWrapper{}; }, events, args.repeats);

    const Result fast = measure<LowLatencyOrderBook>(
        "LowLatencyOrderBook (pools + tick grid + open addressing)",
        [&] { return LowLatencyOrderBook(cfg); }, events, args.repeats);

    print_result(research);
    print_result(fast);

    bench::print_percentile_header();
    bench::print_percentiles("research apply", research.dist);
    bench::print_percentiles("low-latency apply", fast.dist);
    std::printf("\n%-26s %10s %10s %10s %10s %10s %10s %10s\n", "stage (ns)", "p50", "p90", "p99",
                "p99.9", "p99.99", "max", "mean");
    bench::print_percentiles_ns("research apply", research.dist);
    bench::print_percentiles_ns("low-latency apply", fast.dist);

    const double speedup = research.ns_per_event > 0 ? research.ns_per_event / fast.ns_per_event
                                                     : 0.0;
    std::printf("\nspeedup (mean ns/event): %.2fx\n", speedup);
    std::printf("p99 speedup            : %.2fx\n",
                fast.dist.p99 > 0 ? research.dist.p99 / fast.dist.p99 : 0.0);

    // Index behaviour, which is what explains most of the difference.
    {
        LowLatencyOrderBook book(cfg);
        for (const MarketEvent& ev : events) book.apply(ev);
        std::printf("\norder index: capacity=%zu peak_size=%zu avg_probes=%.3f max_probe=%u "
                    "rejected=%llu\n",
                    book.index().capacity(), book.metrics().peak_orders,
                    book.index().average_probes(), book.index().max_probe(),
                    static_cast<unsigned long long>(book.index().rejected()));
        std::printf("levels     : peak=%zu off-grid peak=%zu off-grid lookups=%llu\n",
                    book.metrics().peak_levels, book.metrics().peak_offgrid,
                    static_cast<unsigned long long>(book.metrics().offgrid_lookups));
        std::printf("degraded   : %s (capacity rejections %llu)\n",
                    book.degraded() ? "YES" : "no",
                    static_cast<unsigned long long>(book.metrics().capacity_rejections));
    }

    if (args.json) {
        std::printf(
            "\nJSON {\"events\":%zu,\"research_ns\":%.3f,\"lowlatency_ns\":%.3f,"
            "\"research_p99_cycles\":%.0f,\"lowlatency_p99_cycles\":%.0f,"
            "\"lowlatency_allocs_per_event\":%.6f,\"speedup\":%.3f,\"order_node_bytes\":%d}\n",
            events.size(), research.ns_per_event, fast.ns_per_event, research.dist.p99,
            fast.dist.p99, fast.allocations_per_event, speedup, static_cast<int>(sizeof(LLOrder)));
    }
    return 0;
}
