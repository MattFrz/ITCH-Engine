#include "itch_engine/feed/udp_multicast_backend.hpp"

#include <chrono>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define ITCH_CLOSESOCKET closesocket
#define ITCH_SOCKET_ERRNO WSAGetLastError()
#define ITCH_WOULD_BLOCK(e) ((e) == WSAEWOULDBLOCK)
#define ITCH_SOCKOPT_CAST(p) (reinterpret_cast<const char*>(p))
#define ITCH_SOCKOPT_OUT_CAST(p) (reinterpret_cast<char*>(p))
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#endif
#define ITCH_CLOSESOCKET ::close
#define ITCH_SOCKET_ERRNO errno
#define ITCH_WOULD_BLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#define ITCH_SOCKOPT_CAST(p) (p)
#define ITCH_SOCKOPT_OUT_CAST(p) (p)
#endif

namespace itch {
namespace feed {

namespace {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::string socket_error_text(int err) { return "errno " + std::to_string(err); }

#if defined(_WIN32)
// Winsock needs one-time process initialisation. A function-local static gives
// it exactly-once semantics without an explicit init call in every tool.
struct WinsockInit {
    WinsockInit() {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockInit() {
        if (ok) WSACleanup();
    }
    bool ok = false;
};
bool ensure_winsock() {
    static WinsockInit init;
    return init.ok;
}
#endif

}  // namespace

LinuxUdpBackend::~LinuxUdpBackend() { close(); }

const char* LinuxUdpBackend::name() const {
#if defined(__linux__)
    return "linux-udp(recvmmsg)";
#elif defined(_WIN32)
    return "winsock-udp(recvfrom)";
#else
    return "posix-udp(recvfrom)";
#endif
}

bool LinuxUdpBackend::supports_hardware_timestamps() {
#if defined(__linux__) && defined(SO_TIMESTAMPING)
    return true;
#else
    return false;
#endif
}

bool LinuxUdpBackend::supports_batched_receive() {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

bool LinuxUdpBackend::open(const NetworkConfig& cfg, std::string* error) {
    close();
    cfg_ = cfg;
    stats_ = NetworkStats{};

    if (cfg_.port == 0) {
        if (error) *error = "port must be set";
        return false;
    }
    if (cfg_.multicast_group.empty()) {
        if (error) *error = "multicast_group must be set";
        return false;
    }

#if defined(_WIN32)
    if (!ensure_winsock()) {
        if (error) *error = "WSAStartup failed";
        return false;
    }
#endif

    const auto s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#if defined(_WIN32)
    if (s == INVALID_SOCKET) {
        if (error) *error = "socket(): " + socket_error_text(ITCH_SOCKET_ERRNO);
        return false;
    }
    fd_ = static_cast<int>(s);
#else
    if (s < 0) {
        if (error) *error = "socket(): " + socket_error_text(ITCH_SOCKET_ERRNO);
        return false;
    }
    fd_ = s;
#endif

    if (!set_socket_options(error)) {
        close();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.port);
    // Binding to the group address (rather than INADDR_ANY) makes the kernel
    // filter by destination address as well as port, so traffic for other
    // groups on the same port never reaches this socket. Not portable to
    // Windows, which requires INADDR_ANY for a multicast bind.
#if defined(_WIN32)
    addr.sin_addr.s_addr = INADDR_ANY;
#else
    if (::inet_pton(AF_INET, cfg_.multicast_group.c_str(), &addr.sin_addr) != 1) {
        if (error) *error = "bad multicast_group: " + cfg_.multicast_group;
        close();
        return false;
    }
#endif
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error) *error = "bind(): " + socket_error_text(ITCH_SOCKET_ERRNO);
        close();
        return false;
    }

    if (!join_group(error)) {
        close();
        return false;
    }

