//
// Coroutine runtime — AsyncMutex: mutual exclusion, FIFO fairness, cancelled
// waiter.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <coroutine>

#include <gtest/gtest.h>

#include "coro/coro.h"

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
        co_await coro::with_nursery(
            [&mutex, &counter, &max_overlap, &completed](coro::Nursery& group) -> coro::Task<void> {
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
        co_await coro::with_nursery([&mutex, &order, &guard](coro::Nursery& group) -> coro::Task<void> {
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
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
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

TEST(AsyncMutex, SharedDescriptor_ConcurrentFramesHaveIndependentRegistrations) {
    coro::AsyncMutex mutex;
    const auto operation = mutex.lock();
    coro::ManualClock clock;
    coro::Loop loop{clock};
    std::vector<int> entered;
    int active = 0;
    auto contender = [&](int id) -> coro::Task<void> {
        auto guard = co_await operation;
        EXPECT_EQ(++active, 1);
        entered.push_back(id);
        co_await coro::sleep_for(1ms);
        --active;
    };
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
            group.spawn(contender(1));
            group.spawn(contender(2));
            co_return;
        });
    };
    coro::run(loop, root());
    EXPECT_EQ(entered, (std::vector<int>{1, 2}));
    EXPECT_FALSE(mutex.locked());
}

TEST(AsyncMutex, lock_ReusedAfterCancellationAndHandoff_HasIndependentWaitState) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    coro::AsyncMutex mutex;
    auto root = [&]() -> coro::Task<void> {
        const auto operation = mutex.lock();
        auto holder = co_await operation;
        auto timeout = co_await coro::with_timeout(
            1ms, [&]() -> coro::Task<void> { [[maybe_unused]] auto blocked = co_await operation; });
        EXPECT_TRUE(timeout.timed_out);
        holder.unlock();
        {
            auto guard = co_await operation;
            EXPECT_TRUE(static_cast<bool>(guard));
        }
        EXPECT_FALSE(mutex.locked());
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
            auto guard = co_await operation;
            auto release = [&]() -> coro::Task<void> {
                co_await coro::sleep_for(1ms);
                guard.unlock();
            };
            group.spawn(release());
            auto handed = co_await operation;
            EXPECT_TRUE(static_cast<bool>(handed));
            handed.unlock();
            auto reused = co_await operation;
            EXPECT_TRUE(static_cast<bool>(reused));
            co_return;
        });
    };
    coro::run(loop, root());
    EXPECT_FALSE(mutex.locked());
}

}  // namespace
