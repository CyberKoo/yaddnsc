//
// Unit tests for the branch arms of src/infrastructure/plugin/sync/bridge.cpp
// that the gateway's happy path never reaches: ingress closure, closed-group
// rejection, and the two dispatch-failure arms of Bridge::exchange.
//
// Everything runs in memory. No plugin is dlopen'd and no socket or file is
// touched: host faults are injected with a test-local allocator that fails
// exactly one allocation, inside a window no other code runs in.
//
// The bridge no longer knows about a cycle: a call carries a BridgeSession,
// and the session answers the admission, cancellation and registration
// questions the bridge asks.
// =============================================================================

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <expected>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "coro/checkpoint.hpp"
#include "coro/clock.h"
#include "coro/fwd.h"
#include "coro/host.hpp"
#include "coro/loop.h"
#include "coro/notification.hpp"
#include "coro/offload.hpp"
#include "coro/run.hpp"
#include "coro/scope.hpp"
#include "coro/task.hpp"
#include "coro/with_nursery.hpp"
#include "domain/error/error.h"
#include "infrastructure/http/types.h"
#include "infrastructure/plugin/detail/cycle_runner.h"
#include "infrastructure/plugin/detail/driver_session.h"
#include "infrastructure/plugin/sync/bridge.h"
#include "mocks/allocation_fault.h"
#include "yaddnsc/sdk/driver_abi.h"

static_assert(noexcept(std::declval<plugin::BridgeSession&>().unregister_call(
    std::declval<const std::shared_ptr<plugin::BridgeCall>&>())));

namespace coro {
class Nursery;
}  // namespace coro

namespace {

/// Allocations left before the next one on this thread throws. -1 disarms.
thread_local int allocations_before_failure = -1;

void allocation_hook(std::size_t /*size*/) {
    if (allocations_before_failure == 0) {
        allocations_before_failure = -1;
        throw std::bad_alloc{};
    }
    if (allocations_before_failure > 0) {
        --allocations_before_failure;
    }
}

/// Arms one allocation failure on the calling thread. Keep the window as
/// narrow as the call under test — everything else in the process still
/// allocates normally.
class FailNextAllocation {
public:
    FailNextAllocation() noexcept { allocations_before_failure = 0; }

    FailNextAllocation(const FailNextAllocation&) = delete;
    FailNextAllocation& operator=(const FailNextAllocation&) = delete;

    ~FailNextAllocation() noexcept { allocations_before_failure = -1; }
};

// Arm this binary's allocation-fault hook before any test runs.
[[maybe_unused]] const bool allocation_hook_armed = [] {
    test::set_allocation_fault_hook(&allocation_hook);
    return true;
}();

}  // namespace

namespace {

/// The bridge's per-session counterpart: admission, cancellation and the
/// registration of the calls that are in flight.
class FakeBridgeSession final : public plugin::BridgeSession {
public:
    FakeBridgeSession() = default;

    explicit FakeBridgeSession(coro::NurseryToken token) : token_(std::move(token)) {}

    [[nodiscard]] coro::NurseryToken admission_token() const override { return token_; }

    [[nodiscard]] bool cancelled() const noexcept override { return cancelled_.load(std::memory_order_acquire); }

    void register_call(const std::shared_ptr<plugin::BridgeCall>& call) override {
        ++registered;
        calls.push_back(call);
    }

    void unregister_call(const std::shared_ptr<plugin::BridgeCall>& call) noexcept override {
        ++unregistered;
        for (auto it = calls.begin(); it != calls.end(); ++it) {
            if (*it == call) {
                calls.erase(it);
                break;
            }
        }
    }

    void cancel() noexcept { cancelled_.store(true, std::memory_order_release); }

    coro::NurseryToken token_{};
    std::atomic<bool> cancelled_{false};
    std::size_t registered{0};
    std::size_t unregistered{0};
    std::vector<std::shared_ptr<plugin::BridgeCall>> calls;
};

/// A driver session that completes immediately with a successful result.
class FakeDriverSession final : public plugin::DriverSession {
public:
    [[nodiscard]] coro::Task<plugin::DriverSessionOutcome> run() override {
        co_return plugin::DriverSessionOutcome{std::expected<void, domain::DriverError>{}, false};
    }

    void request_cancel() noexcept override { cancel_requested = true; }

    [[nodiscard]] coro::Task<void> drain() override { co_return; }

    bool cancel_requested{false};
};

[[nodiscard]] std::shared_ptr<plugin::BridgeCall> make_call(std::shared_ptr<plugin::BridgeSession> session = nullptr) {
    auto call = std::make_shared<plugin::BridgeCall>();
    call->session = std::move(session);
    call->url = "http://127.0.0.1:1/unreachable";
    return call;
}

constexpr std::chrono::seconds kWaitBudget{5};

coro::Task<plugin::DriverSessionResult> run_cycle(std::unique_ptr<plugin::DriverSession> session,
                                                  const coro::NurseryToken& nursery) {
    auto& scope = co_await coro::current_scope();
    plugin::CycleRunner runner{scope, std::move(session), nursery, "simple"};
    co_await runner.result_ready().wait();
    co_return runner.take_result();
}

}  // namespace

// ===========================================================================
//  cycle start — closed-group ingress
// ===========================================================================

