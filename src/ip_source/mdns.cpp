//
// Created by Kotarou on 2026/7/2.
//

#include "mdns.h"

#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bit>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>

#include <spdlog/spdlog.h>

#include "fmt.hpp"
#include "dns/util.hpp"
#include "dns/proto/parser.h"
#include "dns/proto/mkquery.h"
#include "network/inet_address.h"
#include "network/net_devices.h"

// ===========================================================================
//  mDNS well-known constants (RFC 6762)
// ===========================================================================

namespace {
    constexpr uint16_t MDNS_PORT = 5353;

    // IPv4 multicast group
    constexpr auto MDNS_IPV4_GROUP = "224.0.0.251";

    // IPv6 multicast group
    constexpr auto MDNS_IPV6_GROUP = "ff02::fb";

    // How long to wait for a response (milliseconds).
    constexpr auto MDNS_TIMEOUT_MS = 500;

    inline const sockaddr_storage MDNS_IPV4_DEST = [] {
        sockaddr_storage d{};
        auto &v4 = *reinterpret_cast<sockaddr_in *>(&d);
        v4.sin_family = AF_INET;
        v4.sin_port = htons(MDNS_PORT);
        inet_pton(AF_INET, MDNS_IPV4_GROUP, &v4.sin_addr);
        return d;
    }();

    inline const sockaddr_storage MDNS_IPV6_DEST = [] {
        sockaddr_storage d{};
        auto &v6 = *reinterpret_cast<sockaddr_in6 *>(&d);
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(MDNS_PORT);
        inet_pton(AF_INET6, MDNS_IPV6_GROUP, &v6.sin6_addr);
        return d;
    }();

    // RAII wrapper that closes the fd on scope exit.
    struct ScopedFd {
        int fd_;

        explicit ScopedFd(int f) : fd_(f) {
        }

        ~ScopedFd() {
            if (fd_ >= 0) ::close(fd_);
        }

        ScopedFd(const ScopedFd &) = delete;

        ScopedFd &operator=(const ScopedFd &) = delete;
    };

    // RAII wrapper that joins the multicast group on construction and
    // leaves it on destruction. Must be destroyed *before* ScopedFd
    // (i.e., declared after it).
    struct ScopedMembership {
        int fd_;
        bool is_ipv6_;
        unsigned int if_index_;
        std::string label_;
        std::optional<in_addr> iface_addr_;

        explicit ScopedMembership(int fd, bool is_ipv6, unsigned int if_index, std::string label,
                                  std::optional<in_addr> iface_addr = std::nullopt) : fd_(fd), is_ipv6_(is_ipv6),
            if_index_(if_index), label_(std::move(label)), iface_addr_(iface_addr) {
            if (is_ipv6_) {
                ipv6_mreq mreq6{};
                inet_pton(AF_INET6, MDNS_IPV6_GROUP, &mreq6.ipv6mr_multiaddr);
                mreq6.ipv6mr_interface = if_index_;
                if (setsockopt(fd_, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq6, sizeof(mreq6)) < 0) {
                    throw std::runtime_error(
                        fmt::format(R"(mDNS: IPV6_JOIN_GROUP failed for "{}": {})", label_, std::strerror(errno))
                    );
                }
            } else {
                ip_mreq mreq{};
                inet_pton(AF_INET, MDNS_IPV4_GROUP, &mreq.imr_multiaddr);
                if (iface_addr_.has_value()) {
                    mreq.imr_interface = *iface_addr_;
                } else {
                    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
                }
                if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
                    throw std::runtime_error(
                        fmt::format(R"(mDNS: IP_ADD_MEMBERSHIP failed for "{}": {})", label_, std::strerror(errno))
                    );
                }
            }
        }

        ~ScopedMembership() {
            if (is_ipv6_) {
                ipv6_mreq mreq6{};
                inet_pton(AF_INET6, MDNS_IPV6_GROUP, &mreq6.ipv6mr_multiaddr);
                mreq6.ipv6mr_interface = if_index_;
                setsockopt(fd_, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mreq6, sizeof(mreq6));
            } else {
                ip_mreq mreq{};
                inet_pton(AF_INET, MDNS_IPV4_GROUP, &mreq.imr_multiaddr);
                if (iface_addr_.has_value()) {
                    mreq.imr_interface = *iface_addr_;
                } else {
                    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
                }
                setsockopt(fd_, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
            }
        }

        ScopedMembership(const ScopedMembership &) = delete;

        ScopedMembership &operator=(const ScopedMembership &) = delete;
    };
} // anonymous namespace

// ===========================================================================
//  MdnsIpSource — public API
// ===========================================================================

MdnsIpSource::MdnsIpSource(std::string hostname, DNS::Type type, std::string interface)
    : hostname_(std::move(hostname)), type_(type), interface_(std::move(interface)) {
}

