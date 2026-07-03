//
// Created by Kotarou on 2026/7/2.
//

#include "mdns.h"

#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <optional>
#include <vector>

#include <spdlog/spdlog.h>

#include "fmt.hpp"
#include "dns/parser.h"
#include "dns/types.h"
#include "dns/mkquery.h"
#include "network/inet_address.h"

// ===========================================================================
//  mDNS well-known constants (RFC 6762)
// ===========================================================================

namespace {
    constexpr uint16_t MDNS_PORT = 5353;

    // IPv4 multicast group
    constexpr auto MDNS_IPV4_GROUP = "224.0.0.251";

    // IPv6 multicast group
    constexpr auto MDNS_IPV6_GROUP = "ff02::fb";

    // How long to wait for a response.
    constexpr auto MDNS_TIMEOUT = std::chrono::milliseconds(500);

    // RAII wrapper that closes the fd on scope exit.
    struct ScopedFd {
        int fd;

        explicit ScopedFd(int f) : fd(f) {
        }

        ~ScopedFd() {
            if (fd >= 0) ::close(fd);
        }

        ScopedFd(const ScopedFd &) = delete;

        ScopedFd &operator=(const ScopedFd &) = delete;
    };
} // anonymous namespace

// ===========================================================================
//  MdnsIpSource — public API
// ===========================================================================

MdnsIpSource::MdnsIpSource(std::string hostname, dns_type type, std::string interface)
    : hostname_(std::move(hostname)), type_(type), interface_(std::move(interface)) {
}

std::vector<InetAddress> MdnsIpSource::resolve() const {
    const bool is_ipv6 = (type_ == dns_type::AAAA);
    const int af = is_ipv6 ? AF_INET6 : AF_INET;
    const char *mcast_group = is_ipv6 ? MDNS_IPV6_GROUP : MDNS_IPV4_GROUP;

    const auto &iface_label = interface_.empty() ? "<default>" : interface_;
    SPDLOG_DEBUG(R"(mDNS: resolving "{}" (type {}) on interface "{}")", hostname_, is_ipv6 ? "AAAA" : "A", iface_label);

    // ── 1. Build the mDNS query packet ──────────────────────────────────────
    const auto ns_type = DNS::to_ns_type(type_);
    const auto query_pkt = DNS::mkquery_mdns(hostname_, ns_type, true);

    // ── 2. Create the socket ────────────────────────────────────────────────
    const int sockfd = ::socket(af, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        SPDLOG_ERROR(R"(mDNS: socket() failed for "{}": {})", hostname_, std::strerror(errno));
        return {};
    }
    ScopedFd guard{sockfd};

    // Allow multiple listeners on the same port (RFC 6762 §15.1).
    int reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        SPDLOG_WARN(R"(mDNS: setsockopt(SO_REUSEADDR) failed for "{}": {})", hostname_, std::strerror(errno));
    }

    // Bind to the interface (Linux) or interface index (macOS/BSD).
    if (!interface_.empty()) {
#if defined(SO_BINDTODEVICE)
        if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, interface_.c_str(), interface_.size()) < 0) {
            SPDLOG_WARN(R"(mDNS: setsockopt(SO_BINDTODEVICE, "{}") failed for "{}": {})",
                        interface_, hostname_, std::strerror(errno));
        }
#elif defined(IP_BOUND_IF)
        unsigned int if_index = ::if_nametoindex(interface_.c_str());
        if (if_index > 0) {
            int ip_level = is_ipv6 ? IPPROTO_IPV6 : IPPROTO_IP;
            int opt_name = is_ipv6 ? IPV6_BOUND_IF : IP_BOUND_IF;
            if (setsockopt(sockfd, ip_level, opt_name, &if_index, sizeof(if_index)) < 0) {
                SPDLOG_WARN(R"(mDNS: setsockopt bound-if failed for "{}": {})", hostname_, std::strerror(errno));
            }
        } else {
            SPDLOG_WARN(R"(mDNS: if_nametoindex("{}") failed for "{}")", interface_, hostname_);
        }
