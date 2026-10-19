//
// Coroutine runtime — Task composition, structured scopes and completion-order
// consumption.
//
// Test tasks are named functions taking explicit parameters. An
// immediately-invoked capturing coroutine lambda (`g.spawn([&]{...}())`) would
// leave the child frame referencing a closure temporary that dies at the end of
// the full expression — a language-level pitfall of coroutine lambdas, not a
// runtime property.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <chrono>
#include <coroutine>
#include <exception>
#include <expected>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "coro/coro.h"
#include "coro/detail/nursery_join.hpp"
#include "coro/detail/wait_operation.h"

namespace coro::detail {
struct FdEntry;
struct NurseryState;
template<typename T>
struct ChildEntry;
struct PromiseBase;
struct TimerNode;
}  // namespace coro::detail

namespace {

using namespace std::chrono_literals;

template<typename T>
concept PublicFrameRelease = requires(T& task) { task.release(); };

template<typename T>
concept PublicContextBinding =
    requires(T& task, coro::Loop& loop, coro::CancelScope& scope) { task.bind_context(loop, scope); };

template<typename T>
concept PublicScopeParent = requires(T& scope) { scope.parent(); };

// The loop's scheduling/registration services and the scope's waiter
// bookkeeping are private: reachable only through detail::LoopAccess and
// detail::ScopeAccess.
template<typename T>
concept PublicLoopScheduling = requires(T& loop, coro::detail::PromiseBase& frame) { loop.schedule(frame); };

template<typename T>
concept PublicLoopRegistration = requires(T& loop, coro::detail::TimerNode& timer, coro::detail::FdEntry& entry) {
    loop.add_timer(timer, coro::TimePoint{}, nullptr, nullptr);
    loop.remove_timer(timer);
    loop.add_fd(entry);
    loop.remove_fd(entry);
};

template<typename T>
concept PublicLoopPool = requires(T& loop) { loop.offload_pool(); };

template<typename T>
concept PublicScopeWaiter = requires(T& scope, coro::detail::CancelSubscription& node) { scope.add_subscriber(node); };

template<typename T>
concept PublicScopeCause = requires(T& scope) {
    scope.cancel(coro::detail::CancelCause::TIMEOUT);
    scope.absorbs(std::declval<const coro::Cancelled&>());
};

coro::Task<int> constant(int value) {
    co_return value;
}

coro::Task<int> add_one(int value) {
    co_return value + 1;
}

coro::Task<void> throw_runtime_error(std::string message) {
    throw std::runtime_error(message);
    co_return;
}

coro::Task<int> fail_immediately(std::string message) {
    throw std::runtime_error(message);
    co_return 0;
}

coro::Task<int> sleep_then_value(std::chrono::milliseconds delay, int value) {
    co_await coro::sleep_for(delay);
    co_return value;
}

coro::Task<void> increment_after_sleep(int& counter, std::chrono::milliseconds delay) {
    co_await coro::sleep_for(delay);
    ++counter;
    co_return;
}

coro::Task<int> set_flag_after_sleep(bool& flag, std::chrono::milliseconds delay, int value) {
    co_await coro::sleep_for(delay);
    flag = true;
    co_return value;
}

coro::Task<void> wait_until_cancelled(bool& cancelled) {
    try {
        co_await coro::sleep_for(1000ms);
    } catch (const coro::Cancelled&) {
        cancelled = true;
        throw;
    }
    co_return;
}

coro::Task<void> cancel_after_sleep(coro::Nursery& group, std::chrono::milliseconds delay) {
    co_await coro::sleep_for(delay);
    group.cancel();
    co_return;
}

coro::Task<void> fail_after_sleep(std::chrono::milliseconds delay, const char* message) {
    co_await coro::sleep_for(delay);
    throw std::runtime_error(message);
    co_return;
}

coro::Task<void> tick_until_cancelled(bool& started, bool& cancelled, int& ticks) {
    started = true;
    for (;;) {
        try {
            co_await coro::sleep_for(50ms);
        } catch (const coro::Cancelled&) {
            cancelled = true;
            throw;
        }
        ++ticks;
    }
}

// ---------------------------------------------------------------------------
// run / inline co_await
// ---------------------------------------------------------------------------

TEST(Task, PublicApi_FrameAndContextAccess_IsUnavailable) {
    EXPECT_FALSE(PublicFrameRelease<coro::Task<int>>);
    EXPECT_FALSE(PublicFrameRelease<coro::Task<void>>);
    EXPECT_FALSE(PublicContextBinding<coro::Task<int>>);
    EXPECT_FALSE(PublicContextBinding<coro::Task<void>>);
    using Frame = std::coroutine_handle<coro::Task<int>::promise_type>;
    EXPECT_FALSE((std::is_constructible_v<coro::Task<int>, Frame>) );
    EXPECT_FALSE((std::is_constructible_v<coro::Nursery, coro::detail::NurseryState*>) );
    EXPECT_FALSE((std::is_constructible_v<coro::Handle<int>, coro::detail::ChildEntry<int>*>) );
    EXPECT_FALSE(PublicScopeParent<coro::CancelScope>);
}

TEST(Task, PublicApi_RuntimeRegistrationServices_AreUnavailable) {
    EXPECT_FALSE(PublicLoopScheduling<coro::Loop>);
    EXPECT_FALSE(PublicLoopRegistration<coro::Loop>);
    EXPECT_FALSE(PublicLoopPool<coro::Loop>);
    EXPECT_FALSE(PublicScopeWaiter<coro::CancelScope>);
    EXPECT_FALSE(PublicScopeCause<coro::CancelScope>);
    // The operations a caller performs on a scope stay public.
    EXPECT_TRUE((requires(coro::CancelScope& scope) {
        scope.cancel();
        scope.cancelled();
        scope.timed_out();
        scope.throw_if_cancelled();
    }));
}

TEST(Task, run_AwaitedChain_ReturnsValue) {
    auto task = []() -> coro::Task<int> {
        const int first = co_await constant(41);
        const int second = co_await add_one(first);
        co_return first + second;
    };
    EXPECT_EQ(coro::run(task()), 83);
}

TEST(Task, run_TaskThrows_RethrowsAtAwait) {
    auto task = []() -> coro::Task<void> {
        co_await throw_runtime_error("inner boom");
        co_return;
    };
    try {
        coro::run(task());
        FAIL() << "expected the defect to propagate out of coro::run";
    } catch (const std::runtime_error& error) {
        EXPECT_STREQ(error.what(), "inner boom");
    }
}

TEST(Task, run_RootTask_RunsOnCallingThread) {
    const std::thread::id caller = std::this_thread::get_id();
    std::thread::id observed;
    auto task = [&observed]() -> coro::Task<void> {
        observed = std::this_thread::get_id();
        co_return;
    };
    coro::run(task());
    EXPECT_EQ(observed, caller);
}

// ---------------------------------------------------------------------------
// spawn + Handle join
// ---------------------------------------------------------------------------

TEST(Nursery, spawn_ThenJoin_YieldsEveryValue) {
    int total = 0;
    auto task = [&total]() -> coro::Task<void> {
        co_await coro::with_nursery([&total](coro::Nursery& group) -> coro::Task<void> {
            auto first = group.spawn(constant(20));
            auto second = group.spawn(constant(22));
            total = (co_await first) + (co_await second);
            co_return;
        });
        co_return;
    };
    coro::run(task());
    EXPECT_EQ(total, 42);
}

TEST(Nursery, spawn_AwaitHandleOfThrowingChild_RethrowsAtJoin) {
    bool caught = false;
    auto task = [&caught]() -> coro::Task<void> {
        // Handle consumption observes the defect. A supervisor nursery must not
        // report that same failure again as an unobserved exit error.
        co_await coro::with_nursery(
            [&caught](coro::Nursery& group) -> coro::Task<void> {
                auto handle = group.spawn(throw_runtime_error("child boom"));
                try {
                    co_await handle;
                } catch (const std::runtime_error& error) {
                    caught = std::string(error.what()) == "child boom";
                }
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    };
    coro::run(task());
    EXPECT_TRUE(caught);
}

// ---------------------------------------------------------------------------
// FAIL_FAST nursery: drain all; first failure cancels siblings then propagates
// ---------------------------------------------------------------------------

TEST(Nursery, NormalExit_DrainsEveryChild) {
    int completed = 0;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&completed]() -> coro::Task<void> {
        co_await coro::with_nursery([&completed](coro::Nursery& group) -> coro::Task<void> {
            for (int i = 0; i < 4; ++i) {
                group.spawn(increment_after_sleep(completed, 10ms));
            }
            co_return;
        });
        co_return;
    };
    coro::run(loop, task());
    EXPECT_EQ(completed, 4);
}

TEST(Nursery, FirstChildThrows_CancelsSiblingsThenPropagates) {
    bool sibling_started = false;
    bool sibling_saw_cancellation = false;
    int sibling_ticks = 0;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&sibling_started, &sibling_saw_cancellation, &sibling_ticks]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&sibling_started, &sibling_saw_cancellation, &sibling_ticks](coro::Nursery& group) -> coro::Task<void> {
                group.spawn(fail_after_sleep(10ms, "sibling failed"));
                group.spawn(tick_until_cancelled(sibling_started, sibling_saw_cancellation, sibling_ticks));
                co_return;
            });
        co_return;
    };
    try {
        coro::run(loop, task());
        FAIL() << "the first child failure must propagate";
    } catch (const std::runtime_error& error) {
        EXPECT_STREQ(error.what(), "sibling failed");
    }
    EXPECT_TRUE(sibling_started);
    EXPECT_TRUE(sibling_saw_cancellation);
    EXPECT_EQ(sibling_ticks, 0);
}