std::vector<InetAddress> MdnsIpSource::resolve() const {
    const bool is_ipv6 = (type_ == DNS::Type::AAAA);
    const int af = is_ipv6 ? AF_INET6 : AF_INET;

    const auto &iface_label = interface_.empty() ? "<default>" : interface_;
    SPDLOG_DEBUG(R"(mDNS: resolving "{}" (type {}) on interface "{}")", hostname_, is_ipv6 ? "AAAA" : "A", iface_label);

    // ── 1. Build the mDNS query packet ──────────────────────────────────────
    const auto query_pkt = DNS::mkquery_mdns(hostname_, DNS::to_ns_type(type_), true);

    // ── 2. Create the socket ────────────────────────────────────────────────
    const int sockfd = ::socket(af, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        throw std::runtime_error(
            fmt::format(R"(mDNS: socket() failed for "{}": {})", hostname_, std::strerror(errno))
        );
    }
    ScopedFd guard{sockfd};

    // Allow multiple listeners on the same port (RFC 6762 §15.1).
    int reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        SPDLOG_WARN(R"(mDNS: setsockopt(SO_REUSEADDR) failed for "{}": {})", hostname_, std::strerror(errno));
    }

    // Bind the socket to the requested interface.
    if (!interface_.empty()) {
#if defined(SO_BINDTODEVICE)
        if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, interface_.c_str(), interface_.size()) < 0) {
            SPDLOG_WARN(R"(mDNS: setsockopt(SO_BINDTODEVICE, "{}") failed for "{}": {})", interface_, hostname_,
                        std::strerror(errno));
        }
#elif defined(IP_BOUND_IF)
        unsigned int idx = NetDevices::name_to_index(interface_);
        if (idx > 0) {
            int ip_level = is_ipv6 ? IPPROTO_IPV6 : IPPROTO_IP;
            int opt_name = is_ipv6 ? IPV6_BOUND_IF : IP_BOUND_IF;
            if (setsockopt(sockfd, ip_level, opt_name, &idx, sizeof(idx)) < 0) {
                SPDLOG_WARN(R"(mDNS: setsockopt bound-if failed for "{}": {})", hostname_, std::strerror(errno));
            }
        } else {
            SPDLOG_WARN(R"(mDNS: interface "{}" not found for "{}")", interface_, hostname_);
        }
#endif
    }

    // Bind ephemeral port — mDNS responses come back to this port.
    // For IPv6, also request v6-only to avoid v4-mapped addresses.
    if (is_ipv6) {
        int v6only = 1;
        if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0) {
            SPDLOG_WARN(R"(mDNS: IPV6_V6ONLY failed for "{}": {})", hostname_, std::strerror(errno));
        }

        sockaddr_in6 bind_addr{};
        bind_addr.sin6_family = AF_INET6;
        bind_addr.sin6_addr = in6addr_any;
        bind_addr.sin6_port = 0;
        if (bind(sockfd, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)) < 0) {
            throw std::runtime_error(
                fmt::format(R"(mDNS: bind() failed for "{}": {})", hostname_, std::strerror(errno))
            );
        }
    } else {
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port = 0;
        if (bind(sockfd, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)) < 0) {
            throw std::runtime_error(
                fmt::format(R"(mDNS: bind() failed for "{}": {})", hostname_, std::strerror(errno))
            );
        }
    }

    // ── 3. Set multicast socket options (required on FreeBSD/BSD) ────────────
    unsigned int if_index = 0;
    if (!interface_.empty()) {
        if_index = NetDevices::name_to_index(interface_);
        if (if_index == 0) {
            SPDLOG_WARN(R"(mDNS: interface "{}" not found for "{}")", interface_, hostname_);
        }
    } else if (is_ipv6) {
        if_index = NetDevices::find_default_interface_index(AF_INET6);
        if (if_index > 0) {
            auto if_name = NetDevices::index_to_name(if_index);
            SPDLOG_DEBUG(R"(mDNS: auto-selected interface "{}" (index {}) for "{}")", if_name.empty() ? "?" : if_name,
                         if_index, hostname_);
        }
    }

    if (is_ipv6) {
        // Set the outgoing interface for IPv6 multicast (FreeBSD needs this to
        // route the sendto() to the correct interface).  When no specific
        // interface was requested, leave the kernel default alone — passing 0
        // as the index can fail on macOS with EINVAL.
        if (if_index > 0) {
            if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &if_index, sizeof(if_index)) < 0) {
                SPDLOG_WARN(R"(mDNS: IPV6_MULTICAST_IF failed for "{}": {})", hostname_, std::strerror(errno));
            }
        }

        // Explicit hop-limit for multicast packets (default is 1 on most
        // systems, but being explicit avoids surprises on FreeBSD).
        int hops = 255;
        if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)) < 0) {
            SPDLOG_WARN(R"(mDNS: IPV6_MULTICAST_HOPS failed for "{}": {})", hostname_, std::strerror(errno));
        }
    } else {
        // Set the outgoing interface for IPv4 multicast (optional on most
        // systems, but good practice when pinned to a specific interface).
        if (if_index > 0) {
#if defined(__linux__)
            // Linux: IP_MULTICAST_IF expects struct ip_mreqn (with imr_ifindex).
            struct ip_mreqn mreqn{};
            mreqn.imr_ifindex = static_cast<int>(if_index);
            if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_IF, &mreqn, sizeof(mreqn)) < 0) {
                SPDLOG_WARN(R"(mDNS: IP_MULTICAST_IF failed for "{}": {})", hostname_, std::strerror(errno));
            }