    slots_ = cfg_.batch_size > 0 ? cfg_.batch_size : 1;
#if !defined(__linux__)
    // Without recvmmsg there is no batching benefit from more than one slot,
    // but keeping the arena the same shape keeps receive() identical.
#endif
    slot_size_ = kMaxDatagramBytes;
    arena_.assign(static_cast<std::size_t>(slots_) * slot_size_, 0);
    hw_ts_.assign(static_cast<std::size_t>(slots_), 0);
    sw_ts_.assign(static_cast<std::size_t>(slots_), 0);
    lengths_.assign(static_cast<std::size_t>(slots_), 0);
    return true;
}

bool LinuxUdpBackend::set_socket_options(std::string* error) {
    const int one = 1;
    if (cfg_.reuse_address) {
        if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, ITCH_SOCKOPT_CAST(&one), sizeof(one)) != 0) {
            if (error) *error = "SO_REUSEADDR: " + socket_error_text(ITCH_SOCKET_ERRNO);
            return false;
        }
#if defined(SO_REUSEPORT) && !defined(_WIN32)
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    }

    // A large receive buffer is the single most effective defence against
    // bursty market data. The kernel routinely grants less than requested
    // (and on Linux doubles what it does grant for bookkeeping), so the
    // effective value is read back and reported rather than assumed.
    const int want = cfg_.recv_buffer_bytes;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, ITCH_SOCKOPT_CAST(&want), sizeof(want));
    int got = 0;
    socklen_t got_len = sizeof(got);
    if (::getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, ITCH_SOCKOPT_OUT_CAST(&got), &got_len) == 0) {
        stats_.effective_recv_buffer = got;
    }

    if (cfg_.nonblocking) {
#if defined(_WIN32)
        u_long mode = 1;
        if (::ioctlsocket(fd_, FIONBIO, &mode) != 0) {
            if (error) *error = "ioctlsocket(FIONBIO): " + socket_error_text(ITCH_SOCKET_ERRNO);
            return false;
        }
#else
        const int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) != 0) {
            if (error) *error = "fcntl(O_NONBLOCK): " + socket_error_text(ITCH_SOCKET_ERRNO);
            return false;
        }
#endif
    }

#if defined(__linux__)
    if (cfg_.busy_poll_us > 0) {
#if defined(SO_BUSY_POLL)
        const int us = cfg_.busy_poll_us;
        // Needs CAP_NET_ADMIN on older kernels; failure is informational, not
        // fatal, because the receiver works fine without it.
        ::setsockopt(fd_, SOL_SOCKET, SO_BUSY_POLL, &us, sizeof(us));
#endif
    }
    if (cfg_.track_kernel_drops) {
#if defined(SO_RXQ_OVFL)
        ::setsockopt(fd_, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof(one));
#endif
    }
    if (cfg_.hardware_timestamps) {
#if defined(SO_TIMESTAMPING)
        const int flags = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE |
                          SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
        ::setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags));
#endif
    }
#endif
    return true;
}

bool LinuxUdpBackend::join_group(std::string* error) {
    ip_mreq mreq{};
    if (::inet_pton(AF_INET, cfg_.multicast_group.c_str(), &mreq.imr_multiaddr) != 1) {
        if (error) *error = "bad multicast_group: " + cfg_.multicast_group;
        return false;
    }
    if (cfg_.interface_address.empty()) {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, cfg_.interface_address.c_str(), &mreq.imr_interface) != 1) {
        if (error) *error = "bad interface_address: " + cfg_.interface_address;
        return false;
    }
    if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, ITCH_SOCKOPT_CAST(&mreq), sizeof(mreq)) !=
        0) {
        if (error) {
            *error = "IP_ADD_MEMBERSHIP " + cfg_.multicast_group + ": " +
                     socket_error_text(ITCH_SOCKET_ERRNO);
        }
        return false;
    }
    joined_ = true;
    return true;
}

void LinuxUdpBackend::close() {
    if (fd_ < 0) return;
    if (joined_) {
        ip_mreq mreq{};
        if (::inet_pton(AF_INET, cfg_.multicast_group.c_str(), &mreq.imr_multiaddr) == 1) {
            if (cfg_.interface_address.empty() ||
                ::inet_pton(AF_INET, cfg_.interface_address.c_str(), &mreq.imr_interface) != 1) {
                mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            }
            ::setsockopt(fd_, IPPROTO_IP, IP_DROP_MEMBERSHIP, ITCH_SOCKOPT_CAST(&mreq),
                         sizeof(mreq));
        }
        joined_ = false;
    }
    ITCH_CLOSESOCKET(fd_);
    fd_ = -1;
}

