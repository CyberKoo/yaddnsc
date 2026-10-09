//
// ip_source — coroutine mDNS source.
//

#ifndef YADDNSC_INFRASTRUCTURE_IP_SOURCE_MDNS_H
#define YADDNSC_INFRASTRUCTURE_IP_SOURCE_MDNS_H

#include <string>

#include "domain/dns/record_kind.h"
#include "infrastructure/ip_source/source.h"

namespace ipsource {

/// MdnsIpSource — discover a LAN device's address via mDNS (RFC 6762).
///
/// Native coroutine implementation: the multicast socket is configured through
/// net::detail socket options, the query/response wire logic is the shared DNS
/// builder and parser, and the response window is a cancel scope
/// (`with_timeout`) rather than a blocking poll — the equivalent of the legacy
/// 500 ms deadline.
class MdnsIpSource final : public CoroIpSource {
public:
    /// @param hostname   mDNS name to query, e.g. "printer.local".
    /// @param type       RecordKind::A (IPv4 multicast) or AAAA (IPv6 multicast).
    /// @param interface  Outbound interface name; empty selects the kernel default.
    MdnsIpSource(std::string hostname, domain::RecordKind type, std::string interface);

    [[nodiscard]] coro::Task<Result> resolve() override;

private:
    std::string hostname_;
    domain::RecordKind type_;
    std::string interface_;
};

}  // namespace ipsource

#endif  // YADDNSC_INFRASTRUCTURE_IP_SOURCE_MDNS_H
