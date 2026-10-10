//
// dns — resolver backends for the coroutine DNS subsystem.
//
// A resolver answers one query. Classic, DoT and DoH implement this interface;
// the dispatcher composes several of them with a strategy, and the bootstrap
// resolver uses the classic wire exchange directly.
//

#ifndef YADDNSC_INFRASTRUCTURE_DNS_RESOLVER_RESOLVER_H
#define YADDNSC_INFRASTRUCTURE_DNS_RESOLVER_RESOLVER_H

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <expected>

#include "domain/dns/record_kind.h"
#include "domain/error/dns_error_info.h"
#include "coro/task.hpp"
#include "infrastructure/dns/types.h"

namespace dns {

/// One DNS backend.
///
/// Failure: recoverable errors use expected<raw response, DnsErrorInfo>.
/// Cancellation throws `coro::Cancelled`; PARSE and CONFIG
/// are definitive (they must not be downgraded to a retryable error); RETRY,
/// CONNECTION, SERVER_REFUSED, NODATA and UNKNOWN are transient.
/// Thread safety: a resolver may hold mutable session state (a persistent
/// connection), so query() is not const; it is not thread-safe and is used from
/// the loop thread only.
class Resolver {
public:
    Resolver() = default;
    Resolver(const Resolver&) = delete;
    Resolver& operator=(const Resolver&) = delete;
    virtual ~Resolver() = default;

    /// Query `host` for `kind` and return the raw response message.
    [[nodiscard]] virtual coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> query(
        std::string host, domain::RecordKind kind) = 0;

    /// Human-readable backend name, for diagnostics.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// Stable numeric id used in diagnostics (the legacy `Resolver #N` logs).
    /// Assigned by the Dispatcher when it takes ownership of the backend.
    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

    /// Assign the diagnostic id. Called once by the Dispatcher; not for other
    /// callers.
    void set_id(const std::uint64_t id) noexcept { id_ = id; }

private:
    std::uint64_t id_ = 0;
};

namespace detail {


/// True when the response header has the TC (truncated) bit set.
[[nodiscard]] inline bool is_truncated(const std::span<const std::uint8_t> response) noexcept {
    return response.size() >= dns::HEADER_SIZE && (response[2] & 0x02) != 0;
}

/// True when an error is permanent for this name and must stop the search.
[[nodiscard]] constexpr bool is_definitive(const domain::DnsError code) noexcept {
    return code == domain::DnsError::PARSE || code == domain::DnsError::CONFIG;
}

/// True when a retry of the same query could succeed.
[[nodiscard]] constexpr bool is_retryable(const domain::DnsError code) noexcept {
    return code == domain::DnsError::RETRY || code == domain::DnsError::UNKNOWN || code == domain::DnsError::CONNECTION;
}

}  // namespace detail

}  // namespace dns

#endif  // YADDNSC_INFRASTRUCTURE_DNS_RESOLVER_RESOLVER_H
