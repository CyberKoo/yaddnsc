//
// Created by Kotarou on 2026/7/2.
//

#include "mdns.h"

#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>

#include <array>
#include <bit>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <system_error>

#include <spdlog/spdlog.h>

#include "fmt.hpp"
#include "dns/util.hpp"
#include "dns/proto/parser.h"
#include "dns/proto/mkquery.h"
#include "network/inet_address.h"
#include "network/net_devices.h"
#include "network/socket.h"

namespace {
    // ===========================================================================
    //  Constants
    // ===========================================================================

    constexpr std::uint16_t MDNS_PORT = 5353;
    constexpr auto MDNS_IPV4_GROUP = "224.0.0.251";
    constexpr auto MDNS_IPV6_GROUP = "ff02::fb";
    constexpr auto MDNS_TIMEOUT_MS = 500;

    inline const SocketAddr MDNS_IPV4_DEST = [] {
        sockaddr_storage d{};
        auto &v4 = *reinterpret_cast<sockaddr_in *>(&d);
        v4.sin_family = AF_INET;
        v4.sin_port = htons(MDNS_PORT);
        if (inet_pton(AF_INET, MDNS_IPV4_GROUP, &v4.sin_addr) != 1) {
            throw std::logic_error("mDNS: inet_pton failed for hardcoded IPv4 multicast group");
        }
        return SocketAddr::from_raw(reinterpret_cast<const sockaddr *>(&d), sizeof(v4));
    }();

    inline const SocketAddr MDNS_IPV6_DEST = [] {
        sockaddr_storage d{};
        auto &v6 = *reinterpret_cast<sockaddr_in6 *>(&d);
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(MDNS_PORT);
        if (inet_pton(AF_INET6, MDNS_IPV6_GROUP, &v6.sin6_addr) != 1) {
            throw std::logic_error("mDNS: inet_pton failed for hardcoded IPv6 multicast group");
        }
        return SocketAddr::from_raw(reinterpret_cast<const sockaddr *>(&d), sizeof(v6));
    }();

    inline const SocketAddr MDNS_IPV4_BIND = [] {
        sockaddr_in bind{};
        bind.sin_family = AF_INET;
        bind.sin_addr.s_addr = INADDR_ANY;
        return SocketAddr::from_raw(reinterpret_cast<const sockaddr *>(&bind), sizeof(bind));
    }();

    inline const SocketAddr MDNS_IPV6_BIND = [] {
        sockaddr_in6 bind{};
        bind.sin6_family = AF_INET6;
        bind.sin6_addr = in6addr_any;
        return SocketAddr::from_raw(reinterpret_cast<const sockaddr *>(&bind), sizeof(bind));
    }();

    // ===========================================================================
    //  Utility functions
    // ===========================================================================

    [[nodiscard]] inline std::string errno_str(int err = errno) {
        return std::error_code{err, std::generic_category()}.message();
    }

    // ===========================================================================
    //  Tag types for compile-time v4/v6 dispatch
    // ===========================================================================

    struct Ipv4Tag {
    };

    struct Ipv6Tag {
    };

    template<typename T>
    concept IpVersionTag = std::is_same_v<T, Ipv4Tag> || std::is_same_v<T, Ipv6Tag>;

    /// Forward declaration — see definition in Helper functions.
    in_addr pick_ipv4_interface_addr(const std::string &interface);

    // ===========================================================================
    //  ScopedMembership<Tag> — RAII multicast group join / leave
    // ===========================================================================

    template<IpVersionTag Tag>
    struct ScopedMembership {
        using mreq_type = std::conditional_t<std::is_same_v<Tag, Ipv6Tag>, ipv6_mreq, ip_mreq>;

        int fd_;
        mreq_type mreq_;

