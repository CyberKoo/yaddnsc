//
// app — the per-subdomain scheduling coroutine (implementation).
//

#include "subdomain_loop.h"

#include <chrono>
#include <compare>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <expected>
#include <string>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

#include "domain/fqdn.h"
#include "domain/update/update_task.h"
#include "coro/now.hpp"
#include "coro/scope.hpp"
#include "coro/sleep.hpp"
#include "application/run_update_cycle.h"
#include "domain/config/runtime_config.h"
#include "domain/error/error.h"
#include "coro/time.h"

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

coro::Duration next_delay(const UpdateCycleOutcome& outcome, int update_interval) noexcept {
    if (!outcome.has_value() && outcome.error().retry_after_seconds > 0) {
        return std::chrono::seconds(outcome.error().retry_after_seconds);
    }
    return std::chrono::seconds(update_interval);
}

}  // namespace

coro::Task<void> subdomain_loop(const domain::DomainConfig& domain, const domain::SubdomainConfig& subdomain,
                                const Services& services) {
    domain::UpdateTask task{
        .domain = domain,
        .subdomain = subdomain,
        .fqdn = domain::make_fqdn(domain.name, subdomain.name),
    };
    const int force_interval = domain.force_update;
    const int update_interval = subdomain.update_interval;

    // First cycle forced when the interval is positive: start one full interval
    // in the past, so the elapsed time never depends on system uptime.
    coro::TimePoint last_force =
        force_interval > 0 ? co_await coro::current_time() - std::chrono::seconds(force_interval) : coro::TimePoint{};

    for (;;) {
        // The interval is anchored at the cycle start, as the legacy queue's
        // pop-and-reschedule did: a slow cycle shortens the following wait and
        // a cycle that overran its interval is followed by an immediate one,
        // so the long-run cadence does not drift by the cycle duration.
        const auto cycle_start = co_await coro::current_time();
        task.force_update = evaluate_force(force_interval, last_force, cycle_start);

        const auto outcome = co_await coro::with_timeout(
            UPDATE_BUDGET,
            [&task, &services]() -> coro::Task<UpdateCycleOutcome> { co_return co_await run_update_cycle(task, services); });

        // Own budget expiry retries on the interval; shutdown propagates.

        // A provider-supplied retry_after is anchored at the failure (the
        // legacy request_retry did the same); the plain interval keeps the
        // cycle-start anchoring described above.
        const auto delay =
            outcome.timed_out ? std::chrono::seconds(update_interval) : next_delay(*outcome, update_interval);
        const bool anchored_at_failure =
            !outcome.timed_out && !outcome->has_value() && outcome->error().retry_after_seconds > 0;
        coro::Duration wait = delay;
        if (!anchored_at_failure) {
            const auto elapsed = co_await coro::current_time() - cycle_start;
            wait = delay > elapsed ? delay - elapsed : coro::Duration{};
        }

        // The sleep is the shutdown checkpoint: a cancelled wait means the
        // enclosing scope was cancelled, so exit instead of pacing the next
        // cycle (a cancelled sleep resumes immediately and would hot-spin).
        co_await coro::sleep_for(wait);
    }
}

}  // namespace app
