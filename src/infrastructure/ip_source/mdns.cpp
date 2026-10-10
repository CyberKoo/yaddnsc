//
// ip_source — coroutine mDNS source (implementation).
//

#include "mdns.h"

#include <net/if.h>
#include <netinet/in.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <optional>

#include "domain/error/error.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/coro/cancelled.h"
#include "infrastructure/coro/scope.hpp"
#include "infrastructure/dns/dns_packet_exception.h"
#include "infrastructure/dns/types.h"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/wire/builder.h"
#include "infrastructure/ip_source/iface_util.h"
#include "infrastructure/ip_source/mdns_response.h"
#include "infrastructure/network/transport/socket_ops.h"
#include "support/fmt.hpp"
#include "support/util/fd.hpp"
#include "domain/dns/record_kind.h"
#include "infrastructure/network/transport/datagram.h"
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

namespace ipsource {
namespace {

constexpr std::uint16_t MDNS_PORT = 5353;
constexpr std::chrono::milliseconds MDNS_TIMEOUT{500};
constexpr std::string_view MDNS_IPV4_GROUP = "224.0.0.251";
constexpr std::string_view MDNS_IPV6_GROUP = "ff02::fb";

/// Largest datagram we read. Big enough for any mDNS answer, including jumbo
/// frames and EDNS0-extended payloads.
constexpr std::size_t MDNS_RECV_BUF_SIZE = 65536;

/// mDNS QCLASS carries the QU (unicast-response) bit (RFC 6762 §5.4).
constexpr std::uint16_t QU_BIT = 0x8000;

[[nodiscard]] const domain::Inet4Address& group_v4() {
    static const domain::Inet4Address group = domain::Inet4Address::parse(MDNS_IPV4_GROUP).value();
    return group;
}

[[nodiscard]] const domain::Inet6Address& group_v6() {
    static const domain::Inet6Address group = domain::Inet6Address::parse(MDNS_IPV6_GROUP).value();
    return group;
}

/// Pick the interface's first IPv4 address for IGMP membership and multicast
/// output. Falls back to INADDR_ANY (kernel routing table decides) when the
/// interface is empty or has no IPv4 address.
[[nodiscard]] in_addr pick_ipv4_interface_addr(const std::string& interface) {
    if (!interface.empty()) {
        if (const auto addresses = get_addresses(interface); addresses.has_value()) {
            for (const domain::InetAddress& address : *addresses) {
                if (const domain::Inet4Address* v4 = address.as_v4(); v4 != nullptr) {
                    in_addr addr{};
                    std::memcpy(&addr, v4->data(), sizeof(addr));
                    return addr;
                }
            }
        }
        SPDLOG_WARN(R"(mDNS no IPv4 address found for interface "{}", falling back to INADDR_ANY)", interface);
    }
    in_addr addr{};
    addr.s_addr = htonl(INADDR_ANY);
    return addr;
}

/// RAII leave for a joined multicast group. A move from the reference argument
/// is not needed: the guard copies the membership request.
class MembershipGuard {
public:
    MembershipGuard() = default;

    MembershipGuard(int fd, int level, int leave_option, const void* request, std::size_t size) noexcept
        : fd_(fd), level_(level), leave_option_(leave_option), size_(size) {
        std::memcpy(request_.data(), request, size);
    }

    ~MembershipGuard() noexcept {
        if (fd_ >= 0) {
            (void) net::detail::set_socket_option(fd_, level_, leave_option_, request_.data(), size_);
        }
    }

    MembershipGuard(const MembershipGuard&) = delete;
    MembershipGuard& operator=(const MembershipGuard&) = delete;

    MembershipGuard(MembershipGuard&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)), level_(other.level_), leave_option_(other.leave_option_),
          size_(other.size_), request_(other.request_) {}

    MembershipGuard& operator=(MembershipGuard&& other) noexcept {
        if (this != &other) {
            leave();
            fd_ = std::exchange(other.fd_, -1);
            level_ = other.level_;
            leave_option_ = other.leave_option_;
            size_ = other.size_;
            request_ = other.request_;
        }
        return *this;
    }

private:
    void leave() noexcept {
        if (fd_ >= 0) {
            (void) net::detail::set_socket_option(fd_, level_, leave_option_, request_.data(), size_);
        }
    }

