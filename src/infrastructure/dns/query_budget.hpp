#ifndef YADDNSC_INFRASTRUCTURE_DNS_QUERY_BUDGET_HPP
#define YADDNSC_INFRASTRUCTURE_DNS_QUERY_BUDGET_HPP

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include <expected>

#include "coro/cancel_scope.h"
#include "coro/scope.hpp"
#include "coro/task.hpp"
#include "domain/error/dns_error_info.h"

namespace dns::detail {
/// Two default attempts plus 50ms backoff fit the application's 4s DNS budget.
inline constexpr auto ATTEMPT_BUDGET = std::chrono::milliseconds(1500);
/// A black-holed first address leaves time for subsequent addresses and I/O.
inline constexpr auto CONNECT_BUDGET = std::chrono::milliseconds(500);
inline constexpr auto SESSION_WAIT_BUDGET = std::chrono::milliseconds(500);
/// One wire exchange — a UDP receive loop or a TCP stream — inside an attempt.
/// Stray datagrams do not restart it; a UDP-to-TCP fallback may see less of it.
inline constexpr auto WIRE_BUDGET = std::chrono::milliseconds(1000);

/// One complete backend attempt, including queueing, bootstrap and I/O.
/// Only this local timeout becomes an error value; ancestor cancellation throws.
template<typename Fn>
[[nodiscard]] coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> bounded_query(Fn fn) {
    auto outcome = co_await coro::with_timeout(ATTEMPT_BUDGET, std::move(fn));
    if (outcome.timed_out) {
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION, "DNS backend attempt timed out"});
    }
    if (!outcome.has_value()) {
        // with_timeout absorbs only a cancellation it owns: its own expiry is
        // timed_out above, and an ancestor cancel is rethrown to this caller,
        // so a value-less outcome means the body cancelled the scope it was
        // running in. Re-raise it on the caller's scope — cancellation is
        // control flow and must not become a DNS error value.
        const coro::CancelScope& scope = co_await coro::current_scope();
        if (scope.cancelled())
            scope.throw_if_cancelled();
        // Nothing in the caller's scope chain is cancelled, so there is no
        // cancellation left to propagate and the attempt simply has no result.
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION,
                                                       "DNS backend attempt was cancelled before producing a result"});
    }
    co_return std::move(*outcome);
}
}  // namespace dns::detail

#endif  // YADDNSC_INFRASTRUCTURE_DNS_QUERY_BUDGET_HPP
