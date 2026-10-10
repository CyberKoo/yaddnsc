//
// dns — DNS over TLS (RFC 7858), one persistent connection per resolver.
//
// The connection is reused across queries and serialized by an AsyncMutex: a
// concurrent query waits for the connection instead of being refused. A lost
// connection is rebuilt once, which is the legacy 2-attempt policy expressed as
// two awaits instead of two threads.
//
// Queries are padded to a 128-octet block (RFC 7830 / RFC 7858 §3.5) and framed
// with the two-byte length prefix, and the ALPN identifier "dot" is offered.
//

#ifndef YADDNSC_INFRASTRUCTURE_DNS_RESOLVER_DOT_H
#define YADDNSC_INFRASTRUCTURE_DNS_RESOLVER_DOT_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <expected>

#include "domain/config/dns_config.h"
#include "domain/error/dns_error_info.h"
#include "infrastructure/coro/async_mutex.hpp"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/dns/resolver/resolver.h"
#include "infrastructure/network/transport/options.h"

namespace net {
class Stream;
class StreamFactory;
class TlsContext;
}  // namespace net

namespace dns {

/// Transport settings for a TLS-based resolver endpoint.
struct EndpointOptions {
    /// Outbound interface binding.
    net::ConnectOptions connect{};
    /// TLS policy; the resolver fills in ALPN and defaults the SNI identity to
    /// a named endpoint host (never an IP literal, RFC 6066 §3).
    /// `verify_peer` defaults to true.
    net::TlsOptions tls{};
    /// Pre-built TLS trust context (off-loop, TlsContext::create). Null fails the
    /// TLS connection closed.
    std::shared_ptr<const net::TlsContext> tls_context{};
    /// Bootstrap DNS servers used to resolve the endpoint host.
    std::vector<domain::DnsServer> bootstrap_dns{};
    /// Stream source; null selects the production factory. Tests inject here.
    std::shared_ptr<net::StreamFactory> factory{};
};

/// DNS over TLS.
///
/// Ownership: owns one persistent TLS stream (rebuilt on loss) and the mutex
/// that serializes it.
/// Failure: expected<raw response, DnsErrorInfo>; a lost connection is retried
/// once, and everything else is reported as is.
/// Thread safety: concurrent queries are serialized by the session mutex; the
/// resolver itself is used from the loop thread only.
class DotResolver final : public Resolver {
public:
    /// @param host  Endpoint host (IP literal or a name resolved via bootstrap).
    /// @param port  Endpoint port (853 by convention).
    /// @param options  Transport/TLS policy for the endpoint.
    DotResolver(std::string host, std::uint16_t port, EndpointOptions options);

    ~DotResolver();

    DotResolver(const DotResolver&) = delete;
    DotResolver& operator=(const DotResolver&) = delete;
    DotResolver(DotResolver&&) = delete;
    DotResolver& operator=(DotResolver&&) = delete;

    [[nodiscard]] coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> query(
        std::string host, domain::RecordKind kind) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "DNS-Over-TLS"; }

    /// Drop the connection; the next query reconnects.
    void close() noexcept;

private:
    [[nodiscard]] coro::Task<std::expected<void, domain::DnsErrorInfo>> ensure_stream();

    std::string host_;
    std::uint16_t port_{853};
    EndpointOptions options_;
    coro::AsyncMutex mutex_;
    std::unique_ptr<net::Stream> stream_;
};

}  // namespace dns

#endif  // YADDNSC_INFRASTRUCTURE_DNS_RESOLVER_DOT_H