// ---------------------------------------------------------------------------
// SUPERVISOR nursery: a child failure is reported, siblings are untouched
// ---------------------------------------------------------------------------

TEST(Nursery, SupervisorChildThrows_ReportsAndLeavesSiblings) {
    bool saw_error = false;
    bool saw_value = false;
    bool sibling_finished = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&saw_error, &saw_value, &sibling_finished]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&saw_error, &saw_value, &sibling_finished](coro::Nursery& group) -> coro::Task<void> {
                group.spawn(fail_immediately("supervised failure"));
                group.spawn(set_flag_after_sleep(sibling_finished, 20ms, 7));
                while (auto outcome = co_await group.next<int>()) {
                    if (outcome->has_value()) {
                        saw_value = outcome->value() == 7;
                    } else {
                        saw_error = true;
                    }
                }
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    };
    coro::run(loop, task());
    EXPECT_TRUE(saw_error);
    EXPECT_TRUE(saw_value);
    EXPECT_TRUE(sibling_finished);
}

// ---------------------------------------------------------------------------
// next(): completion-order consumption
// ---------------------------------------------------------------------------

TEST(Nursery, next_ChildrenFinishOutOfOrder_YieldsCompletionOrder) {
    std::vector<int> order;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&order]() -> coro::Task<void> {
        co_await coro::with_nursery([&order](coro::Nursery& group) -> coro::Task<void> {
            group.spawn(sleep_then_value(30ms, 1));
            group.spawn(sleep_then_value(10ms, 2));
            group.spawn(sleep_then_value(20ms, 3));
            while (auto outcome = co_await group.next<int>()) {
                if (outcome->has_value()) {
                    order.push_back(outcome->value());
                }
            }
            co_return;
        });
        co_return;
    };
    coro::run(loop, task());
    EXPECT_EQ(order, (std::vector<int>{2, 3, 1}));
}