    int fd_ = -1;
    int level_ = 0;
    int leave_option_ = 0;
    std::size_t size_ = 0;
    std::array<std::uint8_t, sizeof(ipv6_mreq)> request_{};
};

/// Best-effort option write; a failure is logged at DEBUG and never aborts the
/// lookup (matching the legacy socket configuration).
void set_int_option(const int fd, const int level, const int option, const int value, const char* name,
                    const std::string& hostname) {
    if (auto result = net::detail::set_socket_option(fd, level, option, &value, sizeof(value)); !result) {
        SPDLOG_DEBUG(R"(mDNS setsockopt {} failed for "{}")", name, hostname);
    }
}

/// Bind the socket, apply multicast options and join the group.
///
/// Mirrors the legacy socket configuration: SO_REUSEPORT for coexistence, the
/// optional SO_BINDTODEVICE, IPV6_V6ONLY, the multicast output interface and
/// TTL/hop limit, then the group join (whose failure is the one hard error).
[[nodiscard]] std::expected<void, domain::IpSourceError> configure_socket(const int fd,
                                                                          const domain::AddressFamily family,
                                                                          const std::string& interface,
                                                                          const std::string& hostname,
                                                                          MembershipGuard& guard) {
#ifdef SO_REUSEPORT
    const int enabled = 1;
    if (auto result = net::detail::set_socket_option(fd, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled));
        !result) {
        SPDLOG_DEBUG(R"(mDNS setsockopt SO_REUSEPORT failed for "{}")", hostname);
    }
#endif

    unsigned int if_index = 0;
    if (!interface.empty()) {
        if_index = ::if_nametoindex(interface.c_str());
        if (if_index == 0) {
            SPDLOG_WARN(R"(mDNS interface "{}" not found for "{}")", interface, hostname);
        }
    } else if (family == domain::AddressFamily::IPV6) {
        // A link-local multicast join needs an explicit scope: with no interface
        // configured, pick the default one (legacy behaviour). IPv4 stays on
        // INADDR_ANY and lets the kernel routing table choose.
        if (const auto default_index = get_default_interface_index(domain::AddressFamily::IPV6);
            default_index.has_value()) {
            if_index = *default_index;
            char name[IF_NAMESIZE] = {};
            const char* resolved = ::if_indextoname(if_index, name);
            SPDLOG_DEBUG(R"(mDNS auto-selected interface "{}" for "{}" (type AAAA))", resolved != nullptr ? name : "?",
                         hostname);
        }
    }

    if (!interface.empty()) {
#if defined(SO_BINDTODEVICE)
        // Best-effort on platforms without SO_BINDTODEVICE; the helper logs once.
        (void) net::detail::bind_to_interface(fd, interface);
#elif defined(IP_BOUND_IF)
        // macOS: there is no SO_BINDTODEVICE; pin the socket by interface index.
        if (if_index > 0) {
            const int level = family == domain::AddressFamily::IPV6 ? IPPROTO_IPV6 : IPPROTO_IP;
            const int option = family == domain::AddressFamily::IPV6 ? IPV6_BOUND_IF : IP_BOUND_IF;
            if (auto result = net::detail::set_socket_option(fd, level, option, &if_index, sizeof(if_index)); !result) {
                SPDLOG_DEBUG(R"(mDNS setsockopt bound-if failed for "{}")", hostname);
            }
        }
#endif
        // FreeBSD has neither SO_BINDTODEVICE nor IP_BOUND_IF; the interface
        // constraint is silently ignored there (an accepted trade-off).
    }

