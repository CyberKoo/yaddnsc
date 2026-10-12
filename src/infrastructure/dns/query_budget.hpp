#ifndef YADDNSC_INFRASTRUCTURE_DNS_QUERY_BUDGET_HPP
#define YADDNSC_INFRASTRUCTURE_DNS_QUERY_BUDGET_HPP

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include <expected>

#include "coro/scope.hpp"
#include "coro/task.hpp"
#include "domain/error/dns_error_info.h"
#include "infrastructure/dns/policy.h"

namespace dns::detail {
/// One complete backend attempt, after session admission, including bootstrap and I/O.
/// Only this local timeout becomes an error value; ancestor cancellation throws.
template<typename Fn>
[[nodiscard]] coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> bounded_query(
    Fn fn, Policy policy = {}) {
    auto outcome = co_await coro::with_timeout(policy.attempt_budget, std::move(fn));
    if (outcome.timed_out) {
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION, "DNS backend attempt timed out"});
    }
    co_return std::move(*outcome);
}
}  // namespace dns::detail

#endif  // YADDNSC_INFRASTRUCTURE_DNS_QUERY_BUDGET_HPP
