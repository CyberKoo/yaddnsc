//
// Coroutine runtime — cancellation combinators and sleep cancellation.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <atomic>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <type_traits>
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
    bool outcome_reported_timeout = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    const auto start = clock.now();

    auto task = [&body_saw_cancellation, &outcome_reported_timeout]() -> coro::Task<void> {
        auto outcome = co_await coro::with_timeout(100ms, [&body_saw_cancellation]() -> coro::Task<void> {
            try {
                co_await coro::sleep_for(10s);
            } catch (const coro::Cancelled&) {
                body_saw_cancellation = true;
                throw;
            }
            co_return;
        });
        outcome_reported_timeout = outcome.timed_out;
        // The combinator returns normally even though the scope was cancelled.
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(body_saw_cancellation);
    EXPECT_TRUE(outcome_reported_timeout);
    EXPECT_EQ(clock.now() - start, 100ms);
}

TEST(CancelScope, with_timeout_DeadlineFires_DoesNotLeakToOuterScope) {
    bool after_timeout_ran = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};

    auto task = [&after_timeout_ran]() -> coro::Task<void> {
        [[maybe_unused]] const auto ignored = co_await coro::with_timeout(10ms, []() -> coro::Task<void> {
            co_await coro::sleep_for(1s);
            co_return;
        });
        // If the timeout had propagated, this sleep would be cancelled too.
        co_await coro::sleep_for(50ms);
        after_timeout_ran = true;
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
        auto outcome = co_await coro::with_timeout(10s, []() -> coro::Task<int> {
            co_await coro::sleep_for(100ms);
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
        auto outcome = co_await coro::with_deadline(deadline, []() -> coro::Task<void> {
            co_await coro::sleep_for(5s);
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
        auto outcome = co_await coro::with_deadline(clock.now() + 100ms, []() -> coro::Task<void> {
            co_await coro::sleep_for(10ms);
            co_return;
        });
        completed = outcome.completed;
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(completed);
}

TEST(CancelScope, WithTimeout_BodyDefect_PropagatesAndRemovesTimer) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool caught = false;
    auto task = [&caught]() -> coro::Task<void> {
        try {
            co_await coro::with_timeout(10ms, []() -> coro::Task<void> {
                co_await coro::sleep_for(1ms);
                throw std::runtime_error("body defect");
            });
        } catch (const std::runtime_error&) {
            caught = true;
        }
        // Passing the former deadline must not cancel the caller.
        co_await coro::sleep_for(20ms);
    };
    coro::run(loop, task());
    EXPECT_TRUE(caught);
    EXPECT_EQ(clock.now().time_since_epoch(), 21ms);
}

TEST(CancelScope, WithDeadline_AncestorDeadlineFires_PropagatesThroughInnerScope) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool inner_returned = false;
    auto task = [&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_deadline(clock.now() + 10ms, [&]() -> coro::Task<void> {
            co_await coro::with_deadline(clock.now() + 100ms,
                                         []() -> coro::Task<void> { co_await coro::sleep_for(1s); });
            inner_returned = true;
        });
        EXPECT_TRUE(outcome.timed_out);
        EXPECT_TRUE(outcome.cancelled);
        EXPECT_FALSE(outcome.completed);
    };
    coro::run(loop, task());
    EXPECT_FALSE(inner_returned);
    EXPECT_EQ(clock.now().time_since_epoch(), 10ms);
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
                try {
                    co_await coro::sleep_for(10s);

                } catch (const coro::Cancelled&) {
                    victim_cancelled = true;

                    throw;
                }
                co_return;
            });
        EXPECT_FALSE(outcome.timed_out);
        co_return;
    };
    auto canceller = [&canceller_saw_scope, &victim_scope]() -> coro::Task<void> {
        co_await coro::sleep_for(5ms);
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
            10ms, [&first_await_cancelled, &cleanup_completed, &shield_reported_cancelled]() -> coro::Task<void> {
                try {
                    co_await coro::sleep_for(1s);

                } catch (const coro::Cancelled&) {
                    first_await_cancelled = true;
                }
                // Cleanup runs under a shield: the outer cancellation must not
                // reach it, so this sleep completes normally.
                auto shielded = co_await coro::non_cancellable(
                    [&shield_reported_cancelled](coro::CancelScope& shield) -> coro::Task<int> {
                        shield_reported_cancelled = shield.cancelled();
                        co_await coro::sleep_for(50ms);
                        co_return 1;
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
            10ms, [&inner_sleep_cancelled, &inner_reported_cancelled]() -> coro::Task<void> {
                auto inner =
                    co_await coro::with_cancel_scope([&inner_sleep_cancelled](coro::CancelScope&) -> coro::Task<void> {
                        try {
                            co_await coro::sleep_for(1s);

                        } catch (const coro::Cancelled&) {
                            inner_sleep_cancelled = true;

                            throw;
                        }
                        co_return;
                    });
                inner_reported_cancelled = inner.cancelled;
                co_return;
            });
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(inner_sleep_cancelled);
    EXPECT_FALSE(inner_reported_cancelled);  // ancestor cancellation never returns an inner outcome
}

TEST(CancelScope, non_cancellable_UnderCancelledScope_StillCompletes) {
    bool inner_completed = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};

    auto task = [&inner_completed]() -> coro::Task<void> {
        [[maybe_unused]] const auto ignored =
            co_await coro::with_timeout(10ms, [&inner_completed]() -> coro::Task<void> {
                try {
                    co_await coro::sleep_for(1s);
                } catch (const coro::Cancelled&) {
                }
                auto shielded =
                    co_await coro::non_cancellable([&inner_completed](coro::CancelScope&) -> coro::Task<void> {
                        co_await coro::sleep_for(20ms);
                        inner_completed = true;
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

TEST(Sleep, sleep_for_InsideCancelledScope_ThrowsCancelled) {
    bool cancelled = false;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    const auto start = clock.now();

    auto task = [&cancelled]() -> coro::Task<void> {
        [[maybe_unused]] const auto ignored =
            co_await coro::with_cancel_scope([&cancelled](coro::CancelScope& scope) -> coro::Task<void> {
                scope.cancel();
                try {
                    co_await coro::sleep_for(1s);

                } catch (const coro::Cancelled&) {
                    cancelled = true;

                    throw;
                }
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
        co_await coro::sleep_for(0ms);
        slept = true;
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
        co_await coro::sleep_for(100ms);
        order.push_back(1);
        co_await coro::sleep_for(50ms);
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
        co_await coro::sleep_until(target);
        reached = clock.now() >= target;
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(reached);
    EXPECT_GE(clock.now(), target);
}

// ---------------------------------------------------------------------------
// loop scheduling fairness
// ---------------------------------------------------------------------------

TEST(CancelScope, with_timeout_BusyReadyQueue_TimerStillFires) {
    // A self-re-filling ready queue must not starve the timer heap: every
    // co_await below enqueues a child frame and every completion enqueues the
    // continuation, so a loop that only fires timers once the ready queue is
    // empty would report this 1ms timeout tens of milliseconds late — after
    // the churn ends. The timeout must win while the churn still runs.
    coro::Loop loop;
    constexpr int churn_steps = 100000;
    int churned = 0;
    int churned_when_timed_out = -1;
    bool timed_out = false;

    auto churn = [&churned]() -> coro::Task<void> {
        for (int i = 0; i < churn_steps; ++i) {
            co_await []() -> coro::Task<void> { co_return; }();
            ++churned;
        }
        co_return;
    };

    auto task = [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            group.spawn(churn());
            const auto outcome = co_await coro::with_timeout(1ms, []() -> coro::Task<void> {
                co_await coro::sleep_for(10s);
                co_return;
            });
            timed_out = outcome.timed_out;
            churned_when_timed_out = churned;
            group.cancel();
            co_return;
        });
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(timed_out);
    // The timer fired mid-churn, not after the ready queue finally drained.
    EXPECT_GE(churned_when_timed_out, 0);
    EXPECT_LT(churned_when_timed_out, churn_steps);
}

TEST(CancelScope, Cancelled_IsControlExceptionOutsideStdException) {
    EXPECT_FALSE((std::is_base_of_v<std::exception, coro::Cancelled>) );
    EXPECT_FALSE((std::is_default_constructible_v<coro::Cancelled>) );
}

TEST(CancelScope, Cancellation_RemainsStickyAfterItIsCaught) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    int throws = 0;
    bool completed = true;
    coro::run(loop, [&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
            scope.cancel();
            for (int i = 0; i < 3; ++i) {
                try {
                    co_await coro::checkpoint();
                } catch (const coro::Cancelled&) {
                    ++throws;
                }
            }
        });
        completed = outcome.completed;
    }());
    EXPECT_EQ(throws, 3);
    EXPECT_FALSE(completed);
}

TEST(CancelScope, LocalAndAncestorCancel_OnlyAncestorAbsorbs) {
    bool inner_returned = false;
    bool outer_cancelled = false;
    coro::run([&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& outer) -> coro::Task<void> {
            co_await coro::with_cancel_scope([&](coro::CancelScope& inner) -> coro::Task<void> {
                inner.cancel();
                outer.cancel();
                co_await coro::checkpoint();
            });
            inner_returned = true;
        });
        outer_cancelled = outcome.cancelled && !outcome.completed;
    }());
    EXPECT_FALSE(inner_returned);
    EXPECT_TRUE(outer_cancelled);
}

TEST(CancelScope, LocalExceptionCaughtBeforeAncestorCancel_IsReattributedAtExit) {
    bool inner_returned = false;
    coro::run([&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& outer) -> coro::Task<void> {
            co_await coro::with_cancel_scope([&](coro::CancelScope& inner) -> coro::Task<void> {
                inner.cancel();
                try {
                    co_await coro::checkpoint();
                } catch (const coro::Cancelled&) {
                    outer.cancel();
                    throw;
                }
            });
            inner_returned = true;
        });
        EXPECT_TRUE(outcome.cancelled);
    }());
    EXPECT_FALSE(inner_returned);
}

TEST(Offload, AlreadyCancelled_DoesNotStartWorker) {
    std::atomic<bool> started{false};
    coro::run([&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
            scope.cancel();
            co_await coro::offload([&] { started.store(true); });
        });
        EXPECT_FALSE(outcome.completed);
    }());
    EXPECT_FALSE(started.load());
}

}  // namespace