#endif
    }

    // For IPv6, explicitly request v6-only (avoid v4-mapped addresses).
    if (is_ipv6) {
        int v6only = 1;
        setsockopt(sockfd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    }

    // Bind ephemeral port — mDNS responses come back to this port.
    if (af == AF_INET6) {
        sockaddr_in6 bind_addr{};
        bind_addr.sin6_family = AF_INET6;
        bind_addr.sin6_addr = in6addr_any;
        bind_addr.sin6_port = 0;
        if (bind(sockfd, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)) < 0) {
            SPDLOG_ERROR(R"(mDNS: bind() failed for "{}": {})", hostname_, std::strerror(errno));
            return {};
        }
    } else {
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port = 0;
        if (bind(sockfd, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)) < 0) {
            SPDLOG_ERROR(R"(mDNS: bind() failed for "{}": {})", hostname_, std::strerror(errno));
            return {};
        }
    }

    // ── 3. Set multicast socket options (required on FreeBSD/BSD) ────────────
    unsigned int if_index = 0;
    if (!interface_.empty()) {
        if_index = ::if_nametoindex(interface_.c_str());
        if (if_index == 0) {
            SPDLOG_WARN(R"(mDNS: if_nametoindex("{}") failed for "{}")", interface_, hostname_);
        }
    }

    if (is_ipv6) {
        // Set the outgoing interface for IPv6 multicast (FreeBSD needs this to
        // route the sendto() to the correct interface; 0 = system default).
        if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_MULTICAST_IF,
                       &if_index, sizeof(if_index)) < 0) {
            SPDLOG_WARN(R"(mDNS: IPV6_MULTICAST_IF failed for "{}": {})", hostname_, std::strerror(errno));
        }

        // Explicit hop-limit for multicast packets (default is 1 on most
        // systems, but being explicit avoids surprises on FreeBSD).
        int hops = 255;
        if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)) < 0) {
            SPDLOG_WARN(R"(mDNS: IPV6_MULTICAST_HOPS failed for "{}": {})", hostname_, std::strerror(errno));
        }

        // Join the multicast group so the kernel delivers responses.
        ipv6_mreq mreq6{};
        inet_pton(AF_INET6, mcast_group, &mreq6.ipv6mr_multiaddr);
        mreq6.ipv6mr_interface = if_index;
        if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq6, sizeof(mreq6)) < 0) {
            SPDLOG_WARN(R"(mDNS: IPV6_JOIN_GROUP failed for "{}": {})", hostname_, std::strerror(errno));
        }
    } else {
        // Set the outgoing interface for IPv4 multicast (optional on most
        // systems, but good practice when pinned to a specific interface).
        if (if_index > 0) {
            if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_IF,
                           &if_index, sizeof(if_index)) < 0) {
                SPDLOG_WARN(R"(mDNS: IP_MULTICAST_IF failed for "{}": {})", hostname_, std::strerror(errno));
            }
        }

        // Explicit TTL for multicast packets.
        int ttl = 255;
        if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
            SPDLOG_WARN(R"(mDNS: IP_MULTICAST_TTL failed for "{}": {})", hostname_, std::strerror(errno));
        }

        // Join the multicast group so the kernel delivers responses.
        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = inet_addr(MDNS_IPV4_GROUP);
        // Socket is already bound to the desired interface via IP_BOUND_IF or
        // SO_BINDTODEVICE above, so INADDR_ANY lets the kernel pick the right one.
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            SPDLOG_WARN(R"(mDNS: IP_ADD_MEMBERSHIP failed for "{}": {})", hostname_, std::strerror(errno));
        }
    }

    // ── 4. Send the query to the multicast group ────────────────────────────
    union {
        sockaddr_in v4;
        sockaddr_in6 v6;
        sockaddr generic;
    } dest{};

    socklen_t dest_len = 0;

    if (is_ipv6) {
        dest.v6.sin6_family = AF_INET6;
        dest.v6.sin6_port = htons(MDNS_PORT);
        inet_pton(AF_INET6, mcast_group, &dest.v6.sin6_addr);
        dest_len = sizeof(dest.v6);
    } else {
        dest.v4.sin_family = AF_INET;
        dest.v4.sin_port = htons(MDNS_PORT);
        inet_pton(AF_INET, mcast_group, &dest.v4.sin_addr);
        dest_len = sizeof(dest.v4);
    }

    const auto sent = sendto(sockfd, query_pkt.data(), query_pkt.size(), 0, &dest.generic, dest_len);

    if (sent < 0) {
        SPDLOG_ERROR(R"(mDNS: sendto() failed for "{}": {})", hostname_, std::strerror(errno));
        return {};
    }

    SPDLOG_TRACE(R"(mDNS: sent {} bytes for "{}")", sent, hostname_);

    // ── 5. Wait for a response (with timeout) ───────────────────────────────
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sockfd, &read_fds);

    timeval tv{};
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>(
        std::chrono::duration_cast<std::chrono::microseconds>(MDNS_TIMEOUT).count()
    );

    const int ready = select(sockfd + 1, &read_fds, nullptr, nullptr, &tv);
    if (ready <= 0) {
        SPDLOG_DEBUG(R"(mDNS: no response for "{}" within {}ms)", hostname_, MDNS_TIMEOUT.count());
        return {};
    }

    // ── 6. Receive the response ─────────────────────────────────────────────
    std::vector<uint8_t> recv_buf(2048);
    const auto recv_len = recvfrom(sockfd, recv_buf.data(), recv_buf.size(), 0, nullptr, nullptr);

    if (recv_len < 0) {
        SPDLOG_ERROR(R"(mDNS: recvfrom() failed for "{}": {})", hostname_, std::strerror(errno));
        return {};
    }

    SPDLOG_TRACE(R"(mDNS: received {} bytes for "{}")", recv_len, hostname_);

    // ── 7. Parse the response ───────────────────────────────────────────────
    std::vector<std::string> raw_records;
    try {
        raw_records = DnsRecordParser::parse_all(recv_buf.data(), static_cast<size_t>(recv_len), hostname_);
    } catch (const std::exception &e) {
        SPDLOG_DEBUG(R"(mDNS: failed to parse response for "{}": {})", hostname_, e.what());
        return {};
    }

    if (raw_records.empty()) {
        SPDLOG_DEBUG(R"(mDNS: no records in response for "{}")", hostname_);
        return {};
    }

    // ── 8. Convert string addresses to InetAddress ──────────────────────────
    std::vector<InetAddress> results;
    results.reserve(raw_records.size());

    for (const auto &rec: raw_records) {
        // We already know the query type (A / AAAA), so parse accordingly.
        if (type_ == dns_type::A) {
            if (auto v4 = Inet4Address::parse(rec)) {
                SPDLOG_DEBUG(R"(mDNS: resolved "{}" → {})", hostname_, rec);
                results.push_back(*v4);  // implicit → InetAddress
            } else {
                SPDLOG_DEBUG(R"(mDNS: skipping non-A record "{}" for "{}")", rec, hostname_);
            }
        } else {  // dns_type::AAAA
            if (auto v6 = Inet6Address::parse(rec)) {
                SPDLOG_DEBUG(R"(mDNS: resolved "{}" → {})", hostname_, rec);
                results.push_back(*v6);  // implicit → InetAddress
            } else {
                SPDLOG_DEBUG(R"(mDNS: skipping non-AAAA record "{}" for "{}")", rec, hostname_);
            }
        }
    }

    if (results.empty()) {
        SPDLOG_DEBUG(R"(mDNS: no address records for "{}")", hostname_);
    }

    return results;
}
