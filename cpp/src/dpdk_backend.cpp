#include "itch_engine/feed/dpdk_backend.hpp"

#include <cstring>

// Everything DPDK-specific is behind the build flag. Without it this file
// still compiles and links, and DpdkBackend::open() explains why it cannot
// run instead of pretending to be a socket. That is what keeps `cmake -S . -B
// build` working on a machine with no DPDK, which is a hard requirement here.

#if defined(ITCH_ENABLE_DPDK) && ITCH_ENABLE_DPDK
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_udp.h>
#include <vector>
#endif

namespace itch {
namespace feed {

bool DpdkBackend::available() {
#if defined(ITCH_ENABLE_DPDK) && ITCH_ENABLE_DPDK
    return true;
#else
    return false;
#endif
}

const char* DpdkBackend::name() const {
#if defined(ITCH_ENABLE_DPDK) && ITCH_ENABLE_DPDK
    return "dpdk(poll-mode)";
#else
    return "dpdk(not built)";
#endif
}

#if !defined(ITCH_ENABLE_DPDK) || !ITCH_ENABLE_DPDK

// --- stub build -------------------------------------------------------------

struct DpdkBackend::Impl {};

DpdkBackend::DpdkBackend() = default;
DpdkBackend::~DpdkBackend() = default;

bool DpdkBackend::open(const NetworkConfig& cfg, std::string* error) {
    (void)cfg;
    if (error) {
        *error =
            "this build has no DPDK backend. Reconfigure with -DITCH_ENABLE_DPDK=ON on a host "
            "with DPDK >= 21.11, hugepages reserved and a NIC bound to a userspace driver "
            "(see docs/dpdk.md), or use the default kernel UDP backend.";
    }
    return false;
}

void DpdkBackend::close() {}

int DpdkBackend::receive(PacketView*, int) { return -1; }

#else

// --- DPDK build -------------------------------------------------------------
//
// Receive path:
//
//   NIC -> DMA into a hugepage-backed mbuf -> rte_eth_rx_burst()
//       -> skip Ethernet/IPv4/UDP headers in place
//       -> PacketView pointing INTO the mbuf
//       -> MoldUDP64 decoder -> ITCH parser -> book
//
// The payload is never copied. mbufs from the previous burst are freed at the
// start of the next one, which is why PacketViews are documented as valid only
// until the following receive().
//
// NOT MEASURED: no DPDK hardware was available while writing this. Treat it as
// a starting point for a deployment and benchmark it yourself before believing
// anything about it.

struct DpdkBackend::Impl {
    rte_mempool* pool = nullptr;
    rte_mbuf* burst[256] = {nullptr};
    std::uint16_t in_flight = 0;
    std::uint32_t group_be = 0;
    std::uint16_t udp_port_be = 0;
    bool eal_initialised = false;
};

DpdkBackend::DpdkBackend() : impl_(new Impl()) {}

DpdkBackend::~DpdkBackend() {
    close();
    delete impl_;
    impl_ = nullptr;
}

bool DpdkBackend::open(const NetworkConfig& cfg, std::string* error) {
    close();
    cfg_ = cfg;
    stats_ = NetworkStats{};

    // EAL arguments come from configuration. Nothing here invents a core mask,
    // a PCI address or a memory channel count.
    std::vector<char*> argv;
    std::vector<std::string> args = dpdk_.eal_args;
    if (args.empty()) args.push_back("itch_live");
    for (std::string& a : args) argv.push_back(&a[0]);

    const int consumed = rte_eal_init(static_cast<int>(argv.size()), argv.data());
    if (consumed < 0) {
        if (error) *error = "rte_eal_init failed: " + std::string(rte_strerror(rte_errno));
        return false;
    }
    impl_->eal_initialised = true;

    if (!rte_eth_dev_is_valid_port(dpdk_.port_id)) {
        if (error) *error = "no such DPDK port " + std::to_string(dpdk_.port_id);
        return false;
    }

    impl_->pool = rte_pktmbuf_pool_create("itch_rx", dpdk_.mbuf_pool_size, 256, 0,
                                          RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (impl_->pool == nullptr) {
        if (error) *error = "rte_pktmbuf_pool_create failed: " + std::string(rte_strerror(rte_errno));
        return false;
    }

    rte_eth_conf port_conf{};
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    if (rte_eth_dev_configure(dpdk_.port_id, 1, 0, &port_conf) != 0) {
        if (error) *error = "rte_eth_dev_configure failed";
        return false;
    }
    if (rte_eth_rx_queue_setup(dpdk_.port_id, dpdk_.queue_id, dpdk_.rx_descriptors,
                               rte_eth_dev_socket_id(dpdk_.port_id), nullptr, impl_->pool) < 0) {
        if (error) *error = "rte_eth_rx_queue_setup failed";
        return false;
    }
    if (rte_eth_dev_start(dpdk_.port_id) < 0) {
        if (error) *error = "rte_eth_dev_start failed";
        return false;
    }
    // Multicast destination MACs are not this port's address, so the NIC has
    // to be told to keep them.
    rte_eth_promiscuous_enable(dpdk_.port_id);

    const std::string group =
        dpdk_.multicast_group.empty() ? cfg_.multicast_group : dpdk_.multicast_group;
    const std::uint16_t port = dpdk_.udp_port != 0 ? dpdk_.udp_port : cfg_.port;
    if (!group.empty()) {
        std::uint32_t addr = 0;
        if (inet_pton(AF_INET, group.c_str(), &addr) != 1) {
            if (error) *error = "bad multicast group " + group;
            return false;
        }
        impl_->group_be = addr;
    }
    impl_->udp_port_be = rte_cpu_to_be_16(port);

    open_ = true;
    return true;
}

void DpdkBackend::close() {
    if (!open_) return;
    for (std::uint16_t i = 0; i < impl_->in_flight; ++i) {
        rte_pktmbuf_free(impl_->burst[i]);
        impl_->burst[i] = nullptr;
    }
    impl_->in_flight = 0;
    rte_eth_dev_stop(dpdk_.port_id);
    rte_eth_dev_close(dpdk_.port_id);
    open_ = false;
}

int DpdkBackend::receive(PacketView* out, int max_packets) {
    if (!open_ || max_packets <= 0) return 0;

    // Free the previous burst. This is why a PacketView is only valid until the
    // next receive() - it points into these mbufs.
    for (std::uint16_t i = 0; i < impl_->in_flight; ++i) {
        rte_pktmbuf_free(impl_->burst[i]);
        impl_->burst[i] = nullptr;
    }
    impl_->in_flight = 0;

    const std::uint16_t want = static_cast<std::uint16_t>(
        max_packets < dpdk_.burst_size ? max_packets : dpdk_.burst_size);
    const std::uint16_t got =
        rte_eth_rx_burst(dpdk_.port_id, dpdk_.queue_id, impl_->burst,
                         want > 256 ? 256 : want);
    impl_->in_flight = got;
    ++stats_.receive_calls;
    if (got == 0) {
        ++stats_.would_block;
        return 0;
    }

    int produced = 0;
    for (std::uint16_t i = 0; i < got; ++i) {
        rte_mbuf* m = impl_->burst[i];
        const std::uint32_t len = rte_pktmbuf_pkt_len(m);
        const std::size_t headers =
            sizeof(rte_ether_hdr) + sizeof(rte_ipv4_hdr) + sizeof(rte_udp_hdr);
        if (len <= headers) continue;

        const rte_ether_hdr* eth = rte_pktmbuf_mtod(m, const rte_ether_hdr*);
        if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) continue;
        const rte_ipv4_hdr* ip =
            reinterpret_cast<const rte_ipv4_hdr*>(reinterpret_cast<const std::uint8_t*>(eth) +
                                                  sizeof(rte_ether_hdr));
        if (ip->next_proto_id != IPPROTO_UDP) continue;
        if (impl_->group_be != 0 && ip->dst_addr != impl_->group_be) continue;

        // IHL is in 32-bit words; options are rare on a feed but not illegal.
        const std::size_t ip_len = static_cast<std::size_t>(ip->version_ihl & 0x0F) * 4u;
        const rte_udp_hdr* udp =
            reinterpret_cast<const rte_udp_hdr*>(reinterpret_cast<const std::uint8_t*>(ip) + ip_len);
        if (impl_->udp_port_be != 0 && udp->dst_port != impl_->udp_port_be) continue;

        const std::uint8_t* payload =
            reinterpret_cast<const std::uint8_t*>(udp) + sizeof(rte_udp_hdr);
        const std::uint16_t udp_len = rte_be_to_cpu_16(udp->dgram_len);
        if (udp_len < sizeof(rte_udp_hdr)) continue;
        const std::uint32_t payload_len = udp_len - sizeof(rte_udp_hdr);

        out[produced].data = payload;
        out[produced].len = payload_len;
        out[produced].sw_timestamp_ns = 0;
        out[produced].hw_timestamp_ns = 0;
#ifdef RTE_MBUF_F_RX_TIMESTAMP
        if (dpdk_.hardware_timestamps && (m->ol_flags & RTE_MBUF_F_RX_TIMESTAMP)) {
            out[produced].hw_timestamp_ns = m->timestamp;
        }
#endif
        ++stats_.packets;
        stats_.bytes += payload_len;
        ++produced;
    }

    rte_eth_stats port_stats{};
    if (rte_eth_stats_get(dpdk_.port_id, &port_stats) == 0) {
        stats_.kernel_drops = port_stats.imissed + port_stats.rx_nombuf;
    }
    return produced;
}

#endif  // ITCH_ENABLE_DPDK

}  // namespace feed
}  // namespace itch
