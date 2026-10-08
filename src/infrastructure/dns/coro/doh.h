//
// dns — DNS over HTTPS (RFC 8484), POST on a persistent connection.
//
// Built on the coroutine HTTP client, so the persistent connection, its
// keep-alive policy, its AsyncMutex and its idempotent replay all come from
// http::PersistentClient instead of being re-implemented here.
//

#ifndef YADDNSC_DNS_CORO_DOH_H
#define YADDNSC_DNS_CORO_DOH_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <expected>

#include "domain/error/dns_error_info.h"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/dns/coro/resolver.h"
#include "infrastructure/net/http/persistent_client.h"

namespace dns {

/// DNS over HTTPS.
///
/// Ownership: owns one persistent HTTP client (and therefore its connection to
/// the DoH endpoint).
/// Failure: expected<raw response, DnsErrorInfo>. A non-200 response is
/// SERVER_REFUSED (4xx) or RETRY (5xx); a lost connection is retried once, which
/// is the legacy 2-attempt policy.
/// Thread safety: concurrent queries are serialized by the client's session
/// mutex, so the connection is never shared by two coroutines at once.
class DohResolver final : public Resolver {
public:
    /// @param url      DoH endpoint, e.g. "https://dns.example/dns-query".
    /// @param options  HTTP policy; its bootstrap DNS servers resolve the
    ///                 endpoint host, and its TLS options are the endpoint's.
    ///                 ALPN is forced to HTTP/1.1.
    /// @throws std::invalid_argument when `url` is not a valid https URL.
    DohResolver(std::string url, http::Options options);

    ~DohResolver();

    DohResolver(const DohResolver&) = delete;
    DohResolver& operator=(const DohResolver&) = delete;
    DohResolver(DohResolver&&) = delete;
    DohResolver& operator=(DohResolver&&) = delete;

    [[nodiscard]] coro::Task<std::expected<std::vector<std::uint8_t>, DnsErrorInfo>> query(std::string host,
                                                                                           RecordKind kind) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "DNS-Over-HTTPS"; }

    /// Drop the connection; the next query reconnects.
    void close() noexcept;

private:
    std::string target_;
    std::shared_ptr<http::PersistentClient> client_;
};

}  // namespace dns

#endif  // YADDNSC_DNS_CORO_DOH_H