#else
            // macOS/BSD: IP_MULTICAST_IF expects unsigned int interface index.
            if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_IF, &if_index, sizeof(if_index)) < 0) {
                SPDLOG_WARN(R"(mDNS: IP_MULTICAST_IF failed for "{}": {})", hostname_, std::strerror(errno));
            }
#endif
        }

        // Explicit TTL for multicast packets.
        int ttl = 255;
        if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
            SPDLOG_WARN(R"(mDNS: IP_MULTICAST_TTL failed for "{}": {})", hostname_, std::strerror(errno));
        }
    }

    // Resolve the local interface address for IPv4 multicast membership.
    // Done here so ScopedMembership focuses only on join/leave (SRP).
    // std::nullopt ⇒ INADDR_ANY (when no interface was requested).
    std::optional<in_addr> iface_addr;
    if (!is_ipv6 && !interface_.empty()) {
        auto subnets = NetDevices::get_ipv4_subnets(interface_);
        if (!subnets.empty()) {
            const auto &addr_bytes = subnets[0].address.addr();
            iface_addr = std::bit_cast<in_addr>(addr_bytes);
        } else {
            SPDLOG_WARN(R"(mDNS: no IPv4 address found for interface "{}" for "{}", falling back to INADDR_ANY)",
                        interface_, hostname_);
        }
    }

    // RAII join/leave (destroyed before ScopedFd, so fd is still valid).
    ScopedMembership membership{sockfd, is_ipv6, if_index, hostname_, iface_addr};

    // ── 4. Send the query to the multicast group ────────────────────────────
    const auto &dest = is_ipv6 ? MDNS_IPV6_DEST : MDNS_IPV4_DEST;
    socklen_t dest_len = is_ipv6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);

    const auto sent = sendto(sockfd, query_pkt.data(), query_pkt.size(), 0,
                             reinterpret_cast<const sockaddr *>(&dest), dest_len);

    if (sent < 0) {
        throw std::runtime_error(
            fmt::format(R"(mDNS: sendto() failed for "{}": {})", hostname_, std::strerror(errno))
        );
    }

    SPDLOG_TRACE(R"(mDNS: sent {} bytes for "{}")", sent, hostname_);

    // ── 5. Wait for a response (with timeout) ───────────────────────────────
    pollfd pfd{};
    pfd.fd = sockfd;
    pfd.events = POLLIN;

    const int ready = poll(&pfd, 1, MDNS_TIMEOUT_MS);
    if (ready <= 0) {
        SPDLOG_DEBUG(R"(mDNS: no response for "{}" within {}ms)", hostname_, MDNS_TIMEOUT_MS);
        return {};
    }

    // ── 6. Receive the response ─────────────────────────────────────────────
    std::vector<uint8_t> recv_buf(2048);

    sockaddr_storage src_addr{};
    socklen_t src_len = sizeof(src_addr);
    const auto recv_len = recvfrom(sockfd, recv_buf.data(), recv_buf.size(), 0,
                                   reinterpret_cast<sockaddr *>(&src_addr), &src_len);

    if (recv_len < 0) {
        throw std::runtime_error(
            fmt::format(R"(mDNS: recvfrom() failed for "{}": {})", hostname_, std::strerror(errno))
        );
    }

    // Verify the response came from the mDNS port (5353).
    // This filters out unrelated multicast traffic that may arrive
    // on the same socket (e.g. from other services on the same group).
    uint16_t src_port = 0;
    if (src_addr.ss_family == AF_INET) {
        src_port = ntohs(reinterpret_cast<sockaddr_in *>(&src_addr)->sin_port);
    } else if (src_addr.ss_family == AF_INET6) {
        src_port = ntohs(reinterpret_cast<sockaddr_in6 *>(&src_addr)->sin6_port);
    }
    if (src_port != MDNS_PORT) {
        SPDLOG_WARN(R"(mDNS: received response for "{}" from non-mDNS port {}, ignoring)", hostname_, src_port);
        return {};
    }

    SPDLOG_TRACE(R"(mDNS: received {} bytes for "{}")", recv_len, hostname_);

    // ── 7. Parse the response ───────────────────────────────────────────────
    std::vector<std::string> raw_records;
    try {
        raw_records = DNS::DnsRecordParser::parse_all(recv_buf.data(), static_cast<size_t>(recv_len), hostname_);
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
        if (type_ == DNS::Type::A) {
            if (auto v4 = Inet4Address::parse(rec)) {
                SPDLOG_DEBUG(R"(mDNS: resolved "{}" → {})", hostname_, rec);
                results.emplace_back(*v4); // implicit → InetAddress
            } else {
                SPDLOG_DEBUG(R"(mDNS: skipping non-A record "{}" for "{}")", rec, hostname_);
            }
        } else {
            // DNS::Type::AAAA
            if (auto v6 = Inet6Address::parse(rec)) {
                SPDLOG_DEBUG(R"(mDNS: resolved "{}" → "{}")", hostname_, rec);
                results.emplace_back(*v6); // implicit → InetAddress
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
