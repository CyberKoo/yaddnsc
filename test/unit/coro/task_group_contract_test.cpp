//
// Coroutine runtime — the TaskGroup/Handle contracts and the AsyncMutex entry
// paths that the rest of the coro suite never reaches.
//
// Handle<T>::valid() on an empty handle, joining a child that has already
// finished, joining under a cancelled scope, the waiter list behind next<T()>,
// the type filter take_completed() applies, and the aggregation an exiting
// supervisor group performs over mixed child outcomes.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "coro/coro.h"

namespace {

using namespace std::chrono_literals;

coro::Task<int> value_after(coro::Duration delay, int value) {
    co_await coro::sleep_for(delay);
    co_return value;
}

coro::Task<void> complete_after(coro::Duration delay) {
    co_await coro::sleep_for(delay);
    co_return;
}

coro::Task<void> fail_after(coro::Duration delay, const char* what) {
    co_await coro::sleep_for(delay);
    throw std::runtime_error{what};
}

/// Fails on its first resumption without ever suspending, so a sibling that
/// fails in the same turn is not turned into a cancellation first.
coro::Task<void> fail_on_first_resume(const char* what) {
    [[maybe_unused]] const coro::CancelScope& scope = co_await coro::current_scope();
    throw std::runtime_error{what};
}

/// Parks in next<int>() under its own cancel scope so the test can detach one
/// waiter from the middle of the group's waiter list.
coro::Task<void> park_in_next(coro::TaskGroup* group, coro::CancelScope** scope_slot, bool* cancelled) {
    const auto outcome = co_await coro::with_cancel_scope([=](coro::CancelScope& scope) -> coro::Task<void> {
        *scope_slot = &scope;
        [[maybe_unused]] const std::optional<coro::Result<int>> result = co_await group->next<int>();
        co_return;
    });
    *cancelled = outcome.cancelled;
}

}  // namespace

// ---------------------------------------------------------------------------
// Handle<T> — the contracts its own documentation states
// ---------------------------------------------------------------------------

TEST(TaskGroupContract, DefaultConstructedHandle_ReportsInvalid) {
    coro::Handle<void> void_handle;
    coro::Handle<int> int_handle;
    EXPECT_FALSE(void_handle.valid());
    EXPECT_FALSE(int_handle.valid());
}

TEST(TaskGroupContract, JoinEmptyHandle_RejectsTheMissingChildResult) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool rejected = false;

    auto root = [&]() -> coro::Task<void> {
        try {
            co_await coro::task_group([](coro::TaskGroup&) -> coro::Task<void> {
                coro::Handle<int> empty;
                co_await empty;
            });
        } catch (const std::logic_error&) {
            rejected = true;
        }
        co_return;
    };

    coro::run(loop, root());
    EXPECT_TRUE(rejected);
}

TEST(TaskGroupContract, JoinCompletedChild_YieldsItsValueWithoutSuspending) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    int joined = 0;
    bool still_valid = false;

    auto root = [&]() -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            auto handle = group.spawn(value_after(1ms, 7));
            co_await coro::sleep_for(2ms);
            still_valid = handle.valid();
            joined = co_await handle;
            co_return;
        });
        co_return;
    };

    coro::run(loop, root());
    EXPECT_EQ(joined, 7);
    EXPECT_TRUE(still_valid);
}

TEST(TaskGroupContract, JoinCancelledByOwningScope_ReleasesTheReservationWithoutConsuming) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool timed_out = false;
    bool released = false;
    int rejoined = 0;

    auto root = [&]() -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            auto handle = group.spawn(value_after(10ms, 7));
            const auto outcome = co_await coro::with_timeout(1ms, [&handle]() -> coro::Task<void> {
                try {
                    [[maybe_unused]] const int value = co_await handle;
                } catch (const coro::Cancelled&) {
                }
                co_return;
            });
            timed_out = outcome.timed_out;
            // The wait was cancelled, not consumed: the child is still claimable
            // and its value still arrives for the next waiter.
            released = handle.valid();
            rejoined = co_await handle;
            co_return;
        });
        co_return;
    };

    coro::run(loop, root());
    EXPECT_TRUE(timed_out);
    EXPECT_TRUE(released);
    EXPECT_EQ(rejoined, 7);
}

// ---------------------------------------------------------------------------
// next<T>() — the waiter list and the type filter
// ---------------------------------------------------------------------------

