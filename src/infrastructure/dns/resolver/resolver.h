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
#include <string>
#include <string_view>
#include <vector>

#include <expected>

#include "coro/task.hpp"
#include "domain/dns/record_kind.h"
#include "domain/error/dns_error_info.h"

namespace dns {

/// One DNS backend.
///
/// Failure: recoverable errors use expected<raw response, DnsErrorInfo>.
/// Cancellation throws `coro::Cancelled`. Only INVALID_QUERY ends a group
/// search; PARSE, CONFIG and negative answers describe this backend. RETRY,
/// CONNECTION may be retried against the same backend. UNKNOWN does not justify retrying the same backend.
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
    /// The task owns its arguments and borrows this resolver until completion.
    [[nodiscard]] virtual coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> query(
        std::string host, domain::RecordKind kind) = 0;

    /// Human-readable backend name, for diagnostics.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// Stable numeric id used in diagnostics (`Resolver #N` log lines).
    /// Assigned by the Dispatcher when it takes ownership of the backend.
    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

    /// Assign the diagnostic id. Called once by the Dispatcher; not for other
    /// callers.
    void set_id(const std::uint64_t id) noexcept { id_ = id; }

private:
    std::uint64_t id_ = 0;
};

namespace detail {

/// True only for INVALID_QUERY, the single error that stops the search for
/// this name: every other code describes this backend and allows searching other backends.
[[nodiscard]] constexpr bool is_definitive(const domain::DnsError code) noexcept {
    return code == domain::DnsError::INVALID_QUERY;
}

/// True when a retry of the same query could succeed.
[[nodiscard]] constexpr bool is_retryable(const domain::DnsError code) noexcept {
    return code == domain::DnsError::RETRY || code == domain::DnsError::CONNECTION;
}

}  // namespace detail

}  // namespace dns

#endif  // YADDNSC_INFRASTRUCTURE_DNS_RESOLVER_RESOLVER_H