TEST(Nursery, next_ChildThrows_ReportsDefectAsValue) {
    bool saw_error = false;
    auto task = [&saw_error]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&saw_error](coro::Nursery& group) -> coro::Task<void> {
                group.spawn(throw_runtime_error("next reports this"));
                while (auto outcome = co_await group.next<void>()) {
                    saw_error = !outcome->has_value();
                }
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    };
    coro::run(task());
    EXPECT_TRUE(saw_error);
}

// ---------------------------------------------------------------------------
// explicit group cancellation
// ---------------------------------------------------------------------------

TEST(Nursery, cancel_Requested_StopsChildren) {
    bool child_cancelled = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&child_cancelled]() -> coro::Task<void> {
        co_await coro::with_nursery([&child_cancelled](coro::Nursery& group) -> coro::Task<void> {
            group.spawn(wait_until_cancelled(child_cancelled));
            group.spawn(cancel_after_sleep(group, 5ms));
            co_return;
        });
        co_return;
    };
    coro::run(loop, task());
    EXPECT_TRUE(child_cancelled);
}

// ---------------------------------------------------------------------------
// spawn_discard: fire-and-forget children with bounded bookkeeping
// ---------------------------------------------------------------------------

TEST(Nursery, spawn_discard_ScopeExitJoinsAChildStillInFlight) {
    int completed = 0;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&completed]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&completed](coro::Nursery& group) -> coro::Task<void> {
                group.spawn_discard(increment_after_sleep(completed, 20ms));
                // The body leaves at once; the group must still join the child
                // instead of reaping its live frame.
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    };
    coro::run(loop, task());
    EXPECT_EQ(completed, 1);
}

