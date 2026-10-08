//
// app — the per-subdomain scheduling coroutine (implementation).
//

#include "subdomain_loop.h"

#include <chrono>
#include <memory>
#include <utility>

#include "application/services.h"
#include "domain/fqdn.h"
#include "domain/update/update_task.h"
#include "infrastructure/coro/group.hpp"
#include "infrastructure/coro/scope.hpp"
#include "infrastructure/coro/sleep.hpp"

namespace app {

namespace {

/// Evaluate and latch the force-update flag for one cycle.
///
/// Byte-for-byte the ScheduleQueue::check_force_update rule: a non-positive
/// interval never forces, and a cycle whose elapsed time since the last forced
/// cycle reaches the interval forces and latches `last_force` to now.
[[nodiscard]] bool evaluate_force(int force_interval, coro::TimePoint& last_force, coro::TimePoint now) noexcept {
    if (force_interval <= 0) {
        return false;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_force).count();
    if (elapsed >= force_interval) {
        last_force = now;
        return true;
    }
    return false;
}

}  // namespace

coro::Duration next_delay(const UpdateOnceOutcome& outcome, int update_interval) noexcept {
    if (!outcome.has_value() && outcome.error().retry_after_seconds > 0) {
        return std::chrono::seconds(outcome.error().retry_after_seconds);
    }
    return std::chrono::seconds(update_interval);
}

coro::Task<void> subdomain_loop(std::shared_ptr<const domain::RuntimeConfig> config, std::size_t domain_index,
                                std::size_t subdomain_index, const Services& services) {
    const auto context = co_await coro::GetContext{};
    const auto& domain = config->domains[domain_index];
    const auto& subdomain = domain.subdomains[subdomain_index];
    const std::string fqdn = domain::make_fqdn(domain.name, subdomain.name);
    const int force_interval = domain.force_update;
    const int update_interval = subdomain.update_interval;
    coro::CancelScope* const scope = context.scope;

    // First cycle forced when the interval is positive: start one full interval
    // in the past (ScheduleQueue's constructor), so the elapsed time never
    // depends on system uptime.
    coro::TimePoint last_force =
        force_interval > 0 ? context.loop->now() - std::chrono::seconds(force_interval) : coro::TimePoint{};

    for (;;) {
        if (scope != nullptr && scope->cancelled()) {
            co_return;
        }

        const bool force = evaluate_force(force_interval, last_force, context.loop->now());
        const domain::UpdateTask task{
            .config = config,
            .domain_index = domain_index,
            .subdomain_index = subdomain_index,
            .fqdn = fqdn,
            .force_update = force,
        };

        const auto outcome = co_await coro::with_timeout(
            UPDATE_BUDGET, [&task, &services](coro::CancelScope&) -> coro::Task<UpdateOnceOutcome> {
                co_return co_await update_once(task, services);
            });

        if (outcome.cancelled) {
            // The enclosing scope was cancelled (shutdown): stop without running
            // another cycle.
            co_return;
        }

        // A budget timeout is a failed cycle, not a shutdown: the body already
        // returned a CANCELLED UpdateError, which next_delay treats as an
        // ordinary failure (interval, or retry_after if one was supplied).
        co_await coro::sleep_for(next_delay(*outcome, update_interval));
    }
}

}  // namespace app
