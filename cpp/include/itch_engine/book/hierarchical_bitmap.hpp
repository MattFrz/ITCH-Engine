#pragma once

// Three-level occupancy bitmap over a tick grid.
//
// The book needs four things from its price-level container: "is this price
// live", "what is the best price", "what is the next price down/up", and
// insert/erase. A std::map answers all four in O(log L) with a pointer chase
// per comparison. A flat array answers the first in one load but cannot answer
// the other three without scanning.
//
// This bitmap fixes the scan. Level 0 holds one bit per tick, level 1 one bit
// per level-0 word, level 2 one bit per level-1 word. Finding the highest set
// bit over a 2^20-tick grid is then three dependent loads and three bit-scans,
// with the top level small enough (4 words for 2^20 ticks) to scan directly.
// Insert and erase are one OR, or one AND plus at most two more when a word
// empties out.
//
// Memory for a 2^20-tick grid: 128 KiB + 2 KiB + 32 B per side. The pages
// actually touched are the ones near the touch, which is the whole point.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace itch {
namespace book {

namespace detail {

inline unsigned highest_bit(std::uint64_t w) noexcept {
#if defined(_MSC_VER)
    unsigned long idx = 0;
    _BitScanReverse64(&idx, w);
    return static_cast<unsigned>(idx);
#else
    return 63u - static_cast<unsigned>(__builtin_clzll(w));
#endif
}

inline unsigned lowest_bit(std::uint64_t w) noexcept {
#if defined(_MSC_VER)
    unsigned long idx = 0;
    _BitScanForward64(&idx, w);
    return static_cast<unsigned>(idx);
#else
    return static_cast<unsigned>(__builtin_ctzll(w));
#endif
}

}  // namespace detail

class HierarchicalBitmap {
public:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    void reset(std::size_t bits) {
        bits_ = round_up(bits, 64);
        const std::size_t l0 = bits_ / 64;
        const std::size_t l1 = round_up(l0, 64) / 64;
        const std::size_t l2 = round_up(l1, 64) / 64;
        l0_.assign(l0, 0);
        l1_.assign(l1 == 0 ? 1 : l1, 0);
        l2_.assign(l2 == 0 ? 1 : l2, 0);
        count_ = 0;
    }

    void clear() noexcept {
        if (count_ == 0) return;
        std::fill(l0_.begin(), l0_.end(), 0ull);
        std::fill(l1_.begin(), l1_.end(), 0ull);
        std::fill(l2_.begin(), l2_.end(), 0ull);
        count_ = 0;
    }

    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }
    std::size_t bits() const noexcept { return bits_; }

    std::size_t memory_bytes() const noexcept {
        return (l0_.capacity() + l1_.capacity() + l2_.capacity()) * sizeof(std::uint64_t);
    }

    bool test(std::size_t i) const noexcept {
        return (l0_[i >> 6] >> (i & 63)) & 1ull;
    }

    void set(std::size_t i) noexcept {
        const std::size_t w0 = i >> 6;
        const std::uint64_t b0 = 1ull << (i & 63);
        if (l0_[w0] & b0) return;
        l0_[w0] |= b0;
        const std::size_t w1 = w0 >> 6;
        l1_[w1] |= 1ull << (w0 & 63);
        l2_[w1 >> 6] |= 1ull << (w1 & 63);
        ++count_;
    }

    void reset_bit(std::size_t i) noexcept {
        const std::size_t w0 = i >> 6;
        const std::uint64_t b0 = 1ull << (i & 63);
        if ((l0_[w0] & b0) == 0) return;
        l0_[w0] &= ~b0;
        --count_;
        if (l0_[w0] != 0) return;
        const std::size_t w1 = w0 >> 6;
        l1_[w1] &= ~(1ull << (w0 & 63));
        if (l1_[w1] != 0) return;
        l2_[w1 >> 6] &= ~(1ull << (w1 & 63));
    }

    // Highest set bit, or kNone.
    std::size_t highest() const noexcept {
        if (count_ == 0) return kNone;
        for (std::size_t w2 = l2_.size(); w2-- > 0;) {
            if (l2_[w2] == 0) continue;
            const std::size_t w1 = (w2 << 6) + detail::highest_bit(l2_[w2]);
            const std::size_t w0 = (w1 << 6) + detail::highest_bit(l1_[w1]);
            return (w0 << 6) + detail::highest_bit(l0_[w0]);
        }
        return kNone;
    }

    // Lowest set bit, or kNone.
    std::size_t lowest() const noexcept {
        if (count_ == 0) return kNone;
        for (std::size_t w2 = 0; w2 < l2_.size(); ++w2) {
            if (l2_[w2] == 0) continue;
            const std::size_t w1 = (w2 << 6) + detail::lowest_bit(l2_[w2]);
            const std::size_t w0 = (w1 << 6) + detail::lowest_bit(l1_[w1]);
            return (w0 << 6) + detail::lowest_bit(l0_[w0]);
        }
        return kNone;
    }