TEST(Nursery, spawn_discard_IsNeverDeliveredThroughNext) {
    int discard_completed = 0;
    std::vector<int> delivered;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&discard_completed, &delivered]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&discard_completed, &delivered](coro::Nursery& group) -> coro::Task<void> {
                // The discard child finishes last, so the parked next() loop is
                // woken by a completion it must not observe.
                group.spawn(sleep_then_value(10ms, 7));
                group.spawn_discard(increment_after_sleep(discard_completed, 30ms));
                while (auto outcome = co_await group.next<int>()) {
                    if (outcome->has_value()) {
                        delivered.push_back(outcome->value());
                    }
                }
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    };
    coro::run(loop, task());
    EXPECT_EQ(delivered, (std::vector<int>{7}));
    EXPECT_EQ(discard_completed, 1);
}

TEST(Nursery, spawn_discard_DefectCancelsSiblingsAndPropagates) {
    // A discard child's defect is a child defect like any other: the nursery
    // cancels the siblings and rethrows it at scope exit.
    bool sibling_cancelled = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&sibling_cancelled]() -> coro::Task<void> {
        co_await coro::with_nursery([&sibling_cancelled](coro::Nursery& group) -> coro::Task<void> {
            group.spawn_discard(fail_after_sleep(10ms, "discard boom"));
            group.spawn(wait_until_cancelled(sibling_cancelled));
            co_return;
        });
        co_return;
    };
    try {
        coro::run(loop, task());
        FAIL() << "a discard child's defect must propagate";
    } catch (const std::runtime_error& error) {
        EXPECT_STREQ(error.what(), "discard boom");
    }
    EXPECT_TRUE(sibling_cancelled);
}

TEST(Nursery, spawn_discard_ManyChildren_AllRunAndAreJoined) {
    // Churn through many discard children in one group: every completion
    // retires the previous one's frame, which the sanitizer build validates.
    int completed = 0;
    auto task = [&completed]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&completed](coro::Nursery& group) -> coro::Task<void> {
                for (int i = 0; i < 500; ++i) {
                    group.spawn_discard(increment_after_sleep(completed, 0ms));
                }
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    };
    coro::run(task());
    EXPECT_EQ(completed, 500);
}

struct ThrowingMove {
    std::shared_ptr<int> lifetime;
    bool* moved;

    ThrowingMove(std::shared_ptr<int> token, bool& flag) : lifetime(std::move(token)), moved(&flag) {}

    // Intentionally throws to verify exception-safe result retirement.
    // NOLINTNEXTLINE(performance-noexcept-move-constructor)
    ThrowingMove(ThrowingMove&& other) : moved(other.moved) {
        if (std::exchange(*moved, true)) {
            throw std::runtime_error("result move failed");
        }
        lifetime = std::move(other.lifetime);
    }
};

coro::Task<ThrowingMove> throwing_move_result(std::shared_ptr<int> token, bool& moved) {
    co_return ThrowingMove{std::move(token), moved};
}

TEST(Task, run_ResultMoveThrows_ReleasesRootFrame) {
    auto token = std::make_shared<int>(1);
    bool moved = false;
    EXPECT_THROW({ [[maybe_unused]] auto result = coro::run(throwing_move_result(token, moved)); }, std::runtime_error);
    EXPECT_EQ(token.use_count(), 1);
}

TEST(Task, await_ResultMoveThrows_ReleasesChildFrame) {
    auto token = std::make_shared<int>(1);
    bool moved = false;
    auto parent = [&]() -> coro::Task<void> {
        EXPECT_THROW(co_await throwing_move_result(token, moved), std::runtime_error);
        EXPECT_EQ(token.use_count(), 1);
    };
    coro::run(parent());
    EXPECT_EQ(token.use_count(), 1);
}

