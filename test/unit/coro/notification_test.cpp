//
// Coroutine runtime — Notification (coro/notification.hpp).
//
// The delivery contract the plugin bridge relies on: a notification with no
// live wait is retained once, repeated notifications coalesce, consecutive
// wait rounds each get fresh registration state, cancellation beats a
// delivered-but-unresumed notification (which is then retained), a live wait
// forbids both a second wait and destroying the notification.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <chrono>
#include <coroutine>
#include <memory>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "coro/coro.h"

namespace {

using namespace std::chrono_literals;

// Spawned children are named coroutines, never immediately-invoked lambda
// coroutines: a lambda closure dies at the end of the spawn statement, while
// reference parameters copied into the frame stay valid for the whole run.

coro::Task<void> wait_rounds(coro::Notification& notification, int rounds, int& completed) {
    for (int round = 0; round < rounds; ++round) {
        co_await notification.wait();
        ++completed;
    }
}

coro::Task<void> wait_under_scope(coro::Notification& notification, coro::CancelScope*& scope, bool& cancelled) {
    const auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& child) -> coro::Task<void> {
        scope = &child;
        co_await notification.wait();
    });
    cancelled = outcome.cancelled;
}

coro::Task<void> notify_then_cancel(coro::Notification& notification, coro::CancelScope*& scope) {
    // Let the waiter reach its awaitable first: the scope-combinator hop
    // schedules the parking body one ready-queue cycle after its own frame.
    co_await coro::sleep_for(1ms);
    notification.notify();
    scope->cancel();  // lands while the wake is queued, before await_resume
}

coro::Task<void> cancel_scope_soon(coro::CancelScope*& scope) {
    co_await coro::sleep_for(1ms);  // let the waiter park first
    scope->cancel();
}

coro::Task<void> second_wait_is_rejected(coro::Notification& notification, bool& rejected) {
    co_await coro::sleep_for(1ms);  // let the first waiter park
    try {
        co_await notification.wait();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    notification.notify();  // the original wait must still complete
}

}  // namespace

TEST(Notification, NotifyBeforeWaitIsRetained) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    coro::Notification changed;
    bool waited = false;

    coro::run(loop, [&]() -> coro::Task<void> {
        changed.notify();
        co_await changed.wait();
        waited = true;
    }());

    EXPECT_TRUE(waited);
}

TEST(Notification, RepeatedNotificationsCoalesce) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    coro::Notification changed;
    int completed = 0;
    bool parked_after_one = false;

    auto observe_second_wait = [&]() -> coro::Task<void> {
        co_await coro::sleep_for(1ms);  // consume the retained notification, then park
        parked_after_one = completed == 1;
        changed.notify();
    };
    coro::run(loop, [&]() -> coro::Task<void> {
        changed.notify();
        changed.notify();
        changed.notify();
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                group.spawn_discard(wait_rounds(changed, 2, completed));
                group.spawn_discard(observe_second_wait());
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
    }());

    EXPECT_TRUE(parked_after_one);
    EXPECT_EQ(completed, 2);
}

TEST(Notification, ConsecutiveRoundsWakeEachWait) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    coro::Notification changed;
    int completed = 0;

    auto notify_three_times = [&]() -> coro::Task<void> {
        for (int round = 0; round < 3; ++round) {
            changed.notify();
            co_await coro::sleep_for(1ms);  // let the waiter resume and park again
        }
    };
    coro::run(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                group.spawn_discard(wait_rounds(changed, 3, completed));
                group.spawn_discard(notify_three_times());
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
    }());

    EXPECT_EQ(completed, 3);
}

TEST(Notification, CancelDuringWaitThrowsCancelled) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    coro::Notification changed;
    coro::CancelScope* scope = nullptr;
    bool cancelled = false;

    coro::run(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                group.spawn_discard(wait_under_scope(changed, scope, cancelled));
                group.spawn_discard(cancel_scope_soon(scope));
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
    }());

    EXPECT_TRUE(cancelled);
}

TEST(Notification, CancelAfterNotifyWinsAndKeepsTheNotification) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    coro::Notification changed;
    coro::CancelScope* scope = nullptr;
    bool cancelled = false;
    bool retained = false;

    coro::run(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                group.spawn_discard(wait_under_scope(changed, scope, cancelled));
                group.spawn_discard(notify_then_cancel(changed, scope));
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        // The cancelled wait lost, so the delivered notification must still be
        // there: this probe completes without anyone notifying again.
        const auto probe = co_await coro::with_timeout(1ms, [&]() -> coro::Task<void> { co_await changed.wait(); });
        retained = probe.completed;
    }());

    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(retained);
}

TEST(Notification, SecondLiveWaitThrowsLogicError) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    coro::Notification changed;
    int completed = 0;
    bool rejected = false;

    coro::run(loop, [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                group.spawn_discard(wait_rounds(changed, 1, completed));
                group.spawn_discard(second_wait_is_rejected(changed, rejected));
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
    }());

    EXPECT_TRUE(rejected);
    EXPECT_EQ(completed, 1);
}

TEST(Notification, DestroyWithQueuedWaiterTerminates) {
    EXPECT_DEATH(
        {
            coro::ManualClock clock;
            coro::Loop loop{clock};
            auto changed = std::make_unique<coro::Notification>();
            coro::run(loop, [&]() -> coro::Task<void> {
                co_await coro::with_nursery(
                    [&](coro::Nursery& group) -> coro::Task<void> {
                        int completed = 0;
                        group.spawn_discard(wait_rounds(*changed, 1, completed));
                        co_await coro::sleep_for(1ms);  // let the waiter park
                        changed->notify();              // queued, not yet resumed
                        changed.reset();                // a live wait forbids destruction
                        co_return;
                    },
                    coro::NurseryPolicy::SUPERVISOR);
            }());
        },
        "");
}
