#pragma once

// Endian-correct fixed-width loads/stores for wire formats.
//
// MoldUDP64 and Nasdaq ITCH are both big-endian with no padding and no
// alignment guarantees, so every field is read through memcpy + an explicit
// byte swap. memcpy of a compile-time-constant size is folded to a single
// unaligned load by every compiler this project targets; taking a
// reinterpret_cast<const uint32_t*> instead would be UB and would fault on
// strict-alignment targets.

#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
#include <cstdlib>
#endif

namespace itch {

inline std::uint16_t bswap16(std::uint16_t v) noexcept {
#if defined(_MSC_VER)
    return _byteswap_ushort(v);
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap16(v);
#else
    return static_cast<std::uint16_t>((v >> 8) | (v << 8));
#endif
}

inline std::uint32_t bswap32(std::uint32_t v) noexcept {
#if defined(_MSC_VER)
    return _byteswap_ulong(v);
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(v);
#else
    return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
#endif
}

inline std::uint64_t bswap64(std::uint64_t v) noexcept {
#if defined(_MSC_VER)
    return _byteswap_uint64(v);
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(v);
#else
    return (static_cast<std::uint64_t>(bswap32(static_cast<std::uint32_t>(v))) << 32) |
           bswap32(static_cast<std::uint32_t>(v >> 32));
#endif
}

inline std::uint8_t load_be8(const std::uint8_t* p) noexcept { return *p; }

inline std::uint16_t load_be16(const std::uint8_t* p) noexcept {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return bswap16(v);
}

inline std::uint32_t load_be32(const std::uint8_t* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return bswap32(v);
}

inline std::uint64_t load_be64(const std::uint8_t* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    return bswap64(v);
}

// ITCH timestamps are 48-bit big-endian nanoseconds since midnight.
inline std::uint64_t load_be48(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    std::memcpy(reinterpret_cast<std::uint8_t*>(&v) + 2, p, 6);
    return bswap64(v);
}

inline void store_be16(std::uint8_t* p, std::uint16_t v) noexcept {
    const std::uint16_t b = bswap16(v);
    std::memcpy(p, &b, sizeof(b));
}

inline void store_be32(std::uint8_t* p, std::uint32_t v) noexcept {
    const std::uint32_t b = bswap32(v);
    std::memcpy(p, &b, sizeof(b));
}

inline void store_be64(std::uint8_t* p, std::uint64_t v) noexcept {
    const std::uint64_t b = bswap64(v);
    std::memcpy(p, &b, sizeof(b));
}

inline void store_be48(std::uint8_t* p, std::uint64_t v) noexcept {
    const std::uint64_t b = bswap64(v);
    std::memcpy(p, reinterpret_cast<const std::uint8_t*>(&b) + 2, 6);
}

}  // namespace itch
