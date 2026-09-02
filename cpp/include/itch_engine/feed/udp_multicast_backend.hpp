#pragma once

// IPv4 UDP multicast receiver.
//
// Primary target is Linux, where it uses recvmmsg (one syscall for a batch of
// datagrams), SO_BUSY_POLL, SO_RXQ_OVFL for kernel drop accounting and
// SO_TIMESTAMPING for NIC hardware receive timestamps. The same class also
// builds on Windows against Winsock with those knobs compiled out, so the
// whole pipeline - including the tests - runs on a development machine that
// is not the deployment target. Where a feature is Linux-only it is reported
// as unavailable rather than silently ignored.
//
// Nothing here hardcodes a group, a port, an interface or any credential:
// every one comes from NetworkConfig, which the tools populate from the
// command line. Point it at a feed you are entitled to receive.

#include <cstdint>
#include <string>
#include <vector>

#include "itch_engine/feed/network_backend.hpp"

namespace itch {
namespace feed {

class LinuxUdpBackend final : public NetworkBackend {
public:
    LinuxUdpBackend() = default;
    ~LinuxUdpBackend() override;

    LinuxUdpBackend(const LinuxUdpBackend&) = delete;
    LinuxUdpBackend& operator=(const LinuxUdpBackend&) = delete;

    bool open(const NetworkConfig& cfg, std::string* error) override;
    void close() override;
    bool is_open() const override { return fd_ >= 0; }

    int receive(PacketView* out, int max_packets) override;

    const NetworkStats& stats() const override { return stats_; }
    const char* name() const override;

    // True when this build can supply NIC hardware receive timestamps.
    static bool supports_hardware_timestamps();
    // True when this build batches receives into one syscall (recvmmsg).
    static bool supports_batched_receive();

    const NetworkConfig& config() const { return cfg_; }

private:
    bool set_socket_options(std::string* error);
    bool join_group(std::string* error);

    int fd_ = -1;
    NetworkConfig cfg_;
    NetworkStats stats_;

    // One contiguous arena, carved into fixed slots, allocated on open() and
    // never touched again. The receive path writes into it directly and hands
    // out pointers into it: no per-packet allocation, no copy.
    std::vector<std::uint8_t> arena_;
    std::vector<std::uint64_t> hw_ts_;
    std::vector<std::uint64_t> sw_ts_;
    std::vector<std::uint32_t> lengths_;
    std::size_t slot_size_ = 0;
    int slots_ = 0;
    std::uint64_t last_drop_count_ = 0;
    bool joined_ = false;
};

// Maximum datagram this receiver will accept. MoldUDP64 on a real feed sits
// well inside one Ethernet MTU; the extra room is for jumbo-frame captures and
// for detecting (rather than silently truncating) anything larger.
inline constexpr std::size_t kMaxDatagramBytes = 9216;

}  // namespace feed
}  // namespace itch
