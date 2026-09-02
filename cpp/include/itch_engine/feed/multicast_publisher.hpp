#pragma once

// IPv4 UDP multicast sender.
//
// The counterpart to LinuxUdpBackend, and the thing that turns the historical
// packet simulator into a real end-to-end test: `itch_replay --publish-group`
// puts a recorded session on the wire as actual multicast datagrams, and
// `itch_live` receives them through the same socket path it would use for an
// exchange feed.
//
//   capture -> itch_replay --publish -> UDP multicast (loopback or a lab LAN)
//                                            |
//                                            v
//                                       itch_live -> MoldUDP64 -> ITCH -> book
//
// That is the only way to exercise the receive path - socket options, joins,
// batching, kernel drops - without a live feed, and it is how the loopback
// smoke test in docs/low_latency_architecture.md is run.
//
// Send only to a group you control. The default TTL is 1, so datagrams do not
// leave the local segment unless that is changed deliberately.

#include <cstdint>
#include <string>

namespace itch {
namespace feed {

struct PublisherConfig {
    std::string multicast_group;
    std::uint16_t port = 0;
    std::string interface_address;  // local NIC to send from; empty = default
    int ttl = 1;                    // 1 = do not leave this segment
    bool loopback = true;           // deliver to receivers on this host
    int send_buffer_bytes = 8 * 1024 * 1024;
};

struct PublisherStats {
    std::uint64_t packets = 0;
    std::uint64_t bytes = 0;
    std::uint64_t errors = 0;
    std::uint64_t would_block = 0;
};

class MulticastPublisher {
public:
    MulticastPublisher() = default;
    ~MulticastPublisher();

    MulticastPublisher(const MulticastPublisher&) = delete;
    MulticastPublisher& operator=(const MulticastPublisher&) = delete;

    bool open(const PublisherConfig& cfg, std::string* error);
    void close();
    bool is_open() const { return fd_ >= 0; }

    // Sends one datagram. Returns false on a hard error; a full send buffer is
    // retried briefly and then counted, because dropping market data silently
    // is the failure this project exists to avoid.
    bool send(const std::uint8_t* data, std::size_t len);

    const PublisherStats& stats() const { return stats_; }

private:
    int fd_ = -1;
    PublisherConfig cfg_;
    PublisherStats stats_;
};

}  // namespace feed
}  // namespace itch
