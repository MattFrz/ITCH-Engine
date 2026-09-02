#pragma once

// Shared benchmark plumbing: argument parsing, reporting, and the measurement
// methodology every benchmark in this directory follows.
//
// Methodology
// -----------
//  * Warm up first. The first pass over a data set pays for page faults, cold
//    branch predictors and an empty book; measuring it measures the warm-up.
//  * Measure throughput on a clean pass with no timer inside the loop, and
//    measure the distribution on a separate pass. A timer around every event
//    changes the loop it is timing; keeping the two apart means the mean is
//    never contaminated by the instrumentation, and the two agreeing is the
//    evidence that the sampled pass is not distorted.
//  * Report the timer overhead alongside the numbers so a reader can tell how
//    much of a small measurement is the measurement.
//  * Report p50 through p99.99 and max. An average alone hides exactly the
//    behaviour a market data path is judged on.
//  * Report allocations per event, because "no allocation on the hot path" is
//    a claim that has to be checked rather than asserted.
//  * Report the tuning state (pinning, TSC invariance) so a number is always
//    attached to the conditions that produced it.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "alloc_counter.hpp"
#include "itch_engine/runtime/affinity.hpp"
#include "itch_engine/runtime/cycles.hpp"
#include "itch_engine/runtime/latency_recorder.hpp"

namespace bench {

struct Args {
    std::size_t events = 2000000;
    std::uint64_t seed = 20260831;
    int cpu = -1;
    bool lock_memory = false;
    bool realtime = false;
    int repeats = 3;
    std::size_t mtu = 1400;
    std::string capture;
    bool json = false;

    // Book capacity, swept because the working-set size turned out to matter
    // more for the tail than any per-operation detail did. 0 = library default.
    std::size_t max_orders = 0;
    std::size_t index_capacity = 0;
    std::size_t max_levels = 0;
    std::size_t tick_count = 0;

    static Args parse(int argc, char** argv) {
        Args a;
        for (int i = 1; i < argc; ++i) {
            const std::string k = argv[i];
            auto next = [&](const char* what) -> const char* {
                if (i + 1 >= argc) {
                    std::fprintf(stderr, "%s needs a value\n", what);
                    std::exit(2);
                }
                return argv[++i];
            };
            if (k == "--events") {
                a.events = std::strtoull(next("--events"), nullptr, 10);
            } else if (k == "--seed") {
                a.seed = std::strtoull(next("--seed"), nullptr, 10);
            } else if (k == "--cpu") {
                a.cpu = std::atoi(next("--cpu"));
            } else if (k == "--repeats") {
                a.repeats = std::atoi(next("--repeats"));
            } else if (k == "--mtu") {
                a.mtu = std::strtoull(next("--mtu"), nullptr, 10);
            } else if (k == "--capture") {
                a.capture = next("--capture");
            } else if (k == "--lock-memory") {
                a.lock_memory = true;
            } else if (k == "--realtime") {
                a.realtime = true;
            } else if (k == "--max-orders") {
                a.max_orders = std::strtoull(next("--max-orders"), nullptr, 10);
            } else if (k == "--index-capacity") {
                a.index_capacity = std::strtoull(next("--index-capacity"), nullptr, 10);
            } else if (k == "--max-levels") {
                a.max_levels = std::strtoull(next("--max-levels"), nullptr, 10);
            } else if (k == "--tick-count") {
                a.tick_count = std::strtoull(next("--tick-count"), nullptr, 10);
            } else if (k == "--json") {
                a.json = true;
            } else if (k == "--help" || k == "-h") {
                std::printf(
                    "options:\n"
                    "  --events N     synthetic events to generate (default 2000000)\n"
                    "  --seed S       generator seed\n"
                    "  --capture P    use a real .itchcap capture instead of synthetic data\n"
                    "  --mtu N        datagram size when building packets (default 1400)\n"
                    "  --cpu N        pin this thread to logical cpu N\n"
                    "  --lock-memory  mlockall (needs privileges; reports if refused)\n"
                    "  --realtime     SCHED_FIFO (needs privileges; reports if refused)\n"
                    "  --repeats N    measured repetitions, best is reported (default 3)\n"
                    "  --json         emit a machine-readable summary as well\n");
                std::exit(0);
            } else {
                std::fprintf(stderr, "unknown option %s (try --help)\n", k.c_str());
                std::exit(2);
            }
        }
        return a;
    }
};

// Applies tuning and prints the conditions the numbers were produced under.
inline itch::runtime::TuningReport announce(const Args& args, const char* title) {
    using namespace itch::runtime;
    RuntimeTuning t;
    t.cpu = args.cpu;
    t.lock_memory = args.lock_memory;
    t.realtime = args.realtime;
    t.prefault_heap = false;
    const TuningReport report = apply_tuning(t);

    TscClock::instance().calibrate(300);
    std::printf("=== %s ===\n", title);
    std::printf("platform      : %s\n", describe_runtime_support().c_str());
    std::printf("tsc           : %.3f GHz, invariant=%s\n",
                TscClock::instance().hz() / 1e9,
                TscClock::instance().invariant() ? "yes" : "NO (prefer the ns columns)");
    std::printf("timer overhead: %llu cycles (lfence;rdtsc / rdtscp;lfence pair)\n",
                static_cast<unsigned long long>(measure_timer_overhead()));
    for (const std::string& note : report.notes) {
        std::printf("runtime       : %s\n", note.c_str());
    }
    return report;
}

inline double cycles_to_ns(double cycles) {
    return itch::runtime::TscClock::instance().cycles_to_ns(cycles);
}

inline void print_percentile_header() {
    std::printf("\n%-26s %10s %10s %10s %10s %10s %10s %10s\n", "stage (cycles)", "p50", "p90",
                "p99", "p99.9", "p99.99", "max", "mean");
}

inline void print_percentiles(const char* label, const itch::runtime::Percentiles& p) {
    std::printf("%-26s %10.0f %10.0f %10.0f %10.0f %10.0f %10.0f %10.1f\n", label, p.p50, p.p90,
                p.p99, p.p999, p.p9999, p.max, p.mean);
}

inline void print_percentiles_ns(const char* label, const itch::runtime::Percentiles& p) {
    std::printf("%-26s %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f %10.2f\n", label,
                cycles_to_ns(p.p50), cycles_to_ns(p.p90), cycles_to_ns(p.p99),
                cycles_to_ns(p.p999), cycles_to_ns(p.p9999), cycles_to_ns(p.max),
                cycles_to_ns(p.mean));
}

// Keeps the optimizer from deleting work whose result is otherwise unused.
template <class T>
inline void keep(const T& value) {
    static volatile const void* sink;
    sink = static_cast<const void*>(&value);
    (void)sink;
}

}  // namespace bench
