//
// app — the per-subdomain scheduling coroutine (implementation).
//

#include "subdomain_task.h"

#include <chrono>
#include <compare>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <exception>
#include <string>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

#include <expected>

#include "application/log_macros.h"
#include "application/services.h"
#include "application/tasks/update_task.h"
#include "coro/cancelled.h"
#include "coro/now.hpp"
#include "coro/scope.hpp"
#include "coro/sleep.hpp"
#include "coro/time.h"
#include "domain/config/runtime_config.h"
#include "domain/error/error.h"
#include "domain/fqdn.h"
#include "domain/update/update_task.h"

#include "min_update_interval.h"

namespace app {

namespace {

/// A failed forced cycle remains due until publishing succeeds.
[[nodiscard]] bool evaluate_force(int force_interval, coro::TimePoint last_force, coro::TimePoint now) noexcept {
    if (force_interval <= 0) {
        return false;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_force).count();
    return elapsed >= force_interval;
}

coro::Duration next_delay(const UpdateCycleOutcome& outcome, int update_interval) noexcept {
    if (!outcome.has_value() && outcome.error().retry_after_seconds > 0) {
        return std::chrono::seconds(outcome.error().retry_after_seconds);
    }
    return std::chrono::seconds(update_interval);
}

}  // namespace

coro::Task<void> subdomain_task(const domain::DomainConfig& domain, const domain::SubdomainConfig& subdomain,
                                const Services& services) {
    domain::SubdomainConfig effective = subdomain;
    domain::UpdateTask task{
        .domain = domain,
        .subdomain = effective,
        .fqdn = domain::make_fqdn(domain.name, subdomain.name),
    };

    // Configurations built without static validation still need a pacing wait.
    if (effective.update_interval <= 0) {
        YLOG_WARN(services.logger,
                  "Subdomain \"{}\" has a non-positive update interval ({}); clamping to the minimum {}s", task.fqdn,
                  effective.update_interval, YADDNSC_MIN_UPDATE_INTERVAL);
        effective.update_interval = YADDNSC_MIN_UPDATE_INTERVAL;
    }

    const int force_interval = domain.force_update;
    const int update_interval = effective.update_interval;

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
        try {
            task.force_update = evaluate_force(force_interval, last_force, cycle_start);

            const auto outcome = co_await coro::with_timeout(services.policy.update_budget,
                                                             [&task, &services]() -> coro::Task<UpdateCycleOutcome> {
                                                                 co_return co_await update_task(task, services);
                                                             });

            if (!outcome.timed_out && outcome->has_value() && task.force_update) {
                last_force = co_await coro::current_time();
            }

            // Provider retry_after starts at failure; the plain interval starts
            // at cycle_start, so a slow cycle shortens the following wait.
            const auto delay =
                outcome.timed_out ? std::chrono::seconds(update_interval) : next_delay(*outcome, update_interval);
            const bool anchored_at_failure =
                !outcome.timed_out && !outcome->has_value() && outcome->error().retry_after_seconds > 0;
            coro::Duration wait = delay;
            if (!anchored_at_failure) {
                const auto elapsed = co_await coro::current_time() - cycle_start;
                wait = delay > elapsed ? delay - elapsed : coro::Duration{};
            }

            co_await coro::sleep_for(wait);
            continue;
        } catch (const coro::Cancelled&) {
            YLOG_DEBUG(services.logger, "Subdomain task for \"{}\" stopped", task.fqdn);
            throw;
        } catch (const std::exception& error) {
            YLOG_ERROR(services.logger, "Update cycle for \"{}\" failed; retrying after the update interval: {}",
                       task.fqdn, error.what());
        } catch (...) {
            YLOG_ERROR(
                services.logger,
                "Update cycle for \"{}\" failed with a non-standard exception; retrying after the update interval",
                task.fqdn);
        }

        // Preserve the former restart policy: a defect waits a full interval
        // and makes the next cycle forced when force updates are enabled.
        last_force = force_interval > 0 ? cycle_start - std::chrono::seconds(force_interval) : coro::TimePoint{};
        co_await coro::sleep_for(std::chrono::seconds(update_interval));
    }
}

}  // namespace app
