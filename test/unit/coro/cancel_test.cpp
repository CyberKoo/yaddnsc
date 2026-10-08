//
// Coroutine runtime — cancellation combinators and sleep cancellation.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <chrono>
#include <vector>

#include <gtest/gtest.h>

#include "infrastructure/coro/coro.h"

namespace {

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// with_timeout
// ---------------------------------------------------------------------------

TEST(CancelScope, with_timeout_DeadlineFires_CancelsBodyAndAbsorbs) {
    bool body_saw_cancellation = false;
    bool scope_reported_timeout = false;
    bool outcome_reported_timeout = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    const auto start = clock.now();

    auto task = [&body_saw_cancellation, &scope_reported_timeout, &outcome_reported_timeout]() -> coro::Task<void> {
        auto outcome = co_await coro::with_timeout(
            100ms, [&body_saw_cancellation, &scope_reported_timeout](coro::CancelScope& scope) -> coro::Task<void> {
                const auto slept = co_await coro::sleep_for(10s);
                body_saw_cancellation = !slept.has_value();
                scope_reported_timeout = scope.timed_out();
                co_return;
            });
        outcome_reported_timeout = outcome.timed_out;
        // The combinator returns normally even though the scope was cancelled.
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(body_saw_cancellation);
    EXPECT_TRUE(scope_reported_timeout);
    EXPECT_TRUE(outcome_reported_timeout);
    EXPECT_EQ(clock.now() - start, 100ms);
}

TEST(CancelScope, with_timeout_DeadlineFires_DoesNotLeakToOuterScope) {
    bool after_timeout_ran = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};

    auto task = [&after_timeout_ran]() -> coro::Task<void> {
        [[maybe_unused]] const auto ignored =
            co_await coro::with_timeout(10ms, [](coro::CancelScope&) -> coro::Task<void> {
                [[maybe_unused]] const auto slept = co_await coro::sleep_for(1s);
                co_return;
            });
        // If the timeout had propagated, this sleep would be cancelled too.
        const auto slept = co_await coro::sleep_for(50ms);
        after_timeout_ran = slept.has_value();
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(after_timeout_ran);
}

TEST(CancelScope, with_timeout_BodyFinishesEarly_RemovesDeadline) {
    int completed = 0;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    const auto start = clock.now();

    auto task = [&completed]() -> coro::Task<void> {
        auto outcome = co_await coro::with_timeout(10s, [](coro::CancelScope&) -> coro::Task<int> {
            [[maybe_unused]] const auto slept = co_await coro::sleep_for(100ms);
            co_return 5;
        });
        EXPECT_FALSE(outcome.timed_out);
        EXPECT_TRUE(outcome.has_value());
        if (outcome.has_value()) {
            completed = *outcome;
        }
        co_return;
    };

    coro::run(loop, task());
    EXPECT_EQ(completed, 5);
    EXPECT_EQ(clock.now() - start, 100ms);
}

// ---------------------------------------------------------------------------
// with_deadline
// ---------------------------------------------------------------------------

TEST(CancelScope, with_deadline_DeadlineReached_CancelsBody) {
    bool timed_out = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    const auto deadline = clock.now() + 40ms;

    auto task = [&timed_out, deadline]() -> coro::Task<void> {
        auto outcome = co_await coro::with_deadline(deadline, [](coro::CancelScope&) -> coro::Task<void> {
            [[maybe_unused]] const auto slept = co_await coro::sleep_for(5s);
            co_return;
        });
        timed_out = outcome.timed_out;
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(timed_out);
    EXPECT_EQ(clock.now(), deadline);
}

TEST(CancelScope, with_deadline_NotReached_CompletesNormally) {
    bool completed = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};

    auto task = [&completed, &clock]() -> coro::Task<void> {
        auto outcome = co_await coro::with_deadline(clock.now() + 100ms, [](coro::CancelScope&) -> coro::Task<void> {
            [[maybe_unused]] const auto slept = co_await coro::sleep_for(10ms);
            co_return;
        });
        completed = outcome.completed;
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(completed);
}

// ---------------------------------------------------------------------------
// with_cancel_scope: cancellation requested by hand
// ---------------------------------------------------------------------------

TEST(CancelScope, with_cancel_scope_CancelledByPeer_StopsBody) {
    bool victim_cancelled = false;
    bool canceller_saw_scope = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    coro::CancelScope* victim_scope = nullptr;

    auto victim = [&victim_cancelled, &victim_scope]() -> coro::Task<void> {
        auto outcome = co_await coro::with_cancel_scope(
            [&victim_cancelled, &victim_scope](coro::CancelScope& scope) -> coro::Task<void> {
                victim_scope = &scope;
                const auto slept = co_await coro::sleep_for(10s);
                victim_cancelled = !slept.has_value();
                co_return;
            });
        EXPECT_FALSE(outcome.timed_out);
        co_return;
    };
    auto canceller = [&canceller_saw_scope, &victim_scope]() -> coro::Task<void> {
        [[maybe_unused]] const auto slept = co_await coro::sleep_for(5ms);
        canceller_saw_scope = victim_scope != nullptr;
        if (victim_scope != nullptr) {
            victim_scope->cancel();
        }
        co_return;
    };
    auto task = [&victim, &canceller]() -> coro::Task<void> {
        co_await coro::task_group([&victim, &canceller](coro::TaskGroup& group) -> coro::Task<void> {
            group.spawn(victim());
            group.spawn(canceller());
            co_return;
        });
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(canceller_saw_scope);
    EXPECT_TRUE(victim_cancelled);
}

// ---------------------------------------------------------------------------
// non_cancellable: cleanup survives an outer cancellation
// ---------------------------------------------------------------------------

TEST(CancelScope, non_cancellable_OuterCancel_CleanupStillCompletes) {
    bool first_await_cancelled = false;
    bool cleanup_completed = false;
    bool shield_reported_cancelled = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};

    auto task = [&first_await_cancelled, &cleanup_completed, &shield_reported_cancelled]() -> coro::Task<void> {
        [[maybe_unused]] const auto ignored = co_await coro::with_timeout(
            10ms,
            [&first_await_cancelled, &cleanup_completed,
             &shield_reported_cancelled](coro::CancelScope&) -> coro::Task<void> {
                const auto first = co_await coro::sleep_for(1s);
                first_await_cancelled = !first.has_value();
                // Cleanup runs under a shield: the outer cancellation must not
                // reach it, so this sleep completes normally.
                auto shielded = co_await coro::non_cancellable(
                    [&shield_reported_cancelled](coro::CancelScope& shield) -> coro::Task<int> {
                        shield_reported_cancelled = shield.cancelled();
                        const auto slept = co_await coro::sleep_for(50ms);
                        co_return slept.has_value() ? 1 : 0;
                    });
                cleanup_completed = shielded.has_value() && *shielded == 1;
                co_return;
            });
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(first_await_cancelled);
    EXPECT_FALSE(shield_reported_cancelled);
    EXPECT_TRUE(cleanup_completed);
}

// ---------------------------------------------------------------------------
// nested scope propagation
// ---------------------------------------------------------------------------

TEST(CancelScope, with_cancel_scope_NestedScope_OuterCancelPropagatesToInner) {
    bool inner_sleep_cancelled = false;
    bool inner_reported_cancelled = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};

    auto task = [&inner_sleep_cancelled, &inner_reported_cancelled]() -> coro::Task<void> {
        [[maybe_unused]] const auto ignored = co_await coro::with_timeout(
            10ms, [&inner_sleep_cancelled, &inner_reported_cancelled](coro::CancelScope&) -> coro::Task<void> {
                auto inner =
                    co_await coro::with_cancel_scope([&inner_sleep_cancelled](coro::CancelScope&) -> coro::Task<void> {
                        const auto slept = co_await coro::sleep_for(1s);
                        inner_sleep_cancelled = !slept.has_value();
                        co_return;
                    });
                inner_reported_cancelled = inner.cancelled;
                co_return;
            });
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(inner_sleep_cancelled);
    EXPECT_TRUE(inner_reported_cancelled);
}

TEST(CancelScope, non_cancellable_UnderCancelledScope_StillCompletes) {
    bool inner_completed = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};

    auto task = [&inner_completed]() -> coro::Task<void> {
        [[maybe_unused]] const auto ignored =
            co_await coro::with_timeout(10ms, [&inner_completed](coro::CancelScope&) -> coro::Task<void> {
                [[maybe_unused]] const auto slept = co_await coro::sleep_for(1s);  // cancelled here
                auto shielded =
                    co_await coro::non_cancellable([&inner_completed](coro::CancelScope&) -> coro::Task<void> {
                        const auto slept = co_await coro::sleep_for(20ms);
                        inner_completed = slept.has_value();
                        co_return;
                    });
                EXPECT_TRUE(shielded.completed);
                co_return;
            });
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(inner_completed);
}

// ---------------------------------------------------------------------------
// sleep: cancellation, the zero-delay path and deadline accuracy
// ---------------------------------------------------------------------------

TEST(Sleep, sleep_for_InsideCancelledScope_ReturnsOperationCanceled) {
    bool cancelled = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    const auto start = clock.now();

    auto task = [&cancelled]() -> coro::Task<void> {
        [[maybe_unused]] const auto ignored =
            co_await coro::with_cancel_scope([&cancelled](coro::CancelScope& scope) -> coro::Task<void> {
                scope.cancel();
                const auto slept = co_await coro::sleep_for(1s);
                cancelled = !slept.has_value();
                co_return;
            });
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(clock.now(), start);  // the cancelled sleep never armed a timer
}

TEST(Sleep, sleep_for_ZeroDelay_DoesNotAdvanceClock) {
    bool slept = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    const auto start = clock.now();

    auto task = [&slept]() -> coro::Task<void> {
        const auto result = co_await coro::sleep_for(0ms);
        slept = result.has_value();
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(slept);
    EXPECT_EQ(clock.now(), start);
}

TEST(Sleep, sleep_for_SequentialDelays_WakesInOrderAndSumsElapsed) {
    std::vector<int> order;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    const auto start = clock.now();

    auto task = [&order]() -> coro::Task<void> {
        const auto first = co_await coro::sleep_for(100ms);
        EXPECT_TRUE(first.has_value());
        order.push_back(1);
        const auto second = co_await coro::sleep_for(50ms);
        EXPECT_TRUE(second.has_value());
        order.push_back(2);
        co_return;
    };

    coro::run(loop, task());
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
    EXPECT_EQ(clock.now() - start, 150ms);
}

TEST(Sleep, sleep_until_AbsoluteDeadline_WakesAtDeadline) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    const auto target = clock.now() + 25ms;
    bool reached = false;

    auto task = [&reached, &clock, target]() -> coro::Task<void> {
        const auto result = co_await coro::sleep_until(target);
        reached = result.has_value() && clock.now() >= target;
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(reached);
    EXPECT_GE(clock.now(), target);
}

}  // namespace
