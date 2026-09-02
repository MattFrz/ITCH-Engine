#pragma once

// Packet capture file: a sequence of MoldUDP64 datagrams exactly as a socket
// would hand them over, plus the arrival timestamp each one had on the
// original day.
//
// This is a local development artifact, not a wire format, so its scalars are
// host-endian little-endian and its only job is to make the live pipeline
// runnable without a live feed:
//
//   normalized parquet -> ITCH messages -> MoldUDP64 datagrams -> .itchcap
//                                                                     |
//   UDP socket ------------------------------------------------------>+--> decoder
//
// Layout:
//
//   offset size field
//   ------ ---- --------------------------------------------------------
//        0    8 magic "ITCHCAP1"
//        8    4 version
//       12    4 header_len (64)
//       16    8 session_epoch_ns   midnight of the session, ns since epoch
//       24    8 packet_count       patched on close
//       32    8 message_count      patched on close
//       40   10 mold session id
//       50    8 stock symbol, space padded
//       58    2 stock locate
//       60    4 reserved
//   ------ ---- 64-byte header, then records:
//       uint32 length, int64 capture_ts_ns, length bytes of datagram

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace itch {
namespace feed {

inline constexpr char kCaptureMagic[8] = {'I', 'T', 'C', 'H', 'C', 'A', 'P', '1'};
inline constexpr std::uint32_t kCaptureVersion = 1;
inline constexpr std::size_t kCaptureHeaderLen = 64;
inline constexpr std::size_t kCaptureRecordPrefix = 12;  // uint32 len + int64 ts

struct CaptureHeader {
    std::int64_t session_epoch_ns = 0;
    std::uint64_t packet_count = 0;
    std::uint64_t message_count = 0;
    char session[10] = {'I', 'T', 'C', 'H', 'C', 'A', 'P', ' ', ' ', ' '};
    char stock[8] = {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
    std::uint16_t stock_locate = 1;
};

class CaptureWriter {
public:
    CaptureWriter() = default;
    ~CaptureWriter() { close(); }

    CaptureWriter(const CaptureWriter&) = delete;
    CaptureWriter& operator=(const CaptureWriter&) = delete;

    bool open(const std::string& path, const CaptureHeader& hdr) {
        close();
        f_ = std::fopen(path.c_str(), "wb");
        if (f_ == nullptr) return false;
        hdr_ = hdr;
        std::uint8_t head[kCaptureHeaderLen] = {};
        write_header_bytes(head, hdr_);
        return std::fwrite(head, 1, kCaptureHeaderLen, f_) == kCaptureHeaderLen;
    }

    bool write_packet(const std::uint8_t* data, std::size_t len, std::int64_t capture_ts_ns,
                      std::uint32_t messages) {
        if (f_ == nullptr) return false;
        const std::uint32_t l = static_cast<std::uint32_t>(len);
        if (std::fwrite(&l, sizeof(l), 1, f_) != 1) return false;
        if (std::fwrite(&capture_ts_ns, sizeof(capture_ts_ns), 1, f_) != 1) return false;
        if (std::fwrite(data, 1, len, f_) != len) return false;
        ++hdr_.packet_count;
        hdr_.message_count += messages;
        return true;
    }

    bool close() {
        if (f_ == nullptr) return true;
        // Patch the counts now that they are known.
        bool ok = std::fseek(f_, 0, SEEK_SET) == 0;
        if (ok) {
            std::uint8_t head[kCaptureHeaderLen] = {};
            write_header_bytes(head, hdr_);
            ok = std::fwrite(head, 1, kCaptureHeaderLen, f_) == kCaptureHeaderLen;
        }
        ok = (std::fclose(f_) == 0) && ok;
        f_ = nullptr;
        return ok;
    }

    const CaptureHeader& header() const noexcept { return hdr_; }

private:
    static void write_header_bytes(std::uint8_t* head, const CaptureHeader& h) {
        std::memcpy(head, kCaptureMagic, 8);
        const std::uint32_t ver = kCaptureVersion;
        const std::uint32_t hl = static_cast<std::uint32_t>(kCaptureHeaderLen);
        std::memcpy(head + 8, &ver, 4);
        std::memcpy(head + 12, &hl, 4);
        std::memcpy(head + 16, &h.session_epoch_ns, 8);
        std::memcpy(head + 24, &h.packet_count, 8);
        std::memcpy(head + 32, &h.message_count, 8);
        std::memcpy(head + 40, h.session, 10);
        std::memcpy(head + 50, h.stock, 8);
        std::memcpy(head + 58, &h.stock_locate, 2);
    }

    std::FILE* f_ = nullptr;
    CaptureHeader hdr_;
};

// Reads a capture back. Preloaded by default: benchmarks must not have file
// I/O inside the measured loop, and a decoder handed a pointer into a
// preloaded buffer sees exactly the same bytes a socket would have written
// into its receive buffer.
class CaptureReader {
public:
    bool open(const std::string& path, std::string* error = nullptr) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr) {
            if (error) *error = "cannot open " + path;
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size < static_cast<long>(kCaptureHeaderLen)) {
            std::fclose(f);
            if (error) *error = path + ": too short to be a capture";
            return false;
        }
        buf_.resize(static_cast<std::size_t>(size));
        const std::size_t got = std::fread(buf_.data(), 1, buf_.size(), f);
        std::fclose(f);
        if (got != buf_.size()) {
            if (error) *error = path + ": short read";
            return false;
        }
        if (std::memcmp(buf_.data(), kCaptureMagic, 8) != 0) {
            if (error) *error = path + ": bad magic (not an ITCHCAP1 capture)";
            return false;
        }
        std::uint32_t ver = 0, hl = 0;
        std::memcpy(&ver, buf_.data() + 8, 4);
        std::memcpy(&hl, buf_.data() + 12, 4);
        if (ver != kCaptureVersion || hl != kCaptureHeaderLen) {
            if (error) *error = path + ": unsupported capture version";
            return false;
        }
        std::memcpy(&hdr_.session_epoch_ns, buf_.data() + 16, 8);
        std::memcpy(&hdr_.packet_count, buf_.data() + 24, 8);
        std::memcpy(&hdr_.message_count, buf_.data() + 32, 8);
        std::memcpy(hdr_.session, buf_.data() + 40, 10);
        std::memcpy(hdr_.stock, buf_.data() + 50, 8);
        std::memcpy(&hdr_.stock_locate, buf_.data() + 58, 2);
        pos_ = kCaptureHeaderLen;
        return true;
    }

    const CaptureHeader& header() const noexcept { return hdr_; }
    std::size_t bytes() const noexcept { return buf_.size(); }

    // Hands out a pointer INTO the preloaded buffer - no copy, exactly like a
    // zero-copy receive path handing out a pointer into the NIC ring.
    bool next(const std::uint8_t*& data, std::size_t& len, std::int64_t& capture_ts_ns) noexcept {
        if (pos_ + kCaptureRecordPrefix > buf_.size()) return false;
        std::uint32_t l = 0;
        std::memcpy(&l, buf_.data() + pos_, 4);
        std::memcpy(&capture_ts_ns, buf_.data() + pos_ + 4, 8);
        const std::size_t start = pos_ + kCaptureRecordPrefix;
        if (start + l > buf_.size()) return false;
        data = buf_.data() + start;
        len = l;
        pos_ = start + l;
        return true;
    }

    void rewind() noexcept { pos_ = kCaptureHeaderLen; }

private:
    std::vector<std::uint8_t> buf_;
    std::size_t pos_ = 0;
    CaptureHeader hdr_;
};

}  // namespace feed
}  // namespace itch
