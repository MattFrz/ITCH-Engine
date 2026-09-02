#pragma once

// Exact latency percentiles from a preallocated sample buffer.
//
// Why raw samples and not a bucketed histogram: the tail is the interesting
// part, and p99.99 out of ten million samples is the 1,000th worst value.
// Bucketing rounds exactly the numbers that matter. Recording is one bounds
// check and one 4-byte store into memory that was allocated and touched before
// the measurement began, which is about as little as a measurement can perturb
// the thing it measures. Sorting happens after the run, never during.
//
// Samples are stored as uint32 TSC ticks. At ~3 GHz that saturates around 1.4
// seconds, far past anything meaningful here, and saturation is counted.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace itch {
namespace runtime {

struct Percentiles {
    std::uint64_t count = 0;
    double min = 0;
    double p50 = 0;
    double p90 = 0;
    double p99 = 0;
    double p999 = 0;
    double p9999 = 0;
    double max = 0;
    double mean = 0;
    std::uint64_t saturated = 0;  // samples clamped at uint32 max
    std::uint64_t discarded = 0;  // non-monotonic reads (thread migration)
};

class LatencyRecorder {
public:
    LatencyRecorder() = default;
    explicit LatencyRecorder(std::size_t capacity) { reserve(capacity); }

    // Allocates and first-touches the whole buffer so the measured loop never
    // takes a page fault attributable to the recorder itself.
    void reserve(std::size_t capacity) {
        samples_.assign(capacity, 0u);
        n_ = 0;
        saturated_ = 0;
        discarded_ = 0;
    }

    void clear() noexcept {
        n_ = 0;
        saturated_ = 0;
        discarded_ = 0;
    }

    std::size_t size() const noexcept { return n_; }
    std::size_t capacity() const noexcept { return samples_.size(); }
    bool full() const noexcept { return n_ >= samples_.size(); }

    void record(std::uint64_t ticks) noexcept {
        if (n_ >= samples_.size()) return;
        if (ticks > 0xFFFFFFFFull) {
            ticks = 0xFFFFFFFFull;
            ++saturated_;
        }
        samples_[n_++] = static_cast<std::uint32_t>(ticks);
    }

    // Records end-begin, discarding the sample if the counter went backwards
    // (a thread that migrated across sockets mid-measurement). Counted, never
    // silently folded into the distribution.
    void record_delta(std::uint64_t begin, std::uint64_t end) noexcept {
        if (end < begin) {
            ++discarded_;
            return;
        }
        record(end - begin);
    }

    Percentiles compute() const {
        Percentiles p;
        p.count = n_;
        p.saturated = saturated_;
        p.discarded = discarded_;
        if (n_ == 0) return p;
        std::vector<std::uint32_t> sorted(samples_.begin(), samples_.begin() + n_);
        std::sort(sorted.begin(), sorted.end());
        long double sum = 0;
        for (std::uint32_t v : sorted) sum += v;
        p.mean = static_cast<double>(sum / static_cast<long double>(n_));
        p.min = sorted.front();
        p.max = sorted.back();
        p.p50 = quantile(sorted, 0.50);
        p.p90 = quantile(sorted, 0.90);
        p.p99 = quantile(sorted, 0.99);
        p.p999 = quantile(sorted, 0.999);
        p.p9999 = quantile(sorted, 0.9999);
        return p;
    }

private:
    static double quantile(const std::vector<std::uint32_t>& sorted, double q) {
        if (sorted.empty()) return 0;
        // Nearest-rank: the smallest value at or above the q-th fraction. No
        // interpolation, so a reported p99.9 is a value that actually occurred.
        std::size_t idx = static_cast<std::size_t>(q * static_cast<double>(sorted.size()));
        if (idx >= sorted.size()) idx = sorted.size() - 1;
        return sorted[idx];
    }

    std::vector<std::uint32_t> samples_;
    std::size_t n_ = 0;
    std::uint64_t saturated_ = 0;
    std::uint64_t discarded_ = 0;
};

}  // namespace runtime
}  // namespace itch
