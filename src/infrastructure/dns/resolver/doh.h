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

#include "infrastructure/dns/resolver/base.h"

namespace Transport {
class Stream;
}

namespace Utils {
class CancellationToken;
}

/// DNS-over-HTTPS resolver (RFC 8484).
///
/// Owns a persistent Transport::Stream (TLS) to the DoH server and
/// performs HTTP/1.1 POST exchanges via the net::http protocol layer.
/// Cancellation is bound at construction; query() takes no token.
class DohResolver final : public ResolverBase {
public:
    /// Production constructor.
    /// @param host   DoH server hostname (SNI + certificate verification).
    /// @param port   DoH server port.
    /// @param path   HTTP path for DNS queries (e.g. "/dns-query").
    /// @param label  Display label for log / error messages.
    /// @param token  Cancellation token, bound for the resolver's lifetime.
    DohResolver(std::string host,
                std::uint16_t port,
                std::string path,
                std::string label,
                Utils::CancellationToken token);

    /// Testing constructor: inject a pre-built stream (fake or real).
    DohResolver(std::string host,
                std::uint16_t port,
                std::string path,
                std::string label,
                std::unique_ptr<Transport::Stream> stream);

    ~DohResolver() override;

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo> query(const std::string& host,
                                                                               RecordKind type) const override;

    [[nodiscard]] std::string_view get_type() const noexcept override { return "DNS-Over-HTTPS"; }

private:
    const std::uint64_t id_;
    const std::string host_;
    const std::uint16_t port_;
    const std::string path_;
    const std::string host_header_;
    const std::string label_;  // display label for log / error messages
    mutable std::mutex mutex_;
    mutable std::unique_ptr<Transport::Stream> stream_;
};

#endif  // YADDNSC_DNS_DOH_RESOLVER_H