TEST(TaskGroupContract, Next_SkipsACompletedChildOfAnotherType) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    std::optional<coro::Result<int>> first;
    bool exhausted = false;

    auto root = [&]() -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            group.spawn(complete_after(1ms));
            group.spawn(value_after(20ms, 7));
            co_await coro::sleep_for(2ms);
            first = co_await group.next<int>();
            exhausted = !(co_await group.next<int>()).has_value();
            co_return;
        });
        co_return;
    };

    coro::run(loop, root());
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(first->has_value());
    EXPECT_EQ(**first, 7);
    EXPECT_TRUE(exhausted);
}

TEST(TaskGroupContract, Next_CancelledWaiterDetachesFromTheMiddleOfTheList) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    coro::CancelScope* scopes[3] = {nullptr, nullptr, nullptr};
    bool cancelled[3] = {false, false, false};

    auto root = [&]() -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            for (int index = 0; index < 3; ++index) {
                group.spawn(park_in_next(&group, &scopes[index], &cancelled[index]));
            }
            group.spawn(value_after(20ms, 7));
            // Every waiter has parked by the time the body's own sleep expires.
            co_await coro::sleep_for(1ms);
            for (const coro::CancelScope* scope : scopes) {
                EXPECT_NE(scope, nullptr);
            }
            if (scopes[1] != nullptr) {
                scopes[1]->cancel();
            }
            co_await coro::sleep_for(30ms);
            co_return;
        });
        co_return;
    };

    coro::run(loop, root());
    EXPECT_TRUE(cancelled[1]);
    EXPECT_FALSE(cancelled[0]);
    EXPECT_FALSE(cancelled[2]);
}

// ---------------------------------------------------------------------------
// Group exits — the aggregation and the first-defect bookkeeping
// ---------------------------------------------------------------------------

TEST(TaskGroupContract, SupervisorGroup_UnconsumedSuccessIsNotReportedAsAFailure) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    std::size_t reported = 0;

    auto root = [&]() -> coro::Task<void> {
        try {
            co_await coro::supervisor_group([](coro::TaskGroup& group) -> coro::Task<void> {
                group.spawn_discard(fail_after(1ms, "discarded defect"));
                // Never consumed, so its slot survives to the exit aggregation.
                group.spawn(value_after(2ms, 7));
                co_await coro::sleep_for(5ms);
                co_return;
            });
        } catch (const coro::GroupError& error) {
            reported = error.errors().size();
        }
        co_return;
    };

    coro::run(loop, root());
    EXPECT_EQ(reported, 1u);
}

TEST(TaskGroupContract, TaskGroup_SecondFailingChild_KeepsTheFirstDefect) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    std::string reported;

    auto root = [&]() -> coro::Task<void> {
        try {
            co_await coro::task_group([](coro::TaskGroup& group) -> coro::Task<void> {
                group.spawn(fail_on_first_resume("first defect"));
                group.spawn(fail_on_first_resume("second defect"));
                co_return;
            });
        } catch (const std::runtime_error& error) {
            reported = error.what();
        }
        co_return;
    };

    coro::run(loop, root());
    EXPECT_TRUE(reported == "first defect" || reported == "second defect");
}

// ---------------------------------------------------------------------------
// AsyncMutex — lock() on a scope that is already cancelled
// ---------------------------------------------------------------------------

TEST(AsyncMutexContract, LockUnderCancelledScope_ThrowsWithoutGrantingTheLock) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool cancelled = false;
    bool scope_reported_cancelled = false;
    bool free_after_attempt = false;
    bool next_lock_granted = false;
    coro::AsyncMutex mutex;

    auto root = [&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
            scope.cancel();
            try {
                [[maybe_unused]] const coro::MutexGuard guard = co_await mutex.lock();
            } catch (const coro::Cancelled&) {
                cancelled = true;
            }
            free_after_attempt = !mutex.locked();
            co_return;
        });
        scope_reported_cancelled = outcome.cancelled;
        coro::MutexGuard guard = co_await mutex.lock();
        next_lock_granted = static_cast<bool>(guard);
        guard.unlock();
        co_return;
    };

    coro::run(loop, root());
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(scope_reported_cancelled);
    EXPECT_TRUE(free_after_attempt);
    EXPECT_TRUE(next_lock_granted);
    EXPECT_FALSE(mutex.locked());
}