    if (family == domain::AddressFamily::IPV6) {
        set_int_option(fd, IPPROTO_IPV6, IPV6_V6ONLY, 1, "IPV6_V6ONLY", hostname);
        if (if_index > 0) {
            if (auto result =
                    net::detail::set_socket_option(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &if_index, sizeof(if_index));
                !result) {
                SPDLOG_DEBUG(R"(mDNS IPV6_MULTICAST_IF failed for "{}")", hostname);
            }
        }
        set_int_option(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, 255, "IPV6_MULTICAST_HOPS", hostname);

        ipv6_mreq membership{};
        std::memcpy(&membership.ipv6mr_multiaddr, group_v6().data(), sizeof(membership.ipv6mr_multiaddr));
        membership.ipv6mr_interface = if_index;
        if (auto result =
                net::detail::set_socket_option(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &membership, sizeof(membership));
            !result) {
            const int error_number = errno;
            return std::unexpected(
                domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE,
                                      fmt::format("mDNS IPV6_JOIN_GROUP failed: {}", std::strerror(error_number))});
        }
        guard = MembershipGuard{fd, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &membership, sizeof(membership)};
        return {};
    }

    if (if_index > 0) {
#if defined(__linux__)
        ip_mreqn output{};
        output.imr_ifindex = static_cast<int>(if_index);
        if (auto result = net::detail::set_socket_option(fd, IPPROTO_IP, IP_MULTICAST_IF, &output, sizeof(output));
            !result) {
            SPDLOG_DEBUG(R"(mDNS IP_MULTICAST_IF failed for "{}")", hostname);
        }
#else
        // macOS / BSD: IP_MULTICAST_IF expects the interface address, not an index.
        const in_addr output = pick_ipv4_interface_addr(interface);
        if (auto result = net::detail::set_socket_option(fd, IPPROTO_IP, IP_MULTICAST_IF, &output, sizeof(output));
            !result) {
            SPDLOG_DEBUG(R"(mDNS IP_MULTICAST_IF failed for "{}")", hostname);
        }
#endif
    }
    // Without an explicit interface the kernel routing table picks the output
    // interface, which is the legacy behaviour too.
    set_int_option(fd, IPPROTO_IP, IP_MULTICAST_TTL, 255, "IP_MULTICAST_TTL", hostname);

    ip_mreq membership{};
    std::memcpy(&membership.imr_multiaddr, group_v4().data(), sizeof(membership.imr_multiaddr));
    membership.imr_interface = pick_ipv4_interface_addr(interface);
    if (auto result =
            net::detail::set_socket_option(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership));
        !result) {
        const int error_number = errno;
        return std::unexpected(
            domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE,
                                  fmt::format("mDNS IP_ADD_MEMBERSHIP failed: {}", std::strerror(error_number))});
    }
    guard = MembershipGuard{fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &membership, sizeof(membership)};
    return {};
}

