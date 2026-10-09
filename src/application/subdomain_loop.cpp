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
/// The rule: a non-positive interval never forces, and a cycle whose elapsed
/// time since the last forced cycle reaches the interval forces and latches
/// `last_force` to now.
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
    // in the past, so the elapsed time never depends on system uptime.
    coro::TimePoint last_force =
        force_interval > 0 ? context.loop->now() - std::chrono::seconds(force_interval) : coro::TimePoint{};

    for (;;) {
        if (scope != nullptr && scope->cancelled()) {
            co_return;
        }

        // The interval is anchored at the cycle start, as the legacy queue's
        // pop-and-reschedule did: a slow cycle shortens the following wait and
        // a cycle that overran its interval is followed by an immediate one,
        // so the long-run cadence does not drift by the cycle duration.
        const auto cycle_start = context.loop->now();
        const bool force = evaluate_force(force_interval, last_force, cycle_start);
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

        // The timer marks the scope cancelled too (timed_out implies
        // cancelled), so the timeout case must be filtered out first: a budget
        // timeout is a failed cycle, not a shutdown. The body already returned
        // a CANCELLED UpdateError, which next_delay treats as an ordinary
        // failure (interval, or retry_after if one was supplied).
        if (outcome.cancelled && !outcome.timed_out) {
            // The enclosing scope was cancelled (shutdown): stop without running
            // another cycle.
            co_return;
        }

        // A provider-supplied retry_after is anchored at the failure (the
        // legacy request_retry did the same); the plain interval keeps the
        // cycle-start anchoring described above.
        const auto delay = next_delay(*outcome, update_interval);
        const bool anchored_at_failure = !outcome->has_value() && outcome->error().retry_after_seconds > 0;
        coro::Duration wait = delay;
        if (!anchored_at_failure) {
            const auto elapsed = context.loop->now() - cycle_start;
            wait = delay > elapsed ? delay - elapsed : coro::Duration{};
        }

        // The sleep is the shutdown checkpoint: a cancelled wait means the
        // enclosing scope was cancelled, so exit instead of pacing the next
        // cycle (a cancelled sleep resumes immediately and would hot-spin).
        if (const auto slept = co_await coro::sleep_for(wait); !slept) {
            co_return;
        }
    }
}

}  // namespace app
