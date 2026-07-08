//
// Created by Kotarou on 2026/7/2.
//

#include "mdns.h"

#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>

#include <algorithm>
#include <array>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <system_error>
#include <utility>

#include <spdlog/spdlog.h>

#include "fmt.hpp"
#include "dns/util.hpp"
#include "dns/parser/parser.h"
#include "dns/wire/builder.h"
#include "dns/wire/query.h"
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

    /// Maximum UDP payload size (65535 - IP/UDP headers).  Large enough to
    /// cover any conceivable mDNS response, including jumbo frames and EDNS0-
    /// extended payloads.
    constexpr std::size_t MDNS_RECV_BUF_SIZE = 65536;

    /// Pre-parsed multicast group addresses via C++ wrappers (no raw inet_pton).
    inline const auto MDNS_IPV4_GROUP_INET = Inet4Address::parse(MDNS_IPV4_GROUP).value();
    inline const auto MDNS_IPV6_GROUP_INET = Inet6Address::parse(MDNS_IPV6_GROUP).value();

    /// Destination addresses (group + port) — SocketAddr::from_inet handles htons() internally.
    inline const SocketAddr MDNS_IPV4_DEST = SocketAddr::from_inet(MDNS_IPV4_GROUP_INET, MDNS_PORT).value();
    inline const SocketAddr MDNS_IPV6_DEST = SocketAddr::from_inet(MDNS_IPV6_GROUP_INET, MDNS_PORT).value();

    /// Bind-on-any addresses (INADDR_ANY / in6addr_any, port = 0 = ephemeral).
    ///
    /// yaddnsc acts as a one-shot mDNS querier (RFC 6762 §5.1).  Per the RFC,
    /// one-shot queries "MUST NOT be sent using UDP source port 5353", since
    /// source port 5353 signals a fully compliant continuous querier (§5.2).
    /// Using an ephemeral port also avoids conflicts with other processes
    /// already listening on port 5353 (e.g. avahi-daemon or systemd-resolved).
    inline const SocketAddr MDNS_IPV4_BIND = SocketAddr::from_inet(Inet4Address{}, 0).value();
    inline const SocketAddr MDNS_IPV6_BIND = SocketAddr::from_inet(Inet6Address{}, 0).value();

    // ===========================================================================
    //  Utility functions
    // ===========================================================================

    [[nodiscard]] inline std::string errno_str(int err) {
        return std::error_code{err, std::generic_category()}.message();
    }

    // ===========================================================================
    //  Tag types for compile-time v4/v6 dispatch
    // ===========================================================================

    struct Ipv4Tag {};
    struct Ipv6Tag {};

    template<typename T>
    concept IpVersionTag = std::is_same_v<T, Ipv4Tag> || std::is_same_v<T, Ipv6Tag>;

    /// Forward declaration — see definition in Helper functions.
    [[nodiscard]] in_addr pick_ipv4_interface_addr(const std::string &interface);

    // ===========================================================================
    //  ScopedMembership<Tag> — RAII multicast group join / leave
    // ===========================================================================

    template<IpVersionTag Tag>
    struct ScopedMembership {
        using mreq_type = std::conditional_t<std::is_same_v<Tag, Ipv6Tag>, ipv6_mreq, ip_mreq>;

        Socket &sock_;
        mreq_type mreq_{};

        /// @param sock  The multicast socket. Must outlive this object.
        explicit ScopedMembership(Socket &sock, unsigned int if_index, const std::string &hostname,
                                  const std::string &interface) : sock_(sock) {
            if constexpr (std::is_same_v<Tag, Ipv6Tag>) {
                // Bridge: copy Inet6Address bytes → POSIX in6_addr.
                auto *v6_dest = reinterpret_cast<std::uint8_t *>(&mreq_.ipv6mr_multiaddr);
                std::ranges::copy_n(MDNS_IPV6_GROUP_INET.data(), sizeof(mreq_.ipv6mr_multiaddr), v6_dest);
                mreq_.ipv6mr_interface = if_index;
                if (auto err = sock_.try_set_option(IPPROTO_IPV6, IPV6_JOIN_GROUP, mreq_)) {
                    throw std::runtime_error(fmt::format(
                            R"(mDNS IPV6_JOIN_GROUP failed for "{}": {})", hostname, errno_str(err))
                    );
                }
            } else {
                // Bridge: copy Inet4Address bytes → POSIX in_addr.
                auto *v4_dest = reinterpret_cast<std::uint8_t *>(&mreq_.imr_multiaddr);
                std::ranges::copy_n(MDNS_IPV4_GROUP_INET.data(), sizeof(mreq_.imr_multiaddr), v4_dest);
                mreq_.imr_interface = pick_ipv4_interface_addr(interface);
                if (auto err = sock_.try_set_option(IPPROTO_IP, IP_ADD_MEMBERSHIP, mreq_)) {
                    throw std::runtime_error(fmt::format(
                            R"(mDNS IP_ADD_MEMBERSHIP failed for "{}": {})", hostname, errno_str(err))
                    );
                }
            }
        }

        ~ScopedMembership() {
            // Guard against a moved-from socket (fd_ == -1).  ScopedMembership is
            // always stack-local and outlives the socket, so this is defensive.
            if (sock_.is_closed()) {
                return;
            }
            constexpr auto level = std::is_same_v<Tag, Ipv6Tag> ? IPPROTO_IPV6 : IPPROTO_IP;
            constexpr auto leave_opt = std::is_same_v<Tag, Ipv6Tag> ? IPV6_LEAVE_GROUP : IP_DROP_MEMBERSHIP;
            if (auto err = sock_.try_set_option(level, leave_opt, mreq_)) {
                SPDLOG_WARN(R"(mDNS {} failed: {})",
                            std::is_same_v<Tag, Ipv6Tag> ? "IPV6_LEAVE_GROUP" : "IP_DROP_MEMBERSHIP", errno_str(err));
            }
        }

        ScopedMembership(const ScopedMembership &) = delete;
        ScopedMembership &operator=(const ScopedMembership &) = delete;
        // Move is implicitly deleted due to the reference member — ScopedMembership
        // is only ever used as a stack-local in resolve_mdns(), so this is intentional.
    };

    // ===========================================================================
    //  Helper functions
    // ===========================================================================

    /// Pick the IPv4 interface address for IGMP membership.
    /// Returns INADDR_ANY if the interface has no IPv4 address or is empty.
    [[nodiscard]] in_addr pick_ipv4_interface_addr(const std::string &interface) {
        if (!interface.empty()) {
            auto subnets = NetDevices::get_ipv4_subnets(interface);
            if (!subnets.empty()) {
                // Bridge: Inet4Address → POSIX in_addr.
                const auto &v4 = subnets[0].address;
                in_addr addr{};
                auto *addr_dest = reinterpret_cast<std::uint8_t *>(&addr);
                std::ranges::copy_n(v4.data(), sizeof(addr), addr_dest);
                return addr;
            }
            SPDLOG_WARN(
                R"(mDNS no IPv4 address found for interface "{}", falling back to INADDR_ANY)", interface
            );
        }
        in_addr addr{};
        addr.s_addr = INADDR_ANY;
        return addr;
    }

    /// Bind the socket to the appropriate mDNS address and set V6ONLY if needed.
    template<IpVersionTag Tag>
    void bind_socket(Socket &sock, const std::string &hostname) {
        if constexpr (std::is_same_v<Tag, Ipv6Tag>) {
            if (auto err = sock.try_set_option(IPPROTO_IPV6, IPV6_V6ONLY, 1)) {
                SPDLOG_WARN(R"(mDNS IPV6_V6ONLY failed for "{}": {})", hostname, errno_str(err));
            }
        }
        sock.bind(std::is_same_v<Tag, Ipv6Tag> ? MDNS_IPV6_BIND : MDNS_IPV4_BIND);
    }

    /// Resolve the multicast interface index.
    template<IpVersionTag Tag>
    [[nodiscard]] unsigned int resolve_multicast_if_index(const std::string &interface, const std::string &hostname) {
        unsigned int if_index = 0;
        if (!interface.empty()) {
            if_index = NetDevices::name_to_index(interface);
            if (if_index == 0) {
                SPDLOG_WARN(R"(mDNS interface "{}" not found for "{}")", interface, hostname);
            }
        // For IPv6, ipv6mr_interface = 0 may resolve to loopback, and link-local
        // multicast requires an explicit scope.  We pick the default interface.
        //
        // For IPv4, imr_interface = INADDR_ANY delegates to the kernel routing
        // table, which is safer (avoids docker/bridge interfaces).  No explicit
        // lookup needed.
        } else if constexpr (std::is_same_v<Tag, Ipv6Tag>) {
            if_index = NetDevices::find_default_interface_index(AF_INET6);
            if (if_index > 0) {
                auto if_name = NetDevices::index_to_name(if_index);
                SPDLOG_DEBUG(R"(mDNS auto-selected interface "{}" for "{}" (type AAAA))",
                             if_name.empty() ? "?" : if_name, hostname);
            }
        }
        return if_index;
    }

    /// Set multicast output options (multicast IF and TTL/HOP limit).
    template<IpVersionTag Tag>
    void setup_multicast_output_opts(Socket &sock, unsigned int if_index, [[maybe_unused]] const std::string &interface,
                                     const std::string &hostname) {
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
    }

    template<IpVersionTag Tag>
    [[nodiscard]] unsigned int setup_multicast_options(Socket &sock, const std::string &hostname, const std::string &interface) {
        bind_socket<Tag>(sock, hostname);
        auto if_index = resolve_multicast_if_index<Tag>(interface, hostname);
        setup_multicast_output_opts<Tag>(sock, if_index, interface, hostname);
        return if_index;
    }

    /// Shared helper: poll, receive, parse DNS response.
    /// Throws std::runtime_error on any failure.
    std::vector<InetAddress> recv_and_parse(Socket &sock, RecordKind type, const std::string &hostname) {
        int ready = sock.wait_for(POLLIN, MDNS_TIMEOUT_MS);
        if (ready <= 0) {
            throw std::runtime_error(
                fmt::format(R"(mDNS no response for "{}" within {}ms)", hostname, MDNS_TIMEOUT_MS)
            );
        }

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init): recv_buf is overwritten by recv_from().
        std::array<std::uint8_t, MDNS_RECV_BUF_SIZE> recv_buf;
        SocketAddr src_addr;
        auto buf = std::as_writable_bytes(std::span{recv_buf});
        ssize_t recv_len = sock.recv_from(buf, &src_addr);
        if (recv_len < 0) {
            int e = errno;
            throw std::runtime_error(
                fmt::format(R"(mDNS recvfrom() failed for "{}": {})", hostname, errno_str(e))
            );
        }

        SPDLOG_TRACE(R"(mDNS received {} bytes for "{}")", recv_len, hostname);

        auto parsed = DNS::RecordParser::parse_strings(std::span{recv_buf.data(), static_cast<size_t>(recv_len)}, hostname);
        if (parsed.records.empty()) {
            throw std::runtime_error(fmt::format(R"(mDNS no records in response for "{}")", hostname));
        }

        std::vector<InetAddress> results;
        results.reserve(parsed.records.size());
        for (const auto &rec: parsed.records) {
            if (type == RecordKind::A) {
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
    std::vector<InetAddress> resolve_mdns(const std::string &hostname, RecordKind type, const std::string &interface) {
        constexpr int af = std::is_same_v<Tag, Ipv6Tag> ? AF_INET6 : AF_INET;
        const auto &dest_addr = std::is_same_v<Tag, Ipv6Tag> ? MDNS_IPV6_DEST : MDNS_IPV4_DEST;

        const auto &iface_label = interface.empty() ? "<default>" : interface;
        SPDLOG_DEBUG(R"(mDNS resolving "{}" (type {}) on interface "{}")", hostname, std::is_same_v<Tag,
                     Ipv6Tag> ? "AAAA" : "A", iface_label);

        // Build mDNS query directly via QueryBuilder.
        // mDNS specifics (RFC 6762): TXID=0, RD=0, QCLASS carries QU bit (0x8000).
        constexpr std::uint16_t QU_BIT = 0x8000;
        const auto query_pkt = DNS::QueryBuilder{}
            .id(0)
            .rd(false)
            .add_question_raw_qclass(hostname,
                DNS::Util::type_to_record_type(type),
                static_cast<std::uint16_t>(DNS::RecordClass::IN) | QU_BIT)
            .build();
        Socket sock(af, SOCK_DGRAM);

        // ── Socket options ──────────────────────────────────────────────────
        if (auto err = sock.try_set_option(SOL_SOCKET, SO_REUSEADDR, 1)) {
            SPDLOG_WARN(R"(mDNS setsockopt(SOL_SOCKET, SO_REUSEADDR) failed for "{}": {})", hostname,
                        errno_str(err));
        }

        // SO_REUSEPORT allows co-existence with other mDNS responders (e.g. avahi-daemon).
        if (auto err = sock.try_set_option(SOL_SOCKET, SO_REUSEPORT, 1)) {
            SPDLOG_DEBUG(R"(mDNS setsockopt(SOL_SOCKET, SO_REUSEPORT) failed for "{}": {})", hostname,
                         errno_str(err));
        }

        if (!interface.empty()) {
#if defined(SO_BINDTODEVICE)
            if (auto err = sock.try_set_option_raw(SOL_SOCKET, SO_BINDTODEVICE, interface.c_str(),
                                                     static_cast<socklen_t>(interface.size() + 1))) {
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
#elif defined(__FreeBSD__)
            // FreeBSD does not have SO_BINDTODEVICE or IP_BOUND_IF.
            // IP_MULTICAST_IF/IPV6_MULTICAST_IF (set in setup_multicast_output_opts)
            // and the multicast group join are sufficient for mDNS.
            SPDLOG_DEBUG(R"(mDNS interface "{}" will be used via multicast options for "{}")", interface,
                         hostname);
#else
            // Platform does not support per-interface binding (no SO_BINDTODEVICE,
            // no IP_BOUND_IF). The interface constraint is silently ignored.
            static bool warned = false;  // NOLINT(cert-err58-cpp)
            if (!warned) {
                SPDLOG_WARN(R"(mDNS interface binding not supported on this platform, interface setting will be ignored)");
                warned = true;
            }
#endif
        }

        // ── Bind + multicast options ────────────────────────────────────────
        unsigned int if_index = setup_multicast_options<Tag>(sock, hostname, interface);

        // ── Join multicast group ──────────────────────────────────────────
        ScopedMembership<Tag> membership{sock, if_index, hostname, interface};

        // ── Send query ──────────────────────────────────────────────────────
        auto data = std::as_bytes(std::span{query_pkt});
        if (sock.send_to(data, dest_addr) < 0) {
            int e = errno;
            throw std::runtime_error(
                fmt::format(R"(mDNS sendto() failed for "{}": {})", hostname, errno_str(e)));
        }

        SPDLOG_TRACE(R"(mDNS sent {} bytes for "{}")", query_pkt.size(), hostname);

        // ── Receive & parse ─────────────────────────────────────────────────
        return recv_and_parse(sock, type, hostname);
    }
} // anonymous namespace

// ===========================================================================
//  MdnsIpSource — public API
// ===========================================================================

MdnsIpSource::MdnsIpSource(std::string hostname, RecordKind type, std::string interface)
    : hostname_(std::move(hostname)), type_(type), interface_(std::move(interface)) {
}

std::vector<InetAddress> MdnsIpSource::resolve() const {
    if (type_ == RecordKind::AAAA) {
        return resolve_mdns<Ipv6Tag>(hostname_, type_, interface_);
    }

    return resolve_mdns<Ipv4Tag>(hostname_, type_, interface_);
}