    // Highest set bit strictly below `i`, or kNone. Used to walk bid depth.
    std::size_t prev_below(std::size_t i) const noexcept {
        if (i == 0 || count_ == 0) return kNone;
        std::size_t w0 = (i - 1) >> 6;
        std::uint64_t word = l0_[w0] & mask_below_or_equal((i - 1) & 63);
        if (word != 0) return (w0 << 6) + detail::highest_bit(word);

        // Climb: find a lower non-empty level-0 word via level 1, then level 2.
        std::size_t w1 = w0 >> 6;
        if (w0 != 0) {
            const std::size_t b1 = w0 & 63;
            std::uint64_t w1word = b1 == 0 ? 0ull : (l1_[w1] & mask_below_or_equal(b1 - 1));
            if (w1word != 0) {
                const std::size_t nw0 = (w1 << 6) + detail::highest_bit(w1word);
                return (nw0 << 6) + detail::highest_bit(l0_[nw0]);
            }
        }
        std::size_t w2 = w1 >> 6;
        {
            const std::size_t b2 = w1 & 63;
            std::uint64_t w2word = b2 == 0 ? 0ull : (l2_[w2] & mask_below_or_equal(b2 - 1));
            if (w2word != 0) {
                const std::size_t nw1 = (w2 << 6) + detail::highest_bit(w2word);
                const std::size_t nw0 = (nw1 << 6) + detail::highest_bit(l1_[nw1]);
                return (nw0 << 6) + detail::highest_bit(l0_[nw0]);
            }
        }
        while (w2-- > 0) {
            if (l2_[w2] == 0) continue;
            const std::size_t nw1 = (w2 << 6) + detail::highest_bit(l2_[w2]);
            const std::size_t nw0 = (nw1 << 6) + detail::highest_bit(l1_[nw1]);
            return (nw0 << 6) + detail::highest_bit(l0_[nw0]);
        }
        return kNone;
    }

    // Lowest set bit strictly above `i`, or kNone. Used to walk ask depth.
    std::size_t next_above(std::size_t i) const noexcept {
        if (count_ == 0 || i + 1 >= bits_) return kNone;
        const std::size_t start = i + 1;
        std::size_t w0 = start >> 6;
        std::uint64_t word = l0_[w0] & mask_above_or_equal(start & 63);
        if (word != 0) return (w0 << 6) + detail::lowest_bit(word);

        std::size_t w1 = w0 >> 6;
        {
            const std::size_t b1 = w0 & 63;
            std::uint64_t w1word = b1 == 63 ? 0ull : (l1_[w1] & mask_above_or_equal(b1 + 1));
            if (w1word != 0) {
                const std::size_t nw0 = (w1 << 6) + detail::lowest_bit(w1word);
                return (nw0 << 6) + detail::lowest_bit(l0_[nw0]);
            }
        }
        std::size_t w2 = w1 >> 6;
        {
            const std::size_t b2 = w1 & 63;
            std::uint64_t w2word = b2 == 63 ? 0ull : (l2_[w2] & mask_above_or_equal(b2 + 1));
            if (w2word != 0) {
                const std::size_t nw1 = (w2 << 6) + detail::lowest_bit(w2word);
                const std::size_t nw0 = (nw1 << 6) + detail::lowest_bit(l1_[nw1]);
                return (nw0 << 6) + detail::lowest_bit(l0_[nw0]);
            }
        }
        for (++w2; w2 < l2_.size(); ++w2) {
            if (l2_[w2] == 0) continue;
            const std::size_t nw1 = (w2 << 6) + detail::lowest_bit(l2_[w2]);
            const std::size_t nw0 = (nw1 << 6) + detail::lowest_bit(l1_[nw1]);
            return (nw0 << 6) + detail::lowest_bit(l0_[nw0]);
        }
        return kNone;
    }

private:
    static std::size_t round_up(std::size_t v, std::size_t m) noexcept {
        return v == 0 ? m : ((v + m - 1) / m) * m;
    }
    static std::uint64_t mask_below_or_equal(std::size_t bit) noexcept {
        return bit >= 63 ? ~0ull : ((1ull << (bit + 1)) - 1ull);
    }
    static std::uint64_t mask_above_or_equal(std::size_t bit) noexcept {
        return bit == 0 ? ~0ull : (~0ull << bit);
    }

    std::vector<std::uint64_t> l0_;
    std::vector<std::uint64_t> l1_;
    std::vector<std::uint64_t> l2_;
    std::size_t bits_ = 0;
    std::size_t count_ = 0;
};

}  // namespace book
}  // namespace itch
