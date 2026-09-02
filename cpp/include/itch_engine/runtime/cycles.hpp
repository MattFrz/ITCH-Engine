#pragma once

// CPU timestamp-counter access and calibration.
//
// Methodology, stated up front because cycle numbers are easy to fake:
//
//   * `cycles_begin()` uses LFENCE;RDTSC and `cycles_end()` uses RDTSCP;LFENCE.
//     RDTSC on its own is not ordered against surrounding instructions, so a
//     naive pair can and does measure the wrong window. The fences pin the
//     region; the cost of the fence pair is measured and reported by the
//     benchmark as `timer_overhead_cycles` and subtracted where noted.
//
//   * On every CPU this project targets, the TSC is invariant (constant rate,
//     unaffected by P-states and C-states). That means TSC ticks are a stable
//     *time* unit but NOT a count of retired core clock cycles: at a boosted
//     core frequency more core cycles pass per TSC tick than at base. So
//     "cycles/event" here means TSC ticks per event, converted to nanoseconds
//     with a measured TSC frequency. It is honest as a latency unit and it is
//     NOT a claim about IPC or core clocks. `has_invariant_tsc()` reports
//     whether the CPU advertises the guarantee; when it does not, prefer the
//     nanosecond numbers.
//
//   * TSC is per-socket-synchronised on modern hardware but a thread migrating
//     between sockets can still read backwards. Benchmarks pin themselves
//     (runtime/affinity.hpp) and the histogram drops negative deltas, counting
//     them so a broken measurement is visible rather than silent.
//
//   * Frequency scaling and SMT siblings change the *distribution*, not the
//     clock. The tuning guidance in docs/linux_tuning.md exists so the numbers
//     are reproducible; without it, expect a fatter tail.

#include <chrono>
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define ITCH_HAS_TSC 1
#else
#define ITCH_HAS_TSC 0
#endif

namespace itch {
namespace runtime {

#if ITCH_HAS_TSC

inline std::uint64_t cycles_begin() noexcept {
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

inline std::uint64_t cycles_end() noexcept {
    unsigned aux = 0;
    const std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

// Unserialised read, for coarse aggregate timing where the fence pair would
// itself dominate (for example around a whole packet, not one field decode).
inline std::uint64_t cycles_now() noexcept { return __rdtsc(); }

inline bool has_tsc() noexcept { return true; }

inline bool has_invariant_tsc() noexcept {
    int regs[4] = {0, 0, 0, 0};
#if defined(_MSC_VER)
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) < 0x80000007u) return false;
    __cpuid(regs, 0x80000007);
#else
    __asm__ __volatile__("cpuid"
                         : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
                         : "a"(0x80000000), "c"(0));
    if (static_cast<unsigned>(regs[0]) < 0x80000007u) return false;
    __asm__ __volatile__("cpuid"
                         : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
                         : "a"(0x80000007), "c"(0));
#endif
    return (regs[3] & (1 << 8)) != 0;  // EDX bit 8: invariant TSC
}

#else  // no TSC: fall back to the steady clock so the code still runs.

inline std::uint64_t cycles_begin() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}
inline std::uint64_t cycles_end() noexcept { return cycles_begin(); }
inline std::uint64_t cycles_now() noexcept { return cycles_begin(); }
inline bool has_tsc() noexcept { return false; }
inline bool has_invariant_tsc() noexcept { return false; }

#endif

// Measures TSC ticks per second against the steady clock. `ms` is the
// calibration window; longer is more accurate and 200 ms is enough to land
// inside 0.1% on a quiet machine.
inline double measure_tsc_hz(int ms = 200) noexcept {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const std::uint64_t c0 = cycles_begin();
    const auto deadline = t0 + std::chrono::milliseconds(ms);
    while (clock::now() < deadline) {
        // Busy wait: sleeping would let the core drop into a C-state and, on
        // some parts, distort the very first samples after the wake-up.
    }
    const std::uint64_t c1 = cycles_end();
    const auto t1 = clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    if (secs <= 0.0) return 0.0;
    return static_cast<double>(c1 - c0) / secs;
}

// Cached calibration. Call once at startup, before pinning matters.
class TscClock {
public:
    static TscClock& instance() {
        static TscClock c;
        return c;
    }

    void calibrate(int ms = 200) {
        hz_ = measure_tsc_hz(ms);
        invariant_ = has_invariant_tsc();
    }

    double hz() const noexcept { return hz_; }
    bool invariant() const noexcept { return invariant_; }

    double cycles_to_ns(double cycles) const noexcept {
        return hz_ > 0.0 ? cycles * 1e9 / hz_ : 0.0;
    }
    double ns_to_cycles(double ns) const noexcept { return hz_ > 0.0 ? ns * hz_ / 1e9 : 0.0; }

private:
    TscClock() { calibrate(); }
    double hz_ = 0.0;
    bool invariant_ = false;
};

// Measures the cost of the cycles_begin/cycles_end pair itself, so it can be
// reported alongside (and subtracted from) short measurements.
inline std::uint64_t measure_timer_overhead(int iterations = 20000) noexcept {
    std::uint64_t best = ~std::uint64_t(0);
    for (int i = 0; i < iterations; ++i) {
        const std::uint64_t a = cycles_begin();
        const std::uint64_t b = cycles_end();
        if (b > a && (b - a) < best) best = b - a;
    }
    return best == ~std::uint64_t(0) ? 0 : best;
}

}  // namespace runtime
}  // namespace itch
