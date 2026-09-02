#include "itch_engine/feed/multicast_publisher.hpp"

#include <cstring>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define ITCH_CLOSESOCKET closesocket
#define ITCH_SOCKET_ERRNO WSAGetLastError()
#define ITCH_WOULD_BLOCK(e) ((e) == WSAEWOULDBLOCK)
#define ITCH_SOCKOPT_CAST(p) (reinterpret_cast<const char*>(p))
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define ITCH_CLOSESOCKET ::close
#define ITCH_SOCKET_ERRNO errno
#define ITCH_WOULD_BLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#define ITCH_SOCKOPT_CAST(p) (p)
#endif

namespace itch {
namespace feed {

namespace {
#if defined(_WIN32)
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

MulticastPublisher::~MulticastPublisher() { close(); }

bool MulticastPublisher::open(const PublisherConfig& cfg, std::string* error) {
    close();
    cfg_ = cfg;
    stats_ = PublisherStats{};

    if (cfg_.multicast_group.empty() || cfg_.port == 0) {
        if (error) *error = "multicast_group and port are required";
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
        if (error) *error = "socket() failed";
        return false;
    }
    fd_ = static_cast<int>(s);
#else
    if (s < 0) {
        if (error) *error = "socket() failed";
        return false;
    }
    fd_ = s;
#endif

    const int ttl = cfg_.ttl;
    if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, ITCH_SOCKOPT_CAST(&ttl), sizeof(ttl)) !=
        0) {
        if (error) *error = "IP_MULTICAST_TTL failed";
        close();
        return false;
    }
    const int loop = cfg_.loopback ? 1 : 0;
    ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, ITCH_SOCKOPT_CAST(&loop), sizeof(loop));

    if (!cfg_.interface_address.empty()) {
        in_addr iface{};
        if (::inet_pton(AF_INET, cfg_.interface_address.c_str(), &iface) != 1) {
            if (error) *error = "bad interface_address " + cfg_.interface_address;
            close();
            return false;
        }
        if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, ITCH_SOCKOPT_CAST(&iface),
                         sizeof(iface)) != 0) {
            if (error) *error = "IP_MULTICAST_IF failed";
            close();
            return false;
        }
    }

    const int sndbuf = cfg_.send_buffer_bytes;
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, ITCH_SOCKOPT_CAST(&sndbuf), sizeof(sndbuf));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(cfg_.port);
    if (::inet_pton(AF_INET, cfg_.multicast_group.c_str(), &dest.sin_addr) != 1) {
        if (error) *error = "bad multicast_group " + cfg_.multicast_group;
        close();
        return false;
    }
    // Connecting a datagram socket fixes the destination, so every send is one
    // syscall with no address to copy.
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) != 0) {
        if (error) *error = "connect() to the group failed";
        close();
        return false;
    }
    return true;
}

void MulticastPublisher::close() {
    if (fd_ < 0) return;
    ITCH_CLOSESOCKET(fd_);
    fd_ = -1;
}

bool MulticastPublisher::send(const std::uint8_t* data, std::size_t len) {
    if (fd_ < 0) return false;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const auto n = ::send(fd_, reinterpret_cast<const char*>(data), static_cast<int>(len), 0);
        if (n >= 0) {
            ++stats_.packets;
            stats_.bytes += len;
            return true;
        }
        const int e = ITCH_SOCKET_ERRNO;
        if (!ITCH_WOULD_BLOCK(e)) {
            ++stats_.errors;
            return false;
        }
        ++stats_.would_block;
        std::this_thread::yield();
    }
    ++stats_.errors;
    return false;
}

}  // namespace feed
}  // namespace itch
