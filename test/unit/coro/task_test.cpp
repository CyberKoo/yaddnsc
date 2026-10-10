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
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include "infrastructure/coro/coro.h"

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
concept PublicLoopRegistration = requires(T& loop, coro::detail::TimerNode& timer, coro::detail::WaitNode& node) {
    loop.add_timer(timer, coro::TimePoint{}, nullptr, nullptr);
    loop.remove_timer(timer);
    loop.remove_fd(0);
    loop.arm_signal(0, node);
    loop.disarm_signal(0, node);
};

template<typename T>
concept PublicLoopPool = requires(T& loop) { loop.offload_pool(); };

template<typename T>
concept PublicScopeWaiter = requires(T& scope, coro::detail::WaitNode& node) { scope.add_waiter(node); };

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

coro::Task<void> cancel_after_sleep(coro::TaskGroup& group, std::chrono::milliseconds delay) {
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
    EXPECT_FALSE((std::is_constructible_v<coro::TaskGroup, coro::detail::GroupState*>) );
    EXPECT_FALSE((std::is_constructible_v<coro::Handle<int>, coro::detail::ChildSlot*>) );
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

TEST(TaskGroup, spawn_ThenJoin_YieldsEveryValue) {
    int total = 0;
    auto task = [&total]() -> coro::Task<void> {
        co_await coro::task_group([&total](coro::TaskGroup& group) -> coro::Task<void> {
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

TEST(TaskGroup, spawn_AwaitHandleOfThrowingChild_RethrowsAtJoin) {
    bool caught = false;
    auto task = [&caught]() -> coro::Task<void> {
        // supervisor_group reports a child defect instead of propagating it, so
        // the only path left is the Handle join.
        co_await coro::supervisor_group([&caught](coro::TaskGroup& group) -> coro::Task<void> {
            auto handle = group.spawn(throw_runtime_error("child boom"));
            try {
                co_await handle;
            } catch (const std::runtime_error& error) {
                caught = std::string(error.what()) == "child boom";
            }
            co_return;
        });
        co_return;
    };
    coro::run(task());
    EXPECT_TRUE(caught);
}

// ---------------------------------------------------------------------------
// task_group: join-all, first failure cancels siblings then propagates
// ---------------------------------------------------------------------------

TEST(TaskGroup, task_group_NormalExit_JoinsEveryChild) {
    int completed = 0;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&completed]() -> coro::Task<void> {
        co_await coro::task_group([&completed](coro::TaskGroup& group) -> coro::Task<void> {
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

TEST(TaskGroup, task_group_FirstChildThrows_CancelsSiblingsThenPropagates) {
    bool sibling_started = false;
    bool sibling_saw_cancellation = false;
    int sibling_ticks = 0;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&sibling_started, &sibling_saw_cancellation, &sibling_ticks]() -> coro::Task<void> {
        co_await coro::task_group(
            [&sibling_started, &sibling_saw_cancellation, &sibling_ticks](coro::TaskGroup& group) -> coro::Task<void> {
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
// supervisor_group: a child failure is reported, siblings are untouched
// ---------------------------------------------------------------------------

TEST(TaskGroup, supervisor_group_ChildThrows_ReportsAndLeavesSiblings) {
    bool saw_error = false;
    bool saw_value = false;
    bool sibling_finished = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&saw_error, &saw_value, &sibling_finished]() -> coro::Task<void> {
        co_await coro::supervisor_group(
            [&saw_error, &saw_value, &sibling_finished](coro::TaskGroup& group) -> coro::Task<void> {
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
            });
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

TEST(TaskGroup, next_ChildrenFinishOutOfOrder_YieldsCompletionOrder) {
    std::vector<int> order;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&order]() -> coro::Task<void> {
        co_await coro::task_group([&order](coro::TaskGroup& group) -> coro::Task<void> {
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

TEST(TaskGroup, next_ChildThrows_ReportsDefectAsValue) {
    bool saw_error = false;
    auto task = [&saw_error]() -> coro::Task<void> {
        co_await coro::supervisor_group([&saw_error](coro::TaskGroup& group) -> coro::Task<void> {
            group.spawn(throw_runtime_error("next reports this"));
            while (auto outcome = co_await group.next<void>()) {
                saw_error = !outcome->has_value();
            }
            co_return;
        });
        co_return;
    };
    coro::run(task());
    EXPECT_TRUE(saw_error);
}

// ---------------------------------------------------------------------------
// explicit group cancellation
// ---------------------------------------------------------------------------

TEST(TaskGroup, cancel_Requested_StopsChildren) {
    bool child_cancelled = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&child_cancelled]() -> coro::Task<void> {
        co_await coro::task_group([&child_cancelled](coro::TaskGroup& group) -> coro::Task<void> {
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

TEST(TaskGroup, spawn_discard_ScopeExitJoinsAChildStillInFlight) {
    int completed = 0;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&completed]() -> coro::Task<void> {
        co_await coro::supervisor_group([&completed](coro::TaskGroup& group) -> coro::Task<void> {
            group.spawn_discard(increment_after_sleep(completed, 20ms));
            // The body leaves at once; the group must still join the child
            // instead of reaping its live frame.
            co_return;
        });
        co_return;
    };
    coro::run(loop, task());
    EXPECT_EQ(completed, 1);
}

TEST(TaskGroup, spawn_discard_IsNeverDeliveredThroughNext) {
    int discard_completed = 0;
    std::vector<int> delivered;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&discard_completed, &delivered]() -> coro::Task<void> {
        co_await coro::supervisor_group([&discard_completed, &delivered](coro::TaskGroup& group) -> coro::Task<void> {
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
        });
        co_return;
    };
    coro::run(loop, task());
    EXPECT_EQ(delivered, (std::vector<int>{7}));
    EXPECT_EQ(discard_completed, 1);
}

TEST(TaskGroup, spawn_discard_DefectCancelsSiblingsAndPropagates) {
    // A discard child's defect is a child defect like any other: task_group
    // cancels the siblings and rethrows it at scope exit.
    bool sibling_cancelled = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto task = [&sibling_cancelled]() -> coro::Task<void> {
        co_await coro::task_group([&sibling_cancelled](coro::TaskGroup& group) -> coro::Task<void> {
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

TEST(TaskGroup, spawn_discard_ManyChildren_AllRunAndAreJoined) {
    // Churn through many discard children in one group: every completion
    // retires the previous one's frame, which the sanitizer build validates.
    int completed = 0;
    auto task = [&completed]() -> coro::Task<void> {
        co_await coro::supervisor_group([&completed](coro::TaskGroup& group) -> coro::Task<void> {
            for (int i = 0; i < 500; ++i) {
                group.spawn_discard(increment_after_sleep(completed, 0ms));
            }
            co_return;
        });
        co_return;
    };
    coro::run(task());
    EXPECT_EQ(completed, 500);
}

struct ThrowingMove {
    std::shared_ptr<int> lifetime;
    bool* moved;

    ThrowingMove(std::shared_ptr<int> token, bool& flag) : lifetime(std::move(token)), moved(&flag) {}

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

TEST(TaskGroup, next_NoMatchingChildren_ReturnsWithoutWaitingForOtherTypes) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool cancelled = false;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            group.spawn(wait_until_cancelled(cancelled));
            EXPECT_FALSE((co_await group.next<int>()).has_value());
            group.cancel();
        });
    };
    coro::run(loop, root());
}

TEST(TaskGroup, next_LastMatchingChildConsumed_ReturnsWhileOtherTypesRun) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool cancelled = false;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
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

TEST(TaskGroup, join_ResultClaimedOnce_AcrossCopiesAndNext) {
    auto root = []() -> coro::Task<void> {
        co_await coro::task_group([](coro::TaskGroup& group) -> coro::Task<void> {
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

TEST(TaskGroup, join_NextConsumedResult_RejectsJoin) {
    auto root = []() -> coro::Task<void> {
        co_await coro::task_group([](coro::TaskGroup& group) -> coro::Task<void> {
            auto handle = group.spawn(constant(42));
            EXPECT_TRUE((co_await group.next<int>()).has_value());
            EXPECT_THROW(co_await handle, std::logic_error);
            coro::Handle<int> empty;
            EXPECT_THROW(co_await empty, std::logic_error);
        });
    };
    coro::run(root());
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

}  // namespace
