#pragma once

// Network backend abstraction.
//
//   NetworkBackend
//     |-- LinuxUdpBackend   kernel UDP sockets (the first and default one)
//     |-- DpdkBackend       poll-mode driver, built only with -DITCH_ENABLE_DPDK=ON
//
// The interface hands out *views* into the backend's own receive buffers:
//
//   NIC -> DMA -> backend buffer -> PacketView -> MoldUDP64 -> ITCH parser
//
// Nothing between the socket and the parser copies the payload. The parser
// decodes fields straight out of the buffer the kernel (or the PMD) filled,
// and MarketEvent is the first thing that is written rather than copied.
//
// Virtual dispatch here is fine: it is once per *batch* of packets, not once
// per message. A 1400-byte datagram carries tens of ITCH messages, so the
// indirect call is amortised roughly 40:1 and does not appear in the profile.

#include <cstdint>
#include <string>
#include <vector>

namespace itch {
namespace feed {

struct PacketView {
    const std::uint8_t* data = nullptr;
    std::uint32_t len = 0;
    // NIC hardware timestamp when the driver supplies one, else 0. Never
    // confuse it with the exchange timestamp inside the ITCH message.
    std::uint64_t hw_timestamp_ns = 0;
    // Kernel/application receive timestamp, monotonic, always populated.
    std::uint64_t sw_timestamp_ns = 0;
};

struct NetworkStats {
    std::uint64_t packets = 0;
    std::uint64_t bytes = 0;
    std::uint64_t receive_calls = 0;
    std::uint64_t would_block = 0;
    std::uint64_t errors = 0;
    // Datagrams the kernel dropped because the socket buffer was full. This is
    // the number that says "you are too slow", and it is distinct from a
    // MoldUDP64 sequence gap, which says "something upstream lost them".
    std::uint64_t kernel_drops = 0;
    std::uint64_t truncated = 0;
    // What the kernel actually granted for SO_RCVBUF, which is usually not
    // what was asked for.
    int effective_recv_buffer = 0;
};

struct NetworkConfig {
    // Every one of these is configuration, never a literal in the code. There
    // are no real multicast groups, ports or credentials anywhere in this
    // repository, and joining a feed you are not entitled to is not a
    // technical question.
    std::string multicast_group;      // e.g. "239.0.0.1" for a local test
    std::string bind_address = "0.0.0.0";
    std::string interface_address;    // local NIC address that joins the group
    std::uint16_t port = 0;

    int recv_buffer_bytes = 32 * 1024 * 1024;
    int batch_size = 32;              // datagrams per receive syscall (Linux)
    bool nonblocking = true;
    bool reuse_address = true;

    // Linux only, ignored elsewhere.
    int busy_poll_us = 0;             // SO_BUSY_POLL
    bool hardware_timestamps = false; // SO_TIMESTAMPING
    bool track_kernel_drops = true;   // SO_RXQ_OVFL
};

class NetworkBackend {
public:
    virtual ~NetworkBackend() = default;

    virtual bool open(const NetworkConfig& cfg, std::string* error) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    // Receives up to `max_packets` datagrams into internal buffers and fills
    // `out` with views over them. Returns the number received (0 when a
    // nonblocking socket has nothing), or -1 on error. Views stay valid until
    // the next receive() on the same backend.
    virtual int receive(PacketView* out, int max_packets) = 0;

    virtual const NetworkStats& stats() const = 0;
    virtual const char* name() const = 0;
};

}  // namespace feed
}  // namespace itch