int LinuxUdpBackend::receive(PacketView* out, int max_packets) {
    if (fd_ < 0 || max_packets <= 0) return 0;
    const int want = max_packets < slots_ ? max_packets : slots_;
    ++stats_.receive_calls;

#if defined(__linux__)
    // One syscall per batch. At feed rates the syscall, not the copy, is what
    // dominates a naive per-datagram recvfrom loop.
    struct mmsghdr msgs[256];
    struct iovec iovecs[256];
    // Control buffer per message, for SO_RXQ_OVFL and SO_TIMESTAMPING.
    alignas(8) char control[256][256];
    const int n_slots = want > 256 ? 256 : want;

    std::memset(msgs, 0, sizeof(struct mmsghdr) * static_cast<std::size_t>(n_slots));
    for (int i = 0; i < n_slots; ++i) {
        iovecs[i].iov_base = arena_.data() + static_cast<std::size_t>(i) * slot_size_;
        iovecs[i].iov_len = slot_size_;
        msgs[i].msg_hdr.msg_iov = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_control = control[i];
        msgs[i].msg_hdr.msg_controllen = sizeof(control[i]);
    }

    const int got = ::recvmmsg(fd_, msgs, static_cast<unsigned>(n_slots), 0, nullptr);
    if (got < 0) {
        const int e = ITCH_SOCKET_ERRNO;
        if (ITCH_WOULD_BLOCK(e)) {
            ++stats_.would_block;
            return 0;
        }
        ++stats_.errors;
        return -1;
    }

    const std::uint64_t sw = now_ns();
    for (int i = 0; i < got; ++i) {
        out[i].data = arena_.data() + static_cast<std::size_t>(i) * slot_size_;
        out[i].len = msgs[i].msg_len;
        out[i].sw_timestamp_ns = sw;
        out[i].hw_timestamp_ns = 0;
        if (msgs[i].msg_hdr.msg_flags & MSG_TRUNC) ++stats_.truncated;

        for (struct cmsghdr* cm = CMSG_FIRSTHDR(&msgs[i].msg_hdr); cm != nullptr;
             cm = CMSG_NXTHDR(&msgs[i].msg_hdr, cm)) {
#if defined(SO_RXQ_OVFL)
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SO_RXQ_OVFL) {
                std::uint32_t drops = 0;
                std::memcpy(&drops, CMSG_DATA(cm), sizeof(drops));
                // The kernel reports a cumulative counter; the delta is what
                // this batch actually lost.
                if (drops > last_drop_count_) {
                    stats_.kernel_drops += drops - last_drop_count_;
                    last_drop_count_ = drops;
                }
            }
#endif
#if defined(SO_TIMESTAMPING)
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SO_TIMESTAMPING) {
                struct timespec ts[3];
                std::memcpy(ts, CMSG_DATA(cm), sizeof(ts));
                // ts[2] is the raw hardware timestamp when the NIC supplies
                // one; ts[0] is the software fallback.
                const struct timespec& hw = ts[2].tv_sec != 0 || ts[2].tv_nsec != 0 ? ts[2] : ts[0];
                out[i].hw_timestamp_ns = static_cast<std::uint64_t>(hw.tv_sec) * 1000000000ull +
                                         static_cast<std::uint64_t>(hw.tv_nsec);
            }
#endif
        }
        ++stats_.packets;
        stats_.bytes += out[i].len;
    }
    return got;
#else
    // Portable fallback: one recvfrom per datagram. Correct everywhere,
    // measurably worse than recvmmsg, and the reason the Linux path exists.
    int got = 0;
    for (int i = 0; i < want; ++i) {
        std::uint8_t* slot = arena_.data() + static_cast<std::size_t>(i) * slot_size_;
        const auto n = ::recvfrom(fd_, reinterpret_cast<char*>(slot),
                                  static_cast<int>(slot_size_), 0, nullptr, nullptr);
        if (n < 0) {
            const int e = ITCH_SOCKET_ERRNO;
            if (ITCH_WOULD_BLOCK(e)) {
                ++stats_.would_block;
                break;
            }
            ++stats_.errors;
            return got > 0 ? got : -1;
        }
        out[got].data = slot;
        out[got].len = static_cast<std::uint32_t>(n);
        out[got].sw_timestamp_ns = now_ns();
        out[got].hw_timestamp_ns = 0;
        ++stats_.packets;
        stats_.bytes += out[got].len;
        ++got;
    }
    return got;
#endif
}

}  // namespace feed
}  // namespace itch