/// Send the query and collect the first matching answer. The response window is
/// the caller's timeout scope; discarded datagrams do not extend it.
[[nodiscard]] coro::Task<Result> collect(const std::string& hostname, const domain::RecordKind type,
                                         const std::string& interface, const bool ipv6) {
    const domain::AddressFamily family = ipv6 ? domain::AddressFamily::IPV6 : domain::AddressFamily::IPV4;

    auto opened = net::detail::open_udp_socket(family);
    if (!opened) {
        const int error_number = errno;
        co_return std::unexpected(
            domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE,
                                  fmt::format("mDNS socket creation failed: {}", std::strerror(error_number))});
    }
    const Utils::UniqueFd fd = std::move(*opened);

    MembershipGuard guard;
    if (auto configured = configure_socket(fd.get(), family, interface, hostname, guard); !configured) {
        co_return std::unexpected(std::move(configured.error()));
    }

    // RFC 6762 §5.1: a one-shot query MUST NOT use source port 5353, so bind an
    // ephemeral port (which also avoids clashing with avahi/systemd-resolved).
    const domain::InetAddress bind_address =
        ipv6 ? domain::InetAddress{domain::Inet6Address{}} : domain::InetAddress{domain::Inet4Address{}};
    if (auto bound = net::detail::bind_local(fd.get(), bind_address, 0); !bound) {
        const int error_number = errno;
        co_return std::unexpected(domain::IpSourceError{
            domain::IpSourceError::Code::UNAVAILABLE,
            fmt::format(R"(mDNS bind failed for "{}": {})", hostname, std::strerror(error_number))});
    }

    std::vector<std::uint8_t> query;
    try {
        query = dns::QueryBuilder{}
                    .id(0)
                    .rd(false)
                    .add_question_raw_qclass(hostname, dns::Util::type_to_record_type(type),
                                             static_cast<std::uint16_t>(dns::RecordClass::IN) | QU_BIT)
                    .build();
    } catch (const DnsPacketException& error) {
        co_return std::unexpected(domain::IpSourceError{
            domain::IpSourceError::Code::UNAVAILABLE,
            fmt::format(R"(mDNS query construction failed for "{}": {})", hostname, error.what())});
    }

    const domain::InetAddress destination = ipv6 ? domain::InetAddress{group_v6()} : domain::InetAddress{group_v4()};
    if (auto sent = co_await net::detail::send_datagram(fd.get(), destination, MDNS_PORT, query); !sent) {
        co_return std::unexpected(domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE,
                                                        fmt::format(R"(mDNS send failed for "{}")", hostname)});
    }

    std::vector<std::uint8_t> buffer(MDNS_RECV_BUF_SIZE);
    for (;;) {
        auto datagram = co_await net::detail::recv_datagram(fd.get(), buffer);
        if (!datagram) {
            co_return std::unexpected(domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE,
                                                            fmt::format(R"(mDNS receive failed for "{}")", hostname)});
        }
        if (datagram->size == 0) {
            continue;
        }
        // Responders answer from port 5353 (RFC 6762 §6); anything else is a
        // stray datagram and is dropped without ending the lookup.
        if (datagram->port != MDNS_PORT) {
            SPDLOG_TRACE(R"(mDNS discarding response from unexpected source port {} for "{}")", datagram->port,
                         hostname);
            continue;
        }

        std::vector<domain::InetAddress> results;
        try {
            results = ipsource::parse_response(std::span{buffer.data(), datagram->size}, hostname, type);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const coro::Cancelled&) {
            throw;
        } catch (const std::exception& error) {
            SPDLOG_TRACE(R"(mDNS discarding unparseable response for "{}": {})", hostname, error.what());
            continue;
        } catch (...) {
            SPDLOG_TRACE(R"(mDNS discarding response for "{}": unknown parser exception)", hostname);
            continue;
        }
        if (results.empty()) {
            SPDLOG_TRACE(R"(mDNS no matching records in response for "{}")", hostname);
            continue;
        }
        for (const auto& address : results) {
            SPDLOG_DEBUG(R"(mDNS resolved "{}" -> {})", hostname, address.to_string());
        }
        co_return results;
    }
}

}  // namespace

MdnsIpSource::MdnsIpSource(std::string hostname, domain::RecordKind type, std::string interface)
    : hostname_(std::move(hostname)), type_(type), interface_(std::move(interface)) {}

coro::Task<Result> MdnsIpSource::resolve() {
    const bool ipv6 = type_ == domain::RecordKind::AAAA;
    SPDLOG_DEBUG(R"(mDNS resolving "{}" (type {}) on interface "{}")", hostname_, ipv6 ? "AAAA" : "A",
                 interface_.empty() ? "<default>" : interface_);

    const auto scoped = co_await coro::with_timeout(MDNS_TIMEOUT, [this, ipv6]() -> coro::Task<Result> {
        co_return co_await collect(hostname_, type_, interface_, ipv6);
    });

    // The window itself is the deadline: its expiry is an ordinary "no answer"
    // failure, while an outer cancellation (shutdown) stays cancellation.
    if (scoped.timed_out) {
        co_return std::unexpected(domain::IpSourceError{
            domain::IpSourceError::Code::UNAVAILABLE,
            fmt::format(R"(mDNS no valid response for "{}" within {}ms)", hostname_, MDNS_TIMEOUT.count())});
    }

    co_return *scoped;
}

}  // namespace ipsource
