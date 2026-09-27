//
// Created by Kotarou on 2026/6/28.
//

#ifndef YADDNSC_DNS_DOH_RESOLVER_H
#define YADDNSC_DNS_DOH_RESOLVER_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "domain/config/dns_config.h"
#include "infrastructure/dns/resolver/base.h"

namespace Transport {
class Stream;
}

namespace Utils {
class CancellationToken;
}

/// DohEndpoint — the DoH server target, bundled so the resolver constructors
/// stay within the ≤4-parameter rule.  Groups the members that are always
/// used together: the HTTPS origin (SNI + certificate verification host),
/// port, HTTP query path, and the display label for log / error messages.
struct DohEndpoint {
    std::string host;
    std::uint16_t port;
    std::string path;
    std::string label;
};

/// DNS-over-HTTPS resolver (RFC 8484).
///
/// Owns a persistent Transport::Stream (TLS) to the DoH server and
/// performs HTTP/1.1 POST exchanges via the net::http protocol layer.
/// Cancellation flows through query() as a parameter; nothing is bound
/// at construction.
class DohResolver final : public ResolverBase {
public:
    /// Production constructor.
    /// @param endpoint   DoH server target (host, port, query path, label).
    /// @param bootstrap  Bootstrap DNS servers used to resolve `endpoint.host`
    ///                   when it is a hostname (empty: hostname targets fail fast).
    explicit DohResolver(DohEndpoint endpoint, std::vector<Config::DnsServer> bootstrap = {});

    /// Testing constructor: inject a pre-built stream (fake or real).
    DohResolver(DohEndpoint endpoint, std::unique_ptr<Transport::Stream> stream);

    ~DohResolver() override;

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo> query(
        const std::string& host, RecordKind type, const Utils::CancellationToken& token) const override;

    [[nodiscard]] std::string_view get_type() const noexcept override { return "DNS-Over-HTTPS"; }

private:
    const std::uint64_t id_;
    const std::string host_;
    const std::uint16_t port_;
    const std::string path_;
    const std::string host_header_;
    const std::string label_;  // display label for log / error messages
    const std::vector<Config::DnsServer> bootstrap_;
    mutable std::mutex mutex_;
    mutable std::unique_ptr<Transport::Stream> stream_;
};

#endif  // YADDNSC_DNS_DOH_RESOLVER_H
