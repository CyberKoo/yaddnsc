#ifndef YADDNSC_INFRASTRUCTURE_DNS_POLICY_H
#define YADDNSC_INFRASTRUCTURE_DNS_POLICY_H

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace dns {

/// DNS scheduling and stage budgets, copied from composition into backends.
/// Durations configure local cancel scopes; no absolute deadline is forwarded.
/// Session admission belongs to the caller's scope, outside an attempt budget.
struct Policy {
    std::chrono::milliseconds attempt_budget{1500};
    std::chrono::milliseconds connect_budget{500};
    std::chrono::milliseconds wire_budget{1000};
    std::chrono::milliseconds family_collection_window{50};
    std::chrono::milliseconds retry_backoff{50};
    std::uint32_t max_retries{1};
    std::size_t max_concurrent{3};
};

}  // namespace dns

#endif  // YADDNSC_INFRASTRUCTURE_DNS_POLICY_H
