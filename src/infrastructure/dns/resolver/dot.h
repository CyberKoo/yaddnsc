//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_DNS_DOT_H
#define YADDNSC_DNS_DOT_H

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

/// DotResolver — DNS-over-TLS (RFC 7858) resolver on Transport.
///
/// Owns a persistent TLS stream to the DoT server; DNS messages are framed
/// with a 2-byte big-endian length prefix (RFC 7858 §3.3).  Cancellation
/// flows through query() as a parameter; nothing is bound at construction.
///
/// Thread-safe: query() acquires an internal mutex around the persistent
/// stream.  Distinct DotResolver objects are independent.
class DotResolver final : public ResolverBase {
public:
    /// Construct with server address and optional port.
    /// @param server  Server hostname or IP address.
    /// @param port    TLS port (default: 853).
    /// @param label   Display label (e.g. "dot.pub:853"), used in log/error messages.
    /// @param bootstrap  Bootstrap DNS servers used to resolve `server` when
    ///                   it is a hostname (empty: hostname targets fail fast).
    explicit DotResolver(std::string server, std::uint16_t port, std::string label,
                         std::vector<Config::DnsServer> bootstrap = {});

    /// Testing constructor: inject a pre-built stream (fake or real).
    DotResolver(std::string server, std::uint16_t port, std::string label, std::unique_ptr<Transport::Stream> stream);

    ~DotResolver() override;

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
    query(const std::string& host, RecordKind type, const Utils::CancellationToken& token) const override;

    [[nodiscard]] std::string_view get_type() const noexcept override { return TYPE; }

private:
    const std::uint64_t id_;
    const std::string server_;
    const std::uint16_t port_;
    const std::string label_;  // display label for log / error messages
    const std::vector<Config::DnsServer> bootstrap_;
    mutable std::mutex mutex_;
    mutable std::unique_ptr<Transport::Stream> stream_;
    static constexpr std::string_view TYPE = "DNS-Over-TLS";
};

#endif  // YADDNSC_DNS_DOT_H