TEST(Nursery, next_NoMatchingChildren_ReturnsWithoutWaitingForOtherTypes) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool cancelled = false;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
            group.spawn(wait_until_cancelled(cancelled));
            EXPECT_FALSE((co_await group.next<int>()).has_value());
            group.cancel();
        });
    };
    coro::run(loop, root());
}

TEST(Nursery, next_LastMatchingChildConsumed_ReturnsWhileOtherTypesRun) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool cancelled = false;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
            group.spawn(wait_until_cancelled(cancelled));
            group.spawn(constant(42));
            auto result = co_await group.next<int>();
            EXPECT_TRUE(result.has_value());
            EXPECT_FALSE((co_await group.next<int>()).has_value());
            group.cancel();
        });
    };
    coro::run(loop, root());
}

TEST(Nursery, join_ResultClaimedOnce_AcrossCopiesAndNext) {
    auto root = []() -> coro::Task<void> {
        co_await coro::with_nursery([](coro::Nursery& group) -> coro::Task<void> {
            auto handle = group.spawn(constant(42));
            auto copy = handle;
            EXPECT_EQ(co_await handle, 42);
            EXPECT_FALSE(copy.valid());
            EXPECT_THROW(co_await copy, std::logic_error);
            EXPECT_FALSE((co_await group.next<int>()).has_value());
        });
    };
    coro::run(root());
}

TEST(Nursery, join_NextConsumedResult_RejectsJoin) {
    auto root = []() -> coro::Task<void> {
        co_await coro::with_nursery([](coro::Nursery& group) -> coro::Task<void> {
            auto handle = group.spawn(constant(42));
            EXPECT_TRUE((co_await group.next<int>()).has_value());
            EXPECT_THROW(co_await handle, std::logic_error);
            coro::Handle<int> empty;
            EXPECT_THROW(co_await empty, std::logic_error);
        });
    };
    coro::run(root());
}

TEST(Nursery, join_HandleOutlivingGroup_FailsSafely) {
    coro::Handle<int> smuggled;
    auto root = [&smuggled]() -> coro::Task<void> {
        co_await coro::with_nursery([&smuggled](coro::Nursery& group) -> coro::Task<void> {
            smuggled = group.spawn(constant(42));
            co_return;
        });
        co_return;
    };
    coro::run(root());
    // The group is destroyed; the handle must read as dead, not dangle.
    EXPECT_FALSE(smuggled.valid());

    auto join_stale = [&smuggled]() -> coro::Task<void> {
        EXPECT_THROW(co_await smuggled, std::logic_error);
        co_return;
    };
    coro::run(join_stale());
}

TEST(Task, run_DispatchThrows_TerminatesInsteadOfUnwindingParkedFrames) {
    EXPECT_DEATH(
        {
            coro::Loop loop;
            loop.post([] { throw std::runtime_error("dispatch failed"); });
            [[maybe_unused]] auto result = coro::run(loop, constant(1));
        },
        "");
}

coro::Task<void> hold_parameter(std::shared_ptr<int> token) {
    EXPECT_NE(token, nullptr);
    co_await coro::checkpoint();
}

