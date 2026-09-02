#pragma once

// Fixed-capacity open-addressing hash map: OrderId (uint64) -> uint32 slot.
//
// Replaces std::unordered_map on the low-latency path. The research book keeps
// its unordered_map; this is measured against it in cpp/bench/bench_book.cpp,
// which is the only reason it exists.
//
// Design notes, each of which is a deliberate choice against the default:
//
//   * Structure-of-arrays. Probing touches only the key array, so one cache
//     line carries 8 candidate keys instead of 5 (a packed 12-byte AoS entry
//     would straddle lines and waste a third of every fetched line). Values
//     are read once, on a hit.
//
//   * Linear probing with backward-shift deletion (Knuth 6.4 algorithm R),
//     not tombstones. An ITCH day is roughly half cancels, so a tombstoned
//     table degrades all day and needs periodic rehashing - which is exactly
//     the unbounded pause a hot path must not have.
//
//   * Fibonacci hashing: one 64-bit multiply and a shift. ITCH order
//     references are near-sequential, which is the case where the identity
//     hash of a power-of-two table clusters badly and where multiplicative
//     hashing spreads cleanly.
//
//   * Allocated once, at construction. No rehash, ever. Exceeding the load
//     factor is a reported failure, not a silent reallocation.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "itch_engine/types.hpp"

namespace itch {
namespace book {

class OrderIndex {
public:
    static constexpr std::uint32_t kNull = 0xFFFFFFFFu;
    // Reserved as the empty marker. An order reference equal to it is rejected
    // and counted rather than corrupting the table; Nasdaq order references
    // are assigned sequentially from 1, so this cannot occur in practice.
    static constexpr OrderId kEmptyKey = ~OrderId(0);

    OrderIndex() = default;
    explicit OrderIndex(std::size_t capacity) { reset(capacity); }

    // `capacity` is rounded up to a power of two. Max load factor is 0.5, so
    // pass at least 2x the peak live order count.
    void reset(std::size_t capacity) {
        std::size_t cap = 16;
        while (cap < capacity) cap <<= 1;
        capacity_ = cap;
        mask_ = cap - 1;
        shift_ = 64;
        for (std::size_t c = cap; c > 1; c >>= 1) --shift_;
        keys_.assign(cap, kEmptyKey);
        values_.assign(cap, kNull);
        size_ = 0;
        max_load_ = cap / 2;
        probes_ = 0;
        lookups_ = 0;
        max_probe_ = 0;
        rejected_ = 0;
    }

    void clear() noexcept {
        if (size_ == 0) return;
        keys_.assign(capacity_, kEmptyKey);
        values_.assign(capacity_, kNull);
        size_ = 0;
    }

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t max_load() const noexcept { return max_load_; }
    bool full() const noexcept { return size_ >= max_load_; }

    // Instrumentation. Counted in every build: two increments per lookup are
    // invisible next to the memory access, and the benchmark reports probe
    // distribution, which is the number that actually explains the timing.
    std::uint64_t lookups() const noexcept { return lookups_; }
    std::uint64_t probes() const noexcept { return probes_; }
    std::uint32_t max_probe() const noexcept { return max_probe_; }
    std::uint64_t rejected() const noexcept { return rejected_; }
    double average_probes() const noexcept {
        return lookups_ == 0 ? 0.0 : static_cast<double>(probes_) / static_cast<double>(lookups_);
    }

    std::uint32_t find(OrderId id) const noexcept {
        ++lookups_;
        std::size_t i = home(id);
        std::uint32_t probe = 0;
        for (;;) {
            const OrderId k = keys_[i];
            ++probe;
            if (k == id) {
                probes_ += probe;
                if (probe > max_probe_) max_probe_ = probe;
                return values_[i];
            }
            if (k == kEmptyKey) {
                probes_ += probe;
                if (probe > max_probe_) max_probe_ = probe;
                return kNull;
            }
            i = (i + 1) & mask_;
        }
    }

    // Returns false if the key is already present or the table is at its load
    // limit. Never grows.
    bool insert(OrderId id, std::uint32_t value) noexcept {
        if (id == kEmptyKey) {
            ++rejected_;
            return false;
        }
        if (size_ >= max_load_) {
            ++rejected_;
            return false;
        }
        std::size_t i = home(id);
        for (;;) {
            const OrderId k = keys_[i];
            if (k == kEmptyKey) {
                keys_[i] = id;
                values_[i] = value;
                ++size_;
                return true;
            }
            if (k == id) return false;
            i = (i + 1) & mask_;
        }
    }

    // Overwrites an existing key, or inserts it.
    bool insert_or_assign(OrderId id, std::uint32_t value) noexcept {
        if (id == kEmptyKey) {
            ++rejected_;
            return false;
        }
        std::size_t i = home(id);
        for (;;) {
            const OrderId k = keys_[i];
            if (k == id) {
                values_[i] = value;
                return true;
            }
            if (k == kEmptyKey) {
                if (size_ >= max_load_) {
                    ++rejected_;
                    return false;
                }
                keys_[i] = id;
                values_[i] = value;
                ++size_;
                return true;
            }
            i = (i + 1) & mask_;
        }
    }

    bool erase(OrderId id) noexcept {
        std::size_t i = home(id);
        for (;;) {
            const OrderId k = keys_[i];
            if (k == id) break;
            if (k == kEmptyKey) return false;
            i = (i + 1) & mask_;
        }
        // Backward-shift: pull up any later entry whose home position is at or
        // before the hole, so every probe chain stays unbroken without a
        // tombstone.
        std::size_t hole = i;
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            const OrderId k = keys_[j];
            if (k == kEmptyKey) break;
            const std::size_t h = home(k);
            // Is h cyclically outside (hole, j]? Then k may move to the hole.
            const bool movable = (hole <= j) ? (h <= hole || h > j) : (h <= hole && h > j);
            if (movable) {
                keys_[hole] = k;
                values_[hole] = values_[j];
                hole = j;
            }
        }
        keys_[hole] = kEmptyKey;
        values_[hole] = kNull;
        --size_;
        return true;
    }

    // Test/diagnostic hook: walks the whole table.
    template <class Fn>
    void for_each(Fn&& fn) const {
        for (std::size_t i = 0; i < capacity_; ++i) {
            if (keys_[i] != kEmptyKey) fn(keys_[i], values_[i]);
        }
    }

    std::size_t memory_bytes() const noexcept {
        return keys_.capacity() * sizeof(OrderId) + values_.capacity() * sizeof(std::uint32_t);
    }

private:
    std::size_t home(OrderId id) const noexcept {
        // Fibonacci hashing: the high bits of id * 2^64/phi are well mixed even
        // for sequential ids, which is exactly the ITCH case.
        return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> shift_) & mask_;
    }

    std::vector<OrderId> keys_;
    std::vector<std::uint32_t> values_;
    std::size_t capacity_ = 0;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
    std::size_t max_load_ = 0;
    unsigned shift_ = 64;

    mutable std::uint64_t lookups_ = 0;
    mutable std::uint64_t probes_ = 0;
    mutable std::uint32_t max_probe_ = 0;
    std::uint64_t rejected_ = 0;
};

}  // namespace book
}  // namespace itch
