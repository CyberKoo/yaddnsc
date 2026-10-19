#include "mdns.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <net/if.h>
#include <spdlog/spdlog.h>

#include "coro/offload.hpp"
#include "coro/scope.hpp"
#include "coro/sleep.hpp"
#include "domain/dns/record_kind.h"
#include "domain/error/error.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/dns/dns_packet_exception.h"
#include "infrastructure/dns/types.h"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/wire/builder.h"
#include "infrastructure/ip_source/iface_util.h"
#include "infrastructure/ip_source/mdns_receive.h"
#include "infrastructure/ip_source/mdns_response.h"
#include "infrastructure/logging/level_enabled.h"
#include "infrastructure/network/transport/multicast.h"
#include "infrastructure/network/transport/udp_socket.h"
#include "support/fmt.hpp"
#include "yaddnsc/util/format.hpp"

namespace ipsource {
namespace {

constexpr std::uint16_t MDNS_PORT = 5353;
constexpr std::chrono::milliseconds MDNS_TIMEOUT{2000};
constexpr std::size_t DISCARD_BURST = 32;
constexpr std::size_t MDNS_RECV_BUF_SIZE = 65536;
constexpr std::uint16_t QU_BIT = 0x8000;

[[nodiscard]] const domain::Inet4Address& group_v4() {
    static const auto group = domain::Inet4Address::parse("224.0.0.251").value();
    return group;
}

[[nodiscard]] const domain::Inet6Address& group_v6() {
    static const auto group = domain::Inet6Address::parse("ff02::fb").value();
    return group;
}

/// Address-based IPv4 membership is needed on Linux as well as BSD. An empty
/// selection lets the kernel choose; no native socket layout escapes the adapter.
[[nodiscard]] domain::Inet4Address pick_ipv4_interface_addr(const std::string& interface) {
    if (!interface.empty()) {
        if (const auto addresses = get_addresses(interface); addresses.has_value()) {
            for (const auto& address : *addresses) {
                if (const auto* v4 = address.as_v4(); v4 != nullptr) {
                    return *v4;
                }
            }
        }
        SPDLOG_WARN(R"(mDNS no IPv4 address found for interface "{}", falling back to INADDR_ANY)", interface);
    }
    return {};
}

/// Owned metadata only; this function runs on a worker, never with a socket.
[[nodiscard]] net::MulticastInterface read_interface_metadata(const std::string& interface,
                                                              domain::AddressFamily family,
                                                              const std::string& hostname) {
    unsigned int index = 0;
    if (!interface.empty()) {
        index = ::if_nametoindex(interface.c_str());
        if (index == 0) {
            SPDLOG_WARN(R"(mDNS interface "{}" not found for "{}")", interface, hostname);
        }
    } else if (family == domain::AddressFamily::IPV6) {
        if (const auto selected = get_default_interface_index(domain::AddressFamily::IPV6); selected) {
            index = *selected;
            char name[IF_NAMESIZE] = {};
            const char* resolved = ::if_indextoname(index, name);
            SPDLOG_DEBUG(R"(mDNS auto-selected interface "{}" for "{}" (type AAAA))", resolved != nullptr ? name : "?",
                         hostname);
        }
    }
    return {index,
            family == domain::AddressFamily::IPV4 ? pick_ipv4_interface_addr(interface) : domain::Inet4Address{}};
}

[[nodiscard]] coro::Task<Result> collect(const std::string& hostname, const domain::RecordKind type,
                                         const std::string& interface, const bool ipv6) {
    const auto family = ipv6 ? domain::AddressFamily::IPV6 : domain::AddressFamily::IPV4;
    net::MulticastInterface metadata;
    if (!interface.empty() || ipv6) {
        metadata = co_await coro::offload(
            [interface, family, hostname] { return read_interface_metadata(interface, family, hostname); });
    }
    // Default IPv4 requires neither metadata nor worker admission.
    net::UdpSocket socket{family};
    if (auto opened = socket.open(); !opened) {
        co_return std::unexpected(
            domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE, "mDNS socket creation failed"});
    }
    const domain::InetAddress destination = ipv6 ? domain::InetAddress{group_v6()} : domain::InetAddress{group_v4()};
    // RFC 6762: link-local multicast uses hop limit 255. Native option shapes
    // and the corresponding leave operation belong to the network adapter, and
    // the group is joined before the local address is chosen.
    const auto membership = net::configure_multicast(socket.native_handle(), {destination, interface, metadata, 255});
    if (!membership) {
        co_return std::unexpected(
            domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE,
                                  fmt::format("mDNS {} failed ({})", ipv6 ? "IPV6_JOIN_GROUP" : "IP_ADD_MEMBERSHIP",
                                              membership.error().message())});
    }
    // RFC 6762 §5.1: one-shot queries use an ephemeral source port, not 5353.
    const domain::InetAddress bind_address =
        ipv6 ? domain::InetAddress{domain::Inet6Address{}} : domain::InetAddress{domain::Inet4Address{}};
    if (auto bound = socket.bind(bind_address, 0); !bound) {
        co_return std::unexpected(domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE,
                                                        fmt::format(R"(mDNS bind for "{}" failed)", hostname)});
    }
    std::vector<std::uint8_t> query;
    try {
        query = dns::QueryBuilder{0}
                    .rd(false)
                    .add_question_raw_qclass(hostname, dns::Util::type_to_record_type(type),
                                             static_cast<std::uint16_t>(dns::RecordClass::IN) | QU_BIT)
                    .build();
    } catch (const DnsPacketException& error) {
        co_return std::unexpected(domain::IpSourceError{
            domain::IpSourceError::Code::UNAVAILABLE,
            fmt::format(R"(mDNS query construction for "{}" failed ({}))", hostname, error.what())});
    }
    if (auto sent = co_await socket.send_to(destination, MDNS_PORT, query); !sent) {
        co_return std::unexpected(domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE,
                                                        fmt::format(R"(mDNS send failed for "{}")", hostname)});
    }
    co_return co_await detail::receive_mdns(socket, hostname, type);
}

}  // namespace

