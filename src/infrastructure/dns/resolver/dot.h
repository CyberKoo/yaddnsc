//
// dns — DNS over TLS (RFC 7858), one persistent connection per resolver.
//
// The connection is reused across queries and serialized by an AsyncMutex: a
// concurrent query waits under its caller's cancellation scope. Whole-query retries belong
// to the dispatcher; failed sessions are discarded before another attempt.
//
// Queries carry the EDNS(0) padding option (RFC 7830) sized to the 128-octet
// query block of RFC 8467 §4.1, and are framed with the two-byte length
// prefix. The ALPN identifier "dot" is offered.
//

#ifndef YADDNSC_INFRASTRUCTURE_DNS_RESOLVER_DOT_H
#define YADDNSC_INFRASTRUCTURE_DNS_RESOLVER_DOT_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <expected>

#include "coro/async_mutex.hpp"
#include "coro/task.hpp"
#include "domain/config/dns_config.h"
#include "domain/error/dns_error_info.h"
#include "infrastructure/dns/policy.h"
#include "infrastructure/dns/resolver/resolver.h"
#include "infrastructure/network/address/host_resolver.h"
#include "infrastructure/network/transport/options.h"

namespace net {
class Stream;
class StreamFactory;
class TlsContext;
}  // namespace net

namespace dns {

/// Transport settings for a TLS-based resolver endpoint.
struct EndpointOptions {
    /// Local budgets for admitted queries and bootstrap scheduling.
    Policy policy{};
    /// Outbound interface binding.
    net::ConnectOptions connect{};
    /// TLS policy; the resolver fills in ALPN and defaults the SNI identity to
    /// a named endpoint host (never an IP literal, RFC 6066 §3). Trust policy
    /// (verification, CA bundle) belongs to the context below.
    net::TlsOptions tls{};
    /// Pre-built TLS trust context (off-loop, TlsContext::create). Null fails the
    /// TLS connection closed.
    std::shared_ptr<const net::TlsContext> tls_context{};
    /// Bootstrap DNS servers used to resolve the endpoint host.
    std::vector<domain::DnsServer> bootstrap_dns{};
    /// Hostname lookup override; empty resolves through bootstrap_dns.
    /// Tests inject here.
    net::HostResolver resolve{};
    /// Stream source; null selects the production factory. Tests inject here.
    std::shared_ptr<net::StreamFactory> factory{};
};

/// DNS over TLS.
///
/// Ownership: owns one persistent TLS stream (rebuilt on loss) and the mutex
/// that serializes it.
/// Failure: expected<raw response, DnsErrorInfo>. Admission waits under the
/// caller scope. The admitted attempt bounds bootstrap and I/O; per-address
/// connect scopes use the injected policy (defaults: 1.5s and 500ms).
/// Internal defects propagate; cancellation releases the session. Endpoint bootstrap errors become CONNECTION, never a
/// verdict on the queried domain. Malformed/TC replies become PARSE and allow failover. Whole-query retries belong to
/// Dispatcher. A lost reused connection may be repaired once within the same attempt budget. Thread safety: concurrent
/// queries are serialized by the session mutex; the resolver itself is used from the loop thread only.
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

    /// Drop the connection; defer safely while a query holds the session guard.
    /// The next query reconnects. Call on the loop thread only.
    void close() noexcept;

private:
    [[nodiscard]] coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> query_once(
        std::string host, domain::RecordKind kind);
    void drop_stream() noexcept;

    [[nodiscard]] coro::Task<std::expected<void, domain::DnsErrorInfo>> ensure_stream();

    std::string host_;
    std::uint16_t port_{853};
    EndpointOptions options_;
    coro::AsyncMutex mutex_;
    std::unique_ptr<net::Stream> stream_;
    bool close_requested_{false};
};

}  // namespace dns

#endif  // YADDNSC_INFRASTRUCTURE_DNS_RESOLVER_DOT_H
