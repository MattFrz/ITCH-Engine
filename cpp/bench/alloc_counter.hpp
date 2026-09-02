#pragma once

// Global allocation counter for the benchmarks.
//
// "Zero allocations on the hot path" is a claim, and a claim needs a
// measurement. Replacing the global operators is the only way to catch an
// allocation that happens three layers down inside something that looked
// innocent. Two integer increments per call is small enough not to distort the
// thing being measured, and the hot path is supposed to be calling this zero
// times anyway - which is exactly what the benchmark asserts.
//
// Benchmarks only. Nothing in the library or the tools includes this.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace bench {

struct AllocStats {
    std::uint64_t allocations = 0;
    std::uint64_t deallocations = 0;
    std::uint64_t bytes = 0;
};

inline AllocStats& alloc_stats() {
    static AllocStats s;
    return s;
}

inline void reset_alloc_stats() { alloc_stats() = AllocStats{}; }

// Snapshot/restore pair so a measured region can be isolated from setup.
class AllocScope {
public:
    AllocScope() : before_(alloc_stats()) {}
    AllocStats delta() const {
        const AllocStats& now = alloc_stats();
        AllocStats d;
        d.allocations = now.allocations - before_.allocations;
        d.deallocations = now.deallocations - before_.deallocations;
        d.bytes = now.bytes - before_.bytes;
        return d;
    }

private:
    AllocStats before_;
};

}  // namespace bench

inline void* operator new(std::size_t n) {
    ++bench::alloc_stats().allocations;
    bench::alloc_stats().bytes += n;
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}

inline void* operator new[](std::size_t n) { return operator new(n); }

inline void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    ++bench::alloc_stats().allocations;
    bench::alloc_stats().bytes += n;
    return std::malloc(n == 0 ? 1 : n);
}

inline void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
    return operator new(n, t);
}

inline void operator delete(void* p) noexcept {
    if (p != nullptr) ++bench::alloc_stats().deallocations;
    std::free(p);
}

inline void operator delete[](void* p) noexcept { operator delete(p); }
inline void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
inline void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }
inline void operator delete(void* p, const std::nothrow_t&) noexcept { operator delete(p); }
inline void operator delete[](void* p, const std::nothrow_t&) noexcept { operator delete(p); }
