//
// dns — DNS over HTTPS (RFC 8484), POST on a persistent connection.
//
// Built on the coroutine HTTP client, so the persistent connection, its
// keep-alive policy, its AsyncMutex and its idempotent replay all come from
// http::PersistentClient instead of being re-implemented here.
//

#ifndef YADDNSC_INFRASTRUCTURE_DNS_RESOLVER_DOH_H
#define YADDNSC_INFRASTRUCTURE_DNS_RESOLVER_DOH_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "coro/async_mutex.hpp"
#include "infrastructure/dns/policy.h"
#include "infrastructure/dns/resolver/resolver.h"

namespace http {
class PersistentClient;
struct Options;
}  // namespace http

namespace dns {

/// DNS over HTTPS.
///
/// Ownership: owns one persistent HTTP client (and therefore its connection to
/// the DoH endpoint).
/// Failure: expected<raw response, DnsErrorInfo>. Non-2xx is SERVER_REFUSED,
/// except 429/5xx (RETRY). Invalid remote responses are PARSE and permit
/// failover, not same-backend retry. Whole-query retries belong to Dispatcher;
/// a lost reused session may be repaired/replayed once within the same attempt.
/// Admission waits under the caller scope, outside the attempt. The injected
/// policy defaults to a 1.5s attempt and a 500ms per-address connect budget.
/// Internal defects propagate; cancellation releases the session. Body size is capped at
/// 16KiB even for direct construction. Redirects are rejected as a local policy.
/// Thread safety: concurrent queries are serialized by the client's session
/// mutex, so the connection is never shared by two coroutines at once.
class DohResolver final : public Resolver {
public:
    /// @param url      DoH endpoint, e.g. "https://dns.example/dns-query".
    /// @param options  HTTP policy; its injected resolver resolves the endpoint
    ///                 host, and its TLS options are the endpoint's.
    ///                 ALPN is forced to HTTP/1.1.
    /// @param policy   Local budgets for admitted queries.
    /// @throws std::invalid_argument when `url` is not a valid https URL.
    DohResolver(std::string url, http::Options options, Policy policy = {});

    ~DohResolver();

    DohResolver(const DohResolver&) = delete;
    DohResolver& operator=(const DohResolver&) = delete;
    DohResolver(DohResolver&&) = delete;
    DohResolver& operator=(DohResolver&&) = delete;

    [[nodiscard]] coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> query(
        std::string host, domain::RecordKind kind) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "DNS-Over-HTTPS"; }

    /// Drop the connection; the next query reconnects.
    void close() noexcept;

private:
    [[nodiscard]] coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> query_once(
        std::string host, domain::RecordKind kind);

    std::string target_;
    std::unique_ptr<http::PersistentClient> client_;
    Policy policy_;
    coro::AsyncMutex mutex_;
};

}  // namespace dns

#endif  // YADDNSC_INFRASTRUCTURE_DNS_RESOLVER_DOH_H
