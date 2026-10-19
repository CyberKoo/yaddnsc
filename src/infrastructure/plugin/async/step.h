#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_STEP_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_STEP_H

#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <yaddnsc/sdk/driver_abi.h>

#include "coro/offload.hpp"
#include "coro/scope.hpp"
#include "coro/task.hpp"

namespace plugin {
struct CallState;

namespace detail {
struct AsyncResources;
struct AsyncEventPacket;

/// Copies borrowed ABI error bytes on the worker before another entry runs.
struct AsyncStepOutcome {
    yaddnsc_status status = YADDNSC_STATUS_INTERNAL_ERROR;
    std::string message;
    uint32_t retry_after_seconds = 0;
    bool error_valid = false;
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
    co_return std::move(*outcome);
}

[[nodiscard]] AsyncStepOutcome make_step_outcome(yaddnsc_status status, const yaddnsc_error& error);

/// Interpret a terminal step on the loop. PENDING/invalid reports fail closed;
/// optional fqdn selects update vs validation error vocabulary. No recovery.
bool finish_async_step(CallState& state, std::string_view driver_name, const AsyncStepOutcome& outcome,
                       std::string_view entry, std::optional<std::string_view> fqdn = std::nullopt);

/// Worker-only entry adapters; the joined return hands state back to the loop.
[[nodiscard]] AsyncStepOutcome create_async_step(AsyncResources& res);
[[nodiscard]] AsyncStepOutcome begin_async_step(AsyncResources& res);
[[nodiscard]] AsyncStepOutcome resume_async_step(AsyncResources& res, std::shared_ptr<AsyncEventPacket> event);
[[nodiscard]] AsyncStepOutcome destroy_async_step(AsyncResources& res);

}  // namespace detail
}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_STEP_H