TEST(BridgeIngress, StartCycle_AfterGroupClosed_ThrowsInsteadOfPublishing) {
    coro::Loop loop;
    coro::NurseryToken lifetime;
    bool caught = false;

    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                lifetime = coro::nursery_token(group);
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        // The group exited: its ingress token closed, so the dead group is
        // never dereferenced and the cycle is refused as a value.
        try {
            auto session = std::make_unique<FakeDriverSession>();
            [[maybe_unused]] auto result = co_await run_cycle(std::move(session), lifetime);
        } catch (const std::logic_error&) {
            caught = true;
        }
    };
    coro::run(loop, root());

    EXPECT_TRUE(caught);
}

TEST(BridgeIngress, StartCycle_SlotAllocationFailure_PropagatesAndKeepsTheGroupUsable) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool caught = false;
    bool second_cycle_published = false;

    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                const auto lifetime = coro::nursery_token(group);
                auto session = std::make_unique<FakeDriverSession>();
                {
                    // The cycle's own storage precedes the ownership transfer,
                    // so the group is left untouched and can still accept work
                    // afterwards. The failure reaches the caller instead of
                    // leaving a subscriber waiting forever.
                    const FailNextAllocation failure;
                    try {
                        [[maybe_unused]] auto result = co_await run_cycle(std::move(session), lifetime);
                    } catch (const std::bad_alloc&) {
                        caught = true;
                    }
                }
                try {
                    auto next = std::make_unique<FakeDriverSession>();
                    auto result = co_await run_cycle(std::move(next), lifetime);
                    second_cycle_published = result.has_value();
                } catch (const std::bad_alloc&) {
                    ADD_FAILURE() << "a failed spawn must not poison the group";
                }
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    coro::run(loop, root());

    EXPECT_TRUE(caught);
    EXPECT_TRUE(second_cycle_published);
}

// ===========================================================================
//  cancel() — the two no-op arms
// ===========================================================================

TEST(BridgeIngress, Cancel_NullCall_DoesNotReachTheLoop) {
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                plugin::Bridge bridge{loop, group, http::Options{}, kWaitBudget};
                bridge.cancel(nullptr);
                // Nothing was posted, so the loop simply resumes and drains the
                // group: reaching the line after the checkpoint is the proof.
                co_await coro::checkpoint();
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    coro::run(loop, root());
}

TEST(BridgeIngress, Cancel_CallWithoutLiveExchange_LatchesWithoutCancellingAScope) {
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                plugin::Bridge bridge{loop, group, http::Options{}, kWaitBudget};
                auto session = std::make_shared<FakeBridgeSession>(coro::nursery_token(group));
                auto call = make_call(session);
                // No exchange ever started, so the posted lookup finds no scope.
                bridge.cancel(call);
                co_await coro::checkpoint();
                co_await coro::checkpoint();

                EXPECT_TRUE(call->cancelled.load(std::memory_order_acquire));
                EXPECT_EQ(call->live_scope, nullptr);
                // The session itself was not cancelled: only this call was.
                EXPECT_FALSE(session->cancelled());
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    coro::run(loop, root());
}

// ===========================================================================
//  exchange() — loop-side admission
// ===========================================================================

TEST(BridgeIngress, Exchange_AlreadyCancelledCall_IsRejectedAtAdmission) {
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                plugin::Bridge bridge{loop, group, http::Options{}, kWaitBudget};
                auto session = std::make_shared<FakeBridgeSession>(coro::nursery_token(group));
                auto call = make_call(session);
                call->cancelled.store(true, std::memory_order_release);

                const auto response = co_await coro::offload_joined([&] { return bridge.exchange(call); });

                EXPECT_FALSE(response);
                EXPECT_EQ(response.error().code, YADDNSC_STATUS_CANCELLED);
                // The call was refused at admission: it never reached the
                // loop-side exchange, so it was never registered.
                EXPECT_EQ(session->registered, 0u);
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    coro::run(loop, root());
}

TEST(BridgeIngress, Exchange_AlreadyCancelledSession_IsRejectedAtAdmission) {
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                plugin::Bridge bridge{loop, group, http::Options{}, kWaitBudget};
                auto session = std::make_shared<FakeBridgeSession>(coro::nursery_token(group));
                session->cancel();
                auto call = make_call(session);

                const auto response = co_await coro::offload_joined([&] { return bridge.exchange(call); });

                EXPECT_FALSE(response);
                EXPECT_EQ(response.error().code, YADDNSC_STATUS_CANCELLED);
                EXPECT_EQ(session->registered, 0u);
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    coro::run(loop, root());
}

// ===========================================================================
//  exchange() — dispatch failure without a session
// ===========================================================================

TEST(BridgeIngress, Exchange_WithoutSession_PostFailureReportsInternalError) {
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                plugin::Bridge bridge{loop, group, http::Options{}, kWaitBudget};
                auto call = make_call();
                // call->session stays null: this is a direct Bridge user, so
                // the defect has no session to be recorded on.
                const auto response = co_await coro::offload_joined([&] {
                    const FailNextAllocation failure;
                    return bridge.exchange(call);
                });

                EXPECT_FALSE(response);
                EXPECT_EQ(response.error().code, YADDNSC_STATUS_INTERNAL_ERROR);
                EXPECT_EQ(response.error().message, "the request could not be dispatched");
                EXPECT_EQ(response.error().retry_after_seconds, 0u);
                // The defect travels with the error, so the caller's boundary
                // can report the original failure and not only a status.
                EXPECT_NE(response.error().defect, nullptr);
                if (response.error().defect) {
                    EXPECT_THROW(std::rethrow_exception(response.error().defect), std::bad_alloc);
                }
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    coro::run(loop, root());
}