TEST(Nursery, next_LocalTimeout_DetachesWaitWithoutCancellingOrClaimingChild) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto root = []() -> coro::Task<void> {
        co_await coro::with_nursery(
            [](coro::Nursery& group) -> coro::Task<void> {
                auto handle = group.spawn(sleep_then_value(100ms, 42));
                const auto start = co_await coro::current_time();
                for (int i = 0; i < 3; ++i) {
                    auto outcome = co_await coro::with_timeout(10ms, [&group]() -> coro::Task<void> {
                        [[maybe_unused]] auto result = co_await group.next<int>();
                        ADD_FAILURE() << "cancelled result wait must not consume";
                    });
                    EXPECT_TRUE(outcome.timed_out);
                    EXPECT_TRUE(handle.valid());
                    EXPECT_FALSE(group.scope().cancelled());
                }
                EXPECT_EQ((co_await coro::current_time()) - start, 30ms);
                auto result = co_await group.next<int>();
                EXPECT_EQ(result->value(), 42);
                EXPECT_FALSE(handle.valid());
                EXPECT_FALSE((co_await group.next<int>()).has_value());
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    coro::run(loop, root());
}

TEST(Nursery, next_AlreadyCancelledCaller_DoesNotClaimReadyResult) {
    auto root = []() -> coro::Task<void> {
        co_await coro::with_nursery(
            [](coro::Nursery& group) -> coro::Task<void> {
                auto handle = group.spawn(constant(42));
                co_await coro::checkpoint();
                co_await coro::checkpoint();
                auto outcome = co_await coro::with_cancel_scope([&group](coro::CancelScope& scope) -> coro::Task<void> {
                    scope.cancel();
                    [[maybe_unused]] auto result = co_await group.next<int>();
                });
                EXPECT_TRUE(outcome.cancelled);
                EXPECT_TRUE(handle.valid());
                EXPECT_EQ(co_await handle, 42);
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    coro::run(root());
}

TEST(Nursery, next_CompletionAndTimeoutSameDeadline_CancellationWinsWithoutClaim) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto root = []() -> coro::Task<void> {
        co_await coro::with_nursery(
            [](coro::Nursery& group) -> coro::Task<void> {
                auto handle = group.spawn(sleep_then_value(10ms, 42));
                auto outcome = co_await coro::with_timeout(10ms, [&group]() -> coro::Task<void> {
                    [[maybe_unused]] auto result = co_await group.next<int>();
                    ADD_FAILURE();
                });
                EXPECT_TRUE(outcome.timed_out);
                EXPECT_EQ(co_await handle, 42);
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    coro::run(loop, root());
}

TEST(Nursery, Consumption_ReleasesFrameAndBoundsHistoryWhileGroupLives) {
    auto root = []() -> coro::Task<void> {
        co_await coro::with_nursery([](coro::Nursery& group) -> coro::Task<void> {
            for (int i = 0; i < 1000; ++i) {
                auto token = std::make_shared<int>(1);
                auto handle = group.spawn(hold_parameter(token));
                if (i % 2 == 0) {
                    co_await handle;
                } else {
                    const auto result = co_await group.next<void>();
                    EXPECT_TRUE(result.has_value() && result->has_value());
                }
                EXPECT_EQ(token.use_count(), 1);
                EXPECT_FALSE(handle.valid());
            }
            group.spawn(constant(42));
            const auto result = co_await group.next<int>();
            EXPECT_TRUE(result.has_value());
            if (result) {
                EXPECT_EQ(result->value(), 42);
            }
        });
    };
    coro::run(root());
}

TEST(Nursery, Consumption_ThrowingMoveStillReleasesFrame) {
    auto root = []() -> coro::Task<void> {
        co_await coro::with_nursery(
            [](coro::Nursery& group) -> coro::Task<void> {
                auto token = std::make_shared<int>(1);
                bool moved = false;
                auto handle = group.spawn(throwing_move_result(token, moved));
                EXPECT_THROW(co_await handle, std::runtime_error);
                EXPECT_EQ(token.use_count(), 1);
                moved = false;
                group.spawn(throwing_move_result(token, moved));
                EXPECT_THROW(co_await group.next<ThrowingMove>(), std::runtime_error);
                EXPECT_EQ(token.use_count(), 1);
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    coro::run(root());
}

TEST(Nursery, Supervisor_UnobservedFailuresAreReportedAfterSiblingsFinish) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    int finished = 0;
    auto root = [&finished]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&finished](coro::Nursery& group) -> coro::Task<void> {
                group.spawn_discard(throw_runtime_error("discard"));
                group.spawn(throw_runtime_error("unclaimed"));
                group.spawn(increment_after_sleep(finished, 10ms));
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    try {
        coro::run(loop, root());
        FAIL();
    } catch (const coro::GroupError& error) {
        EXPECT_EQ(error.errors().size(), 2u);
    }
    EXPECT_EQ(finished, 1);
}

TEST(Nursery, RunnerResources_RemainAliveThroughCancellationDrain) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto root = []() -> coro::Task<void> {
        auto token = std::make_shared<int>(1);
        co_await coro::with_nursery(token,
                                    [](coro::Nursery& nursery, std::shared_ptr<int>& resource) -> coro::Task<void> {
                                        nursery.spawn(hold_parameter(resource));
                                        nursery.cancel();
                                        co_return;
                                    });
        EXPECT_EQ(token.use_count(), 1);
    };
    coro::run(loop, root());
}

coro::Task<void> borrow_until_cancelled(const std::weak_ptr<int>& token, bool& finished) {
    coro::Notification never_notified;
    try {
        co_await never_notified.wait();
    } catch (const coro::Cancelled&) {
        EXPECT_FALSE(token.expired());
        finished = true;
        throw;
    }
}

TEST(Nursery, NestedNursery_DefectDrainsBeforeOuterOwnerDestructs) {
    bool finished = false;
    auto root = [&]() -> coro::Task<void> {
        auto resource = std::make_shared<int>(1);
        const std::weak_ptr<int> borrow = resource;
        EXPECT_THROW(co_await coro::with_nursery(
                         [&](coro::Nursery& nursery) -> coro::Task<void> {
                             nursery.spawn(borrow_until_cancelled(borrow, finished));
                             co_await coro::checkpoint();
                             throw std::runtime_error("body failure");
                         },
                         coro::NurseryPolicy::SUPERVISOR),
                     std::runtime_error);
        EXPECT_TRUE(finished);
        EXPECT_FALSE(borrow.expired());
    };
    coro::run(root());
    EXPECT_TRUE(finished);
}

TEST(Nursery, BodyFactoryThrowsAfterSpawning_StillCancelsAndDrains) {
    bool cancelled = false;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
            group.spawn(wait_until_cancelled(cancelled));
            // Deliberately not a coroutine: failure happens while constructing
            // the body, after execution ownership has already been transferred.
            throw std::runtime_error("factory failure");
        });
    };
    EXPECT_THROW(coro::run(root()), std::runtime_error);
    EXPECT_TRUE(cancelled);
}

TEST(Nursery, SuccessfulDiscard_FrameIsReapedBeforeNextLoopTurn) {
    auto root = []() -> coro::Task<void> {
        co_await coro::with_nursery(
            [](coro::Nursery& group) -> coro::Task<void> {
                auto token = std::make_shared<int>(1);
                group.spawn_discard(hold_parameter(token));
                co_await coro::checkpoint();
                co_await coro::checkpoint();
                co_await coro::checkpoint();
                EXPECT_EQ(token.use_count(), 1);
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    coro::run(root());
}

TEST(Task, await_EmptyOrAlreadyAwaitedTask_ThrowsLogicError) {
    auto root = []() -> coro::Task<void> {
        coro::Task<int> empty;
        EXPECT_THROW(co_await empty, std::logic_error);
        auto task = constant(42);
        EXPECT_EQ(co_await task, 42);
        EXPECT_THROW(co_await task, std::logic_error);
    };
    coro::run(root());
}

TEST(Nursery, BodyNestedManualCancellation_CancelsChildrenBeforeDrain) {
    for (const bool supervisor : {false, true}) {
        for (const bool absolute : {false, true}) {
            coro::ManualClock clock;
            coro::Loop loop{clock};
            bool child_cancelled = false;
            auto root = [&]() -> coro::Task<void> {
                // Guard the broken implementation too: a regression fails at
                // 100ms instead of leaving the entire test binary parked.
                [[maybe_unused]] auto guarded = co_await coro::with_timeout(100ms, [&]() -> coro::Task<void> {
                    auto body = [&](coro::Nursery& group) -> coro::Task<void> {
                        group.spawn(wait_until_cancelled(child_cancelled));
                        auto cancel_body = []() -> coro::Task<void> {
                            co_await coro::sleep_for(1ms);
                            (co_await coro::current_scope()).cancel();
                            co_await coro::checkpoint();
                        };
                        if (absolute) {
                            [[maybe_unused]] auto outcome = co_await coro::with_deadline(clock.now() + 1s, cancel_body);
                        } else {
                            [[maybe_unused]] auto outcome = co_await coro::with_timeout(1s, cancel_body);
                        }
                    };
                    if (supervisor) {
                        co_await coro::with_nursery(body, coro::NurseryPolicy::SUPERVISOR);
                    } else {
                        co_await coro::with_nursery(body);
                    }
                });
            };
            EXPECT_THROW(coro::run(loop, root()), coro::Cancelled);
            EXPECT_TRUE(child_cancelled);
            EXPECT_EQ(clock.now().time_since_epoch(), 1ms);
        }
    }
}

coro::Task<void> observe_exhaustion(coro::Nursery& group, bool& exhausted) {
    EXPECT_FALSE((co_await group.next<int>()).has_value());
    exhausted = true;
}

coro::Task<void> consume_handle(coro::Handle<int> handle) {
    EXPECT_EQ(co_await handle, 42);
}

TEST(Nursery, HandleClaim_WakesNextBeforeProducerCompletes) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool exhausted = false;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                auto handle = group.spawn(sleep_then_value(100ms, 42));
                group.spawn_discard(observe_exhaustion(group, exhausted));
                co_await coro::sleep_for(1ms);  // next() has parked
                group.spawn_discard(consume_handle(handle));
                co_await coro::sleep_for(1ms);
                EXPECT_TRUE(exhausted);
                EXPECT_EQ(clock.now().time_since_epoch(), 2ms);
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    coro::run(loop, root());
    EXPECT_TRUE(exhausted);
}

TEST(Nursery, join_ConcurrentCopies_RejectsReservationAndCancelRestoresResult) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool reserved = false;
    coro::CancelScope* waiter_scope = nullptr;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                auto producer = []() -> coro::Task<int> {
                    co_await coro::sleep_for(100ms);
                    co_return 42;
                };
                auto handle = group.spawn(producer());
                auto copy = handle;
                auto first = [&]() -> coro::Task<void> {
                    const auto outcome =
                        co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                            waiter_scope = &scope;
                            reserved = true;
                            [[maybe_unused]] auto value = co_await handle;
                        });
                    EXPECT_TRUE(outcome.cancelled);
                };
                auto waiter = group.spawn(first());
                while (!reserved) {
                    co_await coro::checkpoint();
                }
                EXPECT_FALSE(copy.valid());
                EXPECT_THROW(co_await copy, std::logic_error);
                waiter_scope->cancel();
                co_await waiter;
                EXPECT_TRUE(copy.valid());
                EXPECT_EQ(co_await copy, 42);
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    coro::run(loop, root());
}

TEST(Nursery, join_LocalTimeout_ReleasesReservationAndLeavesProducerRunning) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
            auto handle = group.spawn(sleep_then_value(100ms, 42));
            auto outcome = co_await coro::with_timeout(5ms, [&]() -> coro::Task<int> { co_return co_await handle; });
            EXPECT_TRUE(outcome.timed_out);
            EXPECT_EQ(clock.now(), coro::TimePoint{} + 5ms);
            EXPECT_TRUE(handle.valid());
            EXPECT_FALSE(group.scope().cancelled());
            EXPECT_EQ(co_await handle, 42);
        });
    };
    coro::run(loop, root());
}

