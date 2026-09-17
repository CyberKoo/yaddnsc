//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_DNS_DOT_H
#define YADDNSC_DNS_DOT_H

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "support/exception.h"
#include "domain/error/dns_error_info.h"
#include "infrastructure/dns/resolver/base.h"

namespace Transport {
class Stream;
}

namespace Utils {
class CancellationToken;
}

/// DotResolver — DNS-over-TLS (RFC 7858) resolver on Transport.
///
/// Owns a persistent TLS stream to the DoT server; DNS messages are framed
/// with a 2-byte big-endian length prefix (RFC 7858 §3.3).  Cancellation is
/// bound at construction; query() takes no token.
///
/// Thread-safe: query() acquires an internal mutex around the persistent
/// stream.  Distinct DotResolver objects are independent.
class DotResolver final : public ResolverBase {
public:
    /// Construct with server address and optional port.
    /// @param server  Server hostname or IP address.
    /// @param port    TLS port (default: 853).
    /// @param label   Display label (e.g. "dot.pub:853"), used in log/error messages.
    /// @param token   Cancellation token, bound for the resolver's lifetime.
    explicit DotResolver(std::string server, std::uint16_t port, std::string label,
                         Utils::CancellationToken token);

    /// Testing constructor: inject a pre-built stream (fake or real).
    DotResolver(std::string server, std::uint16_t port, std::string label,
                std::unique_ptr<Transport::Stream> stream);

    ~DotResolver() override;

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
    query(const std::string &host, RecordKind type) const override;

    [[nodiscard]] std::string_view get_type() const noexcept override { return TYPE; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    static constexpr std::string_view TYPE = "DNS-Over-TLS";
};

#endif // YADDNSC_DNS_DOT_H
