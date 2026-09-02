#pragma once

// Optional DPDK network backend.
//
//   NetworkBackend
//     |-- LinuxUdpBackend   kernel sockets - the default, always built
//     |-- DpdkBackend       poll-mode driver - built only with
//                           -DITCH_ENABLE_DPDK=ON
//
// DPDK is deliberately NOT a dependency of this project. cmake configures and
// the whole engine builds, tests and benchmarks without it; enabling the flag
// is an explicit choice on a machine that has DPDK installed, a supported NIC,
// hugepages reserved and a port bound to a userspace driver. See
// docs/dpdk.md for the exact setup, and be aware that binding a NIC to a
// userspace driver takes it away from the kernel.
//
// What it changes: the kernel network stack disappears from the receive path.
// The NIC DMAs frames into hugepage-backed mbufs and this class hands the
// decoder a pointer into one, so the Ethernet/IP/UDP headers are skipped in
// place and the ITCH parser reads the same DMA'd bytes. No syscall, no copy,
// no interrupt.
//
// What it does not change: everything above it. MoldUDP64, the ITCH parser,
// MarketEvent and the book are identical either way, which is the entire point
// of putting the backend behind an interface.
//
// Honest status: the implementation below is compile-gated and has not been
// run on this machine, which has no DPDK and no supported NIC. It is written
// against the DPDK 21.11+ API and is the starting point for a deployment, not
// a measured result. There are no DPDK latency numbers anywhere in this
// repository for exactly that reason.

#include <cstdint>
#include <string>
#include <vector>

#include "itch_engine/feed/network_backend.hpp"

namespace itch {
namespace feed {

struct DpdkConfig {
    // EAL arguments, passed through verbatim (for example "-l", "2",
    // "--proc-type=primary"). Nothing is invented here.
    std::vector<std::string> eal_args;
    std::uint16_t port_id = 0;
    std::uint16_t queue_id = 0;
    std::uint16_t rx_descriptors = 1024;
    std::uint16_t mbuf_pool_size = 8192;
    std::uint16_t burst_size = 32;
    // The multicast group and UDP port to accept, applied as a software filter
    // after the headers are parsed (and, where the NIC supports it, as a flow
    // rule so unwanted traffic never reaches this core).
    std::string multicast_group;
    std::uint16_t udp_port = 0;
    bool hardware_timestamps = false;
};

class DpdkBackend final : public NetworkBackend {
public:
    DpdkBackend();
    ~DpdkBackend() override;

    DpdkBackend(const DpdkBackend&) = delete;
    DpdkBackend& operator=(const DpdkBackend&) = delete;

    // Compiled in? If false, open() always fails with an explanatory message
    // rather than pretending.
    static bool available();

    void set_dpdk_config(const DpdkConfig& cfg) { dpdk_ = cfg; }

    bool open(const NetworkConfig& cfg, std::string* error) override;
    void close() override;
    bool is_open() const override { return open_; }
    int receive(PacketView* out, int max_packets) override;
    const NetworkStats& stats() const override { return stats_; }
    const char* name() const override;

private:
    struct Impl;
    Impl* impl_ = nullptr;
    DpdkConfig dpdk_;
    NetworkConfig cfg_;
    NetworkStats stats_;
    bool open_ = false;
};

}  // namespace feed
}  // namespace itch