namespace detail {

coro::Task<Result> receive_mdns(net::UdpSocket& socket, const std::string& hostname, const domain::RecordKind type) {
    std::vector<std::uint8_t> buffer(MDNS_RECV_BUF_SIZE);
    std::size_t discarded = 0;
    for (;;) {
        if (discarded == DISCARD_BURST) {
            discarded = 0;
            co_await coro::sleep_for(std::chrono::milliseconds{1});
        }
        auto datagram = co_await socket.recv_from(buffer);
        if (!datagram) {
            co_return std::unexpected(domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE,
                                                            fmt::format(R"(mDNS receive failed for "{}")", hostname)});
        }
        ++discarded;
        if (datagram->size == 0) {
            continue;
        }
        if (datagram->port != MDNS_PORT) {
            SPDLOG_TRACE(R"(mDNS discarding response from unexpected source port {} for "{}")", datagram->port,
                         hostname);
            continue;
        }
        auto parsed = ipsource::parse_response(std::span{buffer.data(), datagram->size}, hostname, type);
        if (!parsed) {
            SPDLOG_TRACE(R"(mDNS discarding unparseable response for "{}": {})", hostname, parsed.error().message);
            continue;
        }
        auto& results = *parsed;
        if (results.empty()) {
            SPDLOG_TRACE(R"(mDNS no matching records in response for "{}")", hostname);
            continue;
        }
        for ([[maybe_unused]] const auto& address : results) {
            if (logging::level_enabled(spdlog::level::trace)) {
                SPDLOG_TRACE(R"(mDNS resolved "{}" -> {})", hostname, address.to_string());
            }
        }
        co_return std::move(results);
    }
}

}  // namespace detail

MdnsIpSource::MdnsIpSource(std::string hostname, domain::RecordKind type, std::string interface)
    : hostname_(std::move(hostname)), type_(type), interface_(std::move(interface)) {}

coro::Task<Result> MdnsIpSource::resolve() {
    const bool ipv6 = type_ == domain::RecordKind::AAAA;
    SPDLOG_DEBUG(R"(mDNS resolving "{}" (type {}) on interface "{}")", hostname_, ipv6 ? "AAAA" : "A",
                 interface_.empty() ? "<default>" : interface_);
    const auto scoped = co_await coro::with_timeout(MDNS_TIMEOUT, [this, ipv6]() -> coro::Task<Result> {
        co_return co_await collect(hostname_, type_, interface_, ipv6);
    });
    if (scoped.timed_out) {
        co_return std::unexpected(domain::IpSourceError{
            domain::IpSourceError::Code::UNAVAILABLE,
            fmt::format(R"(mDNS no valid response for "{}" within {}ms)", hostname_, MDNS_TIMEOUT.count())});
    }
    co_return *scoped;
}

}  // namespace ipsource
