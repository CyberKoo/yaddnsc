//
// dns — classic resolver: UDP query with a TCP retry when the answer is truncated.
//

#ifndef YADDNSC_DNS_CLASSIC_H
#define YADDNSC_DNS_CLASSIC_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <expected>

#include "domain/error/dns_error_info.h"
#include "domain/network/inet_address.h"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/dns/resolver.h"

namespace dns {

/// Classic DNS over UDP, with the RFC 1035 §4.2.2 TCP retry on truncation.
///
/// Ownership: the server address is copied at construction; a socket is opened
/// per query, so a resolver holds no connection state and needs no teardown.
/// Failure: expected<raw response, DnsErrorInfo>.
class ClassicResolver final : public Resolver {
public:
    ClassicResolver(InetAddress server, std::uint16_t port) noexcept : server_(server), port_(port) {}

    [[nodiscard]] coro::Task<std::expected<std::vector<std::uint8_t>, DnsErrorInfo>> query(std::string host,
                                                                                           RecordKind kind) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "Classic"; }

    [[nodiscard]] const InetAddress& server() const noexcept { return server_; }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    InetAddress server_;
    std::uint16_t port_{53};
};

}  // namespace dns

#endif  // YADDNSC_DNS_CLASSIC_H
