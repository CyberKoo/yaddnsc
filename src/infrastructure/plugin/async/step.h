#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_STEP_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_STEP_H

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <yaddnsc/sdk/driver_abi.h>

#include "coro/offload.hpp"
#include "coro/scope.hpp"
#include "coro/scope_outcome.hpp"
#include "coro/task.hpp"

namespace plugin::detail {

/// Owned result of one joined ABI step. Borrowed plugin error views have been
/// copied before the worker returns to the loop.
struct AsyncStepOutcome {
    yaddnsc_status status{YADDNSC_STATUS_INTERNAL_ERROR};
    std::string message;
    std::uint32_t retry_after_seconds{0};
    bool error_valid{false};
};

/// A plugin entry cannot be abandoned while it borrows its context. Shield the
/// joined offload only; protocol cancellation is processed between steps.
template<typename Fn>
coro::Task<AsyncStepOutcome> run_async_step(Fn fn) {
    auto outcome = co_await coro::non_cancellable(
        [fn = std::move(fn)](coro::CancelScope&) mutable -> coro::Task<AsyncStepOutcome> {
            co_return co_await coro::offload_joined(std::move(fn));
        });
    assert(outcome.has_value() && "shielded async step produced no value");
    if (!outcome.has_value()) {
        co_return AsyncStepOutcome{.status = YADDNSC_STATUS_CANCELLED,
                                   .message = "async step was cancelled before it produced a result"};
    }
    co_return std::move(*outcome);
}

/// The destroy step must yield a result.
///
/// The destroy step is the last use of a plugin instance. Without a result the
/// host cannot prove that the instance was destroyed, and releasing the module
/// and the services would then leave the plugin running against freed borrows.
/// A missing result is therefore a fatal cleanup failure, not a cancellation
/// the caller may degrade and continue from.
///
/// `driver` names the plugin and `phase` describes the step that could not
/// prove its completion; both reach the fatal policy's diagnostic.
[[nodiscard]] AsyncStepOutcome enforce_destroy_step_result(coro::ScopeOutcome<AsyncStepOutcome> outcome,
                                                           std::string_view driver, std::string_view phase) noexcept;

/// Run the destroy step with the same joined, shielded offload as
/// `run_async_step`, but with a non-degradable completion contract: a missing
/// result or a throwing step enters the fatal cleanup policy, so the caller
/// only ever observes a confirmed destroy result.
template<typename Fn>
coro::Task<AsyncStepOutcome> run_async_destroy_step(std::string_view driver, std::string_view phase, Fn fn) {
    coro::ScopeOutcome<AsyncStepOutcome> outcome{};
    try {
        outcome = co_await coro::non_cancellable(
            [fn = std::move(fn)](coro::CancelScope&) mutable -> coro::Task<AsyncStepOutcome> {
                co_return co_await coro::offload_joined(std::move(fn));
            });
    } catch (...) {
        // A step that threw produced no result either. The shield keeps
        // ancestor cancellation out of this scope, so this arm observes
        // defects only — and a destroy step without a result leaves the
        // instance alive, which the policy, not the caller, must answer.
        outcome = coro::ScopeOutcome<AsyncStepOutcome>{};
    }
    co_return enforce_destroy_step_result(std::move(outcome), driver, phase);
}

[[nodiscard]] AsyncStepOutcome make_step_outcome(yaddnsc_status status, const yaddnsc_error& error);

}  // namespace plugin::detail

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_STEP_H
