//
// Coroutine runtime — AsyncMutex: mutual exclusion, FIFO fairness, cancelled
// waiter.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <algorithm>
#include <chrono>
#include <vector>

#include <gtest/gtest.h>

#include "infrastructure/coro/coro.h"

namespace {

using namespace std::chrono_literals;

coro::Task<void> enter_hold_leave(coro::AsyncMutex& mutex, int& counter, int& max_overlap, int& completed) {
    auto guard = co_await mutex.lock();
    max_overlap = std::max(max_overlap, ++counter);
    co_await coro::sleep_for(5ms);
    --counter;
    ++completed;
    co_return;
}

coro::Task<void> acquire_record_release(coro::AsyncMutex& mutex, int id, std::vector<int>& order) {
    auto guard = co_await mutex.lock();
    order.push_back(id);
    co_await coro::sleep_for(1ms);
    co_return;
}

TEST(AsyncMutex, lock_ThreeHolders_SerializesCriticalSections) {
    coro::AsyncMutex mutex;
    int counter = 0;
    int max_overlap = 0;
    int completed = 0;
    coro::ManualClock clock;
    coro::Loop loop{clock};

    auto task = [&mutex, &counter, &max_overlap, &completed]() -> coro::Task<void> {
        co_await coro::task_group(
            [&mutex, &counter, &max_overlap, &completed](coro::TaskGroup& group) -> coro::Task<void> {
                for (int i = 0; i < 3; ++i) {
                    group.spawn(enter_hold_leave(mutex, counter, max_overlap, completed));
                }
                co_return;
            });
        co_return;
    };

    coro::run(loop, task());
    EXPECT_EQ(completed, 3);
    EXPECT_EQ(max_overlap, 1);
    EXPECT_FALSE(mutex.locked());
}

TEST(AsyncMutex, lock_ThreeWaiters_GrantsInFifoOrder) {
    coro::AsyncMutex mutex;
    std::vector<int> order;
    coro::ManualClock clock;
    coro::Loop loop{clock};

    auto task = [&mutex, &order]() -> coro::Task<void> {
        auto guard = co_await mutex.lock();
        EXPECT_TRUE(static_cast<bool>(guard));
        co_await coro::task_group([&mutex, &order, &guard](coro::TaskGroup& group) -> coro::Task<void> {
            for (int i = 0; i < 3; ++i) {
                group.spawn(acquire_record_release(mutex, i, order));
            }
            // Give every waiter time to queue before the lock is handed over.
            co_await coro::sleep_for(10ms);
            if (static_cast<bool>(guard)) {
                guard.unlock();
            }
            co_return;
        });
        co_return;
    };

    coro::run(loop, task());
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2}));
}

TEST(AsyncMutex, lock_WaiterCancelled_ReturnsCanceledAndKeepsLockHeld) {
    coro::AsyncMutex mutex;
    bool waiter_cancelled = false;
    bool still_locked = false;
    bool released = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};

    auto task = [&mutex, &waiter_cancelled, &still_locked, &released]() -> coro::Task<void> {
        auto guard = co_await mutex.lock();
        EXPECT_TRUE(static_cast<bool>(guard));

        auto outcome = co_await coro::with_timeout(10ms, [&mutex, &waiter_cancelled]() -> coro::Task<void> {
            try {
                auto second = co_await mutex.lock();
            } catch (const coro::Cancelled&) {
                waiter_cancelled = true;
                throw;
            }
            co_return;
        });
        EXPECT_TRUE(outcome.timed_out);
        still_locked = mutex.locked();

        if (static_cast<bool>(guard)) {
            guard.unlock();
        }
        released = !mutex.locked();
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(waiter_cancelled);
    EXPECT_TRUE(still_locked);
    EXPECT_TRUE(released);
}

TEST(AsyncMutex, CancellationAfterHandoff_ReleasesGrantedLock) {
    coro::AsyncMutex mutex;
    coro::CancelScope* waiter_scope = nullptr;
    bool waiter_returned = false;
    bool cancelled = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto waiter = [&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
            waiter_scope = &scope;
            auto guard = co_await mutex.lock();
            waiter_returned = true;
        });
        cancelled = outcome.cancelled;
    };
    coro::run(loop, [&]() -> coro::Task<void> {
        auto guard = co_await mutex.lock();
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            group.spawn(waiter());
            co_await coro::sleep_for(1ms);
            EXPECT_NE(waiter_scope, nullptr);
            guard.unlock();
            if (waiter_scope != nullptr) {
                waiter_scope->cancel();
            }
        });
        EXPECT_FALSE(mutex.locked());
        auto next = co_await mutex.lock();
        EXPECT_TRUE(static_cast<bool>(next));
    }());
    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(waiter_returned);
    EXPECT_FALSE(mutex.locked());
}

}  // namespace
