//
// Coroutine runtime — signals as coroutine events.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <chrono>
#include <csignal>

#include <gtest/gtest.h>

#include "infrastructure/coro/coro.h"

namespace {

using namespace std::chrono_literals;

coro::Task<void> wait_for_signal(bool* delivered) {
    auto result = co_await coro::on_signal(SIGUSR1);
    *delivered = result.has_value();
    co_return;
}

coro::Task<void> raise_signal_after(std::chrono::milliseconds delay) {
    // Arming the handler takes one ready-queue hop (the on_signal task frame is
    // scheduled before it runs), so wait for the loop to get there. Raising
    // before the handler is installed would take the default action.
    co_await coro::sleep_for(delay);
    ::raise(SIGUSR1);
    co_return;
}

TEST(Signal, on_signal_SignalRaised_Completes) {
    coro::Loop loop;
    bool delivered = false;

    auto task = [&delivered]() -> coro::Task<void> {
        co_await coro::task_group([&delivered](coro::TaskGroup& group) -> coro::Task<void> {
            group.spawn(wait_for_signal(&delivered));
            group.spawn(raise_signal_after(5ms));
            co_return;
        });
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(delivered);
}

TEST(Signal, on_signal_ScopeTimesOut_CancelsWait) {
    coro::Loop loop;
    bool timed_out = false;
    bool wait_cancelled = false;

    auto task = [&timed_out, &wait_cancelled]() -> coro::Task<void> {
        auto outcome =
            co_await coro::with_timeout(20ms, [&wait_cancelled](coro::CancelScope& scope) -> coro::Task<void> {
                auto result = co_await coro::on_signal(SIGUSR2);
                wait_cancelled = !result.has_value();
                EXPECT_TRUE(scope.timed_out());
                co_return;
            });
        timed_out = outcome.timed_out;
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(timed_out);
    EXPECT_TRUE(wait_cancelled);
}

TEST(Signal, on_signal_RepeatedSignals_HandledInLoop) {
    coro::Loop loop;
    int handled = 0;

    auto watcher = [&handled]() -> coro::Task<void> {
        for (int i = 0; i < 2; ++i) {
            auto result = co_await coro::on_signal(SIGUSR1);
            if (!result.has_value()) {
                co_return;
            }
            ++handled;
        }
        co_return;
    };
    auto first_raise = []() -> coro::Task<void> {
        co_await coro::sleep_for(5ms);
        ::raise(SIGUSR1);
        co_return;
    };
    auto second_raise = []() -> coro::Task<void> {
        co_await coro::sleep_for(15ms);
        ::raise(SIGUSR1);
        co_return;
    };
    auto task = [&watcher, &first_raise, &second_raise]() -> coro::Task<void> {
        co_await coro::task_group([&watcher, &first_raise, &second_raise](coro::TaskGroup& group) -> coro::Task<void> {
            group.spawn(watcher());
            group.spawn(first_raise());
            group.spawn(second_raise());
            co_return;
        });
        co_return;
    };

    coro::run(loop, task());
    EXPECT_EQ(handled, 2);
}

}  // namespace