TEST(Nursery, join_CompletionAndTimeoutSameDeadline_LeavesResultUnconsumed) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
            auto handle = group.spawn(sleep_then_value(5ms, 42));
            auto outcome = co_await coro::with_timeout(5ms, [&]() -> coro::Task<int> { co_return co_await handle; });
            EXPECT_TRUE(outcome.timed_out);
            EXPECT_TRUE(handle.valid());
            const auto result = co_await group.next<int>();
            EXPECT_TRUE(result.has_value());
            if (result) {
                EXPECT_EQ(result->value(), 42);
            }
            EXPECT_FALSE(handle.valid());
        });
    };
    coro::run(loop, root());
}

TEST(Nursery, join_AlreadyCancelledCaller_DoesNotReserveReadyResult) {
    auto root = []() -> coro::Task<void> {
        co_await coro::with_nursery([](coro::Nursery& group) -> coro::Task<void> {
            auto handle = group.spawn(constant(42));
            co_await coro::checkpoint();
            co_await coro::checkpoint();
            auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                scope.cancel();
                [[maybe_unused]] auto result = co_await handle;
            });
            EXPECT_TRUE(outcome.cancelled);
            EXPECT_TRUE(handle.valid());
            EXPECT_EQ(co_await handle, 42);
        });
    };
    coro::run(root());
}

}  // namespace