        explicit ScopedMembership(int fd, unsigned int if_index, const std::string &hostname,
                                  const std::string &interface) : fd_(fd) {
            if constexpr (std::is_same_v<Tag, Ipv6Tag>) {
                if (inet_pton(AF_INET6, MDNS_IPV6_GROUP, &mreq_.ipv6mr_multiaddr) != 1) {
                    throw std::logic_error("mDNS: inet_pton failed for hardcoded IPv6 multicast group");
                }
                mreq_.ipv6mr_interface = if_index;
                if (setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq_, sizeof(mreq_)) < 0) {
                    throw std::runtime_error(fmt::format(
                            R"(mDNS IPV6_JOIN_GROUP failed for "{}": {})", hostname, errno_str())
                    );
                }
            } else {
                if (inet_pton(AF_INET, MDNS_IPV4_GROUP, &mreq_.imr_multiaddr) != 1) {
                    throw std::logic_error("mDNS: inet_pton failed for hardcoded IPv4 multicast group");
                }
                mreq_.imr_interface = pick_ipv4_interface_addr(interface);
                if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq_, sizeof(mreq_)) < 0) {
                    throw std::runtime_error(fmt::format(
                            R"(mDNS IP_ADD_MEMBERSHIP failed for "{}": {})", hostname, errno_str())
                    );
                }
            }
        }

        ~ScopedMembership() {
            int ret;
            if constexpr (std::is_same_v<Tag, Ipv6Tag>) {
                ret = setsockopt(fd_, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mreq_, sizeof(mreq_));
            } else {
                ret = setsockopt(fd_, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq_, sizeof(mreq_));
            }
            if (ret < 0) {
                SPDLOG_WARN(R"(mDNS {}_LEAVE_GROUP failed: {})",
                            std::is_same_v<Tag, Ipv6Tag> ? "IPV6" : "IP", errno_str());
            }
        }

        ScopedMembership(const ScopedMembership &) = delete;

        ScopedMembership &operator=(const ScopedMembership &) = delete;

        ScopedMembership(ScopedMembership &&) = default;

        ScopedMembership &operator=(ScopedMembership &&) = default;
    };

    // ===========================================================================
    //  Helper functions
    // ===========================================================================

    /// Pick the IPv4 interface address for IGMP membership.
    /// Returns INADDR_ANY if the interface has no IPv4 address or is empty.
    in_addr pick_ipv4_interface_addr(const std::string &interface) {
        if (!interface.empty()) {
            auto subnets = NetDevices::get_ipv4_subnets(interface);
            if (!subnets.empty()) {
                const auto &addr_bytes = subnets[0].address.addr();
                return std::bit_cast<in_addr>(addr_bytes);
            }
            SPDLOG_WARN(
                R"(mDNS no IPv4 address found for interface "{}", falling back to INADDR_ANY)", interface
            );
        }
        in_addr addr{};
        addr.s_addr = htonl(INADDR_ANY);
        return addr;
    }

    template<IpVersionTag Tag>
    unsigned int setup_multicast_options(Socket &sock, const std::string &hostname, const std::string &interface) {
        // ── Bind ────────────────────────────────────────────────────────────
        if constexpr (std::is_same_v<Tag, Ipv6Tag>) {
            if (auto err = sock.try_set_option(IPPROTO_IPV6, IPV6_V6ONLY, 1)) {
                SPDLOG_WARN(R"(mDNS IPV6_V6ONLY failed for "{}": {})", hostname, errno_str(err));
            }
        }

        sock.bind(std::is_same_v<Tag, Ipv6Tag> ? MDNS_IPV6_BIND : MDNS_IPV4_BIND);

        // ── Multicast options ──────────────────────────────────────────────
        unsigned int if_index = 0;
        if (!interface.empty()) {
            if_index = NetDevices::name_to_index(interface);
            if (if_index == 0) {
                SPDLOG_WARN(R"(mDNS interface "{}" not found for "{}")", interface, hostname);
            }
        } else if constexpr (std::is_same_v<Tag, Ipv6Tag>) {
            if_index = NetDevices::find_default_interface_index(AF_INET6);
            if (if_index > 0) {
                auto if_name = NetDevices::index_to_name(if_index);
                SPDLOG_DEBUG(R"(mDNS auto-selected interface "{}" for "{}" (type AAAA))",
                             if_name.empty() ? "?" : if_name, hostname);
            }
        }

        if constexpr (std::is_same_v<Tag, Ipv6Tag>) {
            if (if_index > 0) {
                if (auto err = sock.try_set_option(IPPROTO_IPV6, IPV6_MULTICAST_IF, if_index)) {
                    SPDLOG_WARN(R"(mDNS IPV6_MULTICAST_IF failed for "{}": {})", hostname, errno_str(err));
                }
            }
            if (auto err = sock.try_set_option(IPPROTO_IPV6, IPV6_MULTICAST_HOPS, 255)) {
                SPDLOG_WARN(R"(mDNS IPV6_MULTICAST_HOPS failed for "{}": {})", hostname, errno_str(err));
            }
        } else {
            if (if_index > 0) {
#if defined(__linux__)
                ip_mreqn mreqn{};
                mreqn.imr_ifindex = static_cast<int>(if_index);
                if (auto err = sock.try_set_option(IPPROTO_IP, IP_MULTICAST_IF, mreqn)) {
                    SPDLOG_WARN(R"(mDNS IP_MULTICAST_IF failed for "{}": {})", hostname, errno_str(err));
                }
#else
                // macOS / BSD: IP_MULTICAST_IF expects struct in_addr (interface
                // address), not an interface index.
                auto ipv4_addr = pick_ipv4_interface_addr(interface);
                if (auto err = sock.try_set_option(IPPROTO_IP, IP_MULTICAST_IF, ipv4_addr)) {
                    SPDLOG_WARN(R"(mDNS IP_MULTICAST_IF failed for "{}": {})", hostname, errno_str(err));
                }
#endif
            }
            if (auto err = sock.try_set_option(IPPROTO_IP, IP_MULTICAST_TTL, 255)) {
                SPDLOG_WARN(R"(mDNS IP_MULTICAST_TTL failed for "{}": {})", hostname, errno_str(err));
            }
        }

        return if_index;
    }

    /// Shared helper: poll, receive, validate port, parse DNS response.
    /// Throws std::runtime_error on any failure.
    std::vector<InetAddress> recv_and_parse(Socket &sock, DNS::Type type, const std::string &hostname) {
        int ready = sock.wait_for(POLLIN, MDNS_TIMEOUT_MS);
        if (ready <= 0) {
            throw std::runtime_error(
                fmt::format(R"(mDNS no response for "{}" within {}ms)", hostname, MDNS_TIMEOUT_MS)
            );
        }

        std::vector<std::uint8_t> recv_buf(2048);
        SocketAddr src_addr;
        auto buf = std::as_writable_bytes(std::span{recv_buf});
        ssize_t recv_len = sock.recv_from(buf, &src_addr);
        if (recv_len < 0) {
            throw std::runtime_error(
                fmt::format(R"(mDNS recvfrom() failed for "{}": {})", hostname, errno_str())
            );
        }

        // if (src_addr.port() != MDNS_PORT) {
        //     // RFC 6762 §6.7: unicast responses MAY be sent from ephemeral ports.
        //     // Treat this as a warning, not a hard error.
        //     SPDLOG_WARN(R"(mDNS response for "{}" came from non-standard port {} (expected 5353); "
        //                 R"(RFC 6762 §6.7 permits ephemeral-source unicast replies)", hostname, src_addr.port());
        // }

        SPDLOG_TRACE(R"(mDNS received {} bytes for "{}")", recv_len, hostname);

        auto raw_records = DNS::DnsRecordParser::parse_all(recv_buf.data(), recv_len, hostname);
        if (raw_records.empty()) {
            throw std::runtime_error(fmt::format(R"(mDNS no records in response for "{}")", hostname));
        }

        std::vector<InetAddress> results;
        results.reserve(raw_records.size());
        for (const auto &rec: raw_records) {
            if (type == DNS::Type::A) {
                if (auto v4 = Inet4Address::parse(rec)) {
                    SPDLOG_DEBUG(R"(mDNS resolved "{}" → {})", hostname, rec);
                    results.emplace_back(*v4);
                } else {
                    SPDLOG_DEBUG(R"(mDNS skipping non-A record "{}" for "{}")", rec, hostname);
                }
            } else {
                if (auto v6 = Inet6Address::parse(rec)) {
                    SPDLOG_DEBUG(R"(mDNS resolved "{}" → "{}")", hostname, rec);
                    results.emplace_back(*v6);
                } else {
                    SPDLOG_DEBUG(R"(mDNS skipping non-AAAA record "{}" for "{}")", rec, hostname);
                }
            }
        }

        if (results.empty()) {
            throw std::runtime_error(fmt::format(R"(mDNS no address records for "{}")", hostname));
        }

        return results;
    }

    // ===========================================================================
    //  resolve_mdns<Tag>()  —  one instantiation per address family
    // ===========================================================================

    template<IpVersionTag Tag>
    std::vector<InetAddress> resolve_mdns(const std::string &hostname, DNS::Type type, const std::string &interface) {
        constexpr int af = std::is_same_v<Tag, Ipv6Tag> ? AF_INET6 : AF_INET;
        constexpr const auto &dest_addr = std::is_same_v<Tag, Ipv6Tag> ? MDNS_IPV6_DEST : MDNS_IPV4_DEST;

        const auto &iface_label = interface.empty() ? "<default>" : interface;
        SPDLOG_DEBUG(R"(mDNS resolving "{}" (type {}) on interface "{}")", hostname, std::is_same_v<Tag,
                     Ipv6Tag> ? "AAAA" : "A", iface_label);

        const auto query_pkt = DNS::mkquery_mdns(hostname, DNS::Util::to_ns_type(type), true);
        Socket sock(af, SOCK_DGRAM);

        // ── Socket options ──────────────────────────────────────────────────
        if (auto err = sock.try_set_option(SOL_SOCKET, SO_REUSEADDR, 1)) {
            SPDLOG_WARN(R"(mDNS setsockopt(SOL_SOCKET, SO_REUSEADDR) failed for "{}": {})", hostname,
                        errno_str(err));
        }

        if (!interface.empty()) {
#if defined(SO_BINDTODEVICE)
            if (auto err = sock.try_set_option_raw(SOL_SOCKET, SO_BINDTODEVICE, interface.c_str(), interface.size())) {
                SPDLOG_WARN(R"(mDNS setsockopt(SOL_SOCKET, SO_BINDTODEVICE) failed for "{}": {})", hostname,
                            errno_str(err));
            }
#elif defined(IP_BOUND_IF)
            unsigned int idx = NetDevices::name_to_index(interface);
            if (idx > 0) {
                constexpr int ip_level = std::is_same_v<Tag, Ipv6Tag> ? IPPROTO_IPV6 : IPPROTO_IP;
                constexpr int opt_name = std::is_same_v<Tag, Ipv6Tag> ? IPV6_BOUND_IF : IP_BOUND_IF;
                if (auto err = sock.try_set_option(ip_level, opt_name, idx)) {
                    SPDLOG_WARN(R"(mDNS setsockopt bound-if failed for "{}": {})", hostname, errno_str(err));
                }
            } else {
                SPDLOG_WARN(R"(mDNS interface "{}" not found for "{}")", interface, hostname);
            }
#endif
        }

        // ── Bind + multicast options ────────────────────────────────────────
        unsigned int if_index = setup_multicast_options<Tag>(sock, hostname, interface);

        // ── Join multicast group ──────────────────────────────────────────
        ScopedMembership<Tag> membership{sock.native_handle(), if_index, hostname, interface};

        // ── Send query ──────────────────────────────────────────────────────
        auto data = std::as_bytes(std::span{query_pkt});
        if (sock.send_to(data, dest_addr) < 0) {
            throw std::runtime_error(
                fmt::format(R"(mDNS sendto() failed for "{}": {})", hostname, errno_str()));
        }

        SPDLOG_TRACE(R"(mDNS sent {} bytes for "{}")", query_pkt.size(), hostname);

        // ── Receive & parse ─────────────────────────────────────────────────
        return recv_and_parse(sock, type, hostname);
    }
} // anonymous namespace

// ===========================================================================
//  MdnsIpSource — public API
// ===========================================================================

MdnsIpSource::MdnsIpSource(std::string hostname, DNS::Type type, std::string interface)
    : hostname_(std::move(hostname)), type_(type), interface_(std::move(interface)) {
}

std::vector<InetAddress> MdnsIpSource::resolve() const {
    if (type_ == DNS::Type::AAAA) {
        return resolve_mdns<Ipv6Tag>(hostname_, type_, interface_);
    }

    return resolve_mdns<Ipv4Tag>(hostname_, type_, interface_);
}
