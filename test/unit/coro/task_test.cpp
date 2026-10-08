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
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "infrastructure/coro/coro.h"

namespace {

using namespace std::chrono_literals;

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
    const auto slept = co_await coro::sleep_for(delay);
    if (!slept) {
        co_return -1;
    }
    co_return value;
}

coro::Task<void> increment_after_sleep(int& counter, std::chrono::milliseconds delay) {
    co_await coro::sleep_for(delay);
    ++counter;
    co_return;
}

coro::Task<int> set_flag_after_sleep(bool& flag, std::chrono::milliseconds delay, int value) {
    const auto slept = co_await coro::sleep_for(delay);
    flag = slept.has_value();
    co_return value;
}

coro::Task<void> wait_until_cancelled(bool& cancelled) {
    const auto slept = co_await coro::sleep_for(1000ms);
    cancelled = !slept.has_value();
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
        const auto slept = co_await coro::sleep_for(50ms);
        if (!slept) {
            cancelled = true;
            co_return;
        }
        ++ticks;
    }
}

// ---------------------------------------------------------------------------
// run / inline co_await
// ---------------------------------------------------------------------------

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

}  // namespace
