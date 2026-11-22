/// Contract tests for the async destroy completion policy.
///
/// The destroy step is the last use of a plugin instance. Releasing the module
/// and the services while the instance may still be alive would leave the
/// plugin running against freed borrows, so a destroy step that produced no
/// result cannot be degraded to a cancellation — it enters the fatal cleanup
/// policy. The policy is a plain function over a real ScopeOutcome, so these
/// tests drive it directly: no plugin, and no production test hook.

#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <yaddnsc/sdk/driver_abi.h>

#include "coro/run.hpp"
#include "coro/scope_outcome.hpp"
#include "infrastructure/plugin/async/step.h"

namespace {

using plugin::detail::AsyncStepOutcome;

constexpr std::string_view DRIVER_NAME = "duckdns";
constexpr std::string_view DESTROY_PHASE = "async destroy step did not complete";

/// A scope outcome whose body produced a result, as a completed destroy step
/// does.
coro::ScopeOutcome<AsyncStepOutcome> produced_outcome(const yaddnsc_status status) {
    coro::ScopeOutcome<AsyncStepOutcome> outcome;
    outcome.value.emplace(AsyncStepOutcome{.status = status, .message = "destroyed"});
    return outcome;
}

TEST(AsyncDestroyStep, MissingResult_EntersTheFatalCleanupPolicy) {
    // The fatal policy reports through the logging backend on stdout, while
    // GoogleTest matches the child's stderr: the empty matcher asserts the
    // death itself.
    EXPECT_DEATH(
        {
            [[maybe_unused]] const auto outcome = plugin::detail::enforce_destroy_step_result(
                coro::ScopeOutcome<AsyncStepOutcome>{}, DRIVER_NAME, DESTROY_PHASE);
        },
        "");
}

TEST(AsyncDestroyStep, ProducedResult_ReturnsTheOutcomeWithoutAborting) {
    const auto outcome =
        plugin::detail::enforce_destroy_step_result(produced_outcome(YADDNSC_STATUS_OK), DRIVER_NAME, DESTROY_PHASE);
    EXPECT_EQ(outcome.status, YADDNSC_STATUS_OK);
    EXPECT_EQ(outcome.message, "destroyed");
}

TEST(AsyncDestroyStep, DestroyStep_ReturnsThePluginResult) {
    const auto outcome = coro::run(plugin::detail::run_async_destroy_step(DRIVER_NAME, DESTROY_PHASE, [] {
        return AsyncStepOutcome{.status = YADDNSC_STATUS_OK, .message = "destroyed"};
    }));
    EXPECT_EQ(outcome.status, YADDNSC_STATUS_OK);
    EXPECT_EQ(outcome.message, "destroyed");
}

}  // namespace
