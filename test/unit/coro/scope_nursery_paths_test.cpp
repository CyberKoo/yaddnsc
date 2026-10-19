//
// Coroutine runtime — scope-combinator and nursery paths that the main suite does
// not reach: every combinator with a non-void result, deadline arms, the
// body-defect and body-cancellation exits of a nursery body, supervised
// aggregation, and the empty-Handle contract.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <expected>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "coro/coro.h"

namespace {

using namespace std::chrono_literals;

coro::Task<int> value_after(coro::Duration delay, int value) {
    co_await coro::sleep_for(delay);
    co_return value;
}

coro::Task<void> throw_after(coro::Duration delay, const char* what) {
    co_await coro::sleep_for(delay);
    throw std::runtime_error{what};
}

coro::Task<void> sleep_then_flag(coro::Duration delay, bool& flag) {
    co_await coro::sleep_for(delay);
    flag = true;
    co_return;
}

}  // namespace

// ---------------------------------------------------------------------------
// Handle<T> — the empty contract
// ---------------------------------------------------------------------------

TEST(ScopeNurseryPaths, Handle_DefaultConstructed_ReportsInvalid) {
    coro::Handle<void> void_handle;
    coro::Handle<int> int_handle;
    EXPECT_FALSE(void_handle.valid());
    EXPECT_FALSE(int_handle.valid());
}

// ---------------------------------------------------------------------------
// Combinators with a non-void body
// ---------------------------------------------------------------------------

TEST(ScopeNurseryPaths, WithTimeout_ValueBody_CompletesWithValue) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool has_value = false;
    bool timed_out = true;
    int value = 0;

    auto root = [&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_timeout(1s, []() -> coro::Task<int> {
            return value_after(5ms, 21);
        });
        has_value = outcome.has_value();
        timed_out = outcome.timed_out;
        if (outcome.has_value()) {
            value = *outcome * 2;
        }
        co_return;
    };
    coro::run(loop, root());
    EXPECT_TRUE(has_value);
    EXPECT_FALSE(timed_out);
    EXPECT_EQ(value, 42);
}

TEST(ScopeNurseryPaths, WithTimeout_ValueBodyExpires_AbsorbsWithoutValue) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool has_value = true;
    bool timed_out = false;

    auto root = [&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_timeout(1ms, []() -> coro::Task<int> {
            return value_after(1h, 1);
        });
        has_value = outcome.has_value();
        timed_out = outcome.timed_out;
        co_return;
    };
    coro::run(loop, root());
    EXPECT_FALSE(has_value);
    EXPECT_TRUE(timed_out);
}

TEST(ScopeNurseryPaths, WithDeadline_ValueBody_CompletesWithValue) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool has_value = false;
    int value = 0;

    auto root = [&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_deadline(clock.now() + 1s, []() -> coro::Task<int> {
            return value_after(5ms, 7);
        });
        has_value = outcome.has_value();
        if (outcome.has_value()) {
            value = *outcome;
        }
        co_return;
    };
    coro::run(loop, root());
    EXPECT_TRUE(has_value);
    EXPECT_EQ(value, 7);
}

TEST(ScopeNurseryPaths, WithCancelScope_ValueBody_CompletesWithValue) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool has_value = false;
    bool cancelled = true;
    int value = 0;

    auto root = [&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_cancel_scope([](coro::CancelScope&) -> coro::Task<int> {
            return value_after(1ms, 3);
        });
        has_value = outcome.has_value();
        cancelled = outcome.cancelled;
        if (outcome.has_value()) {
            value = *outcome;
        }
        co_return;
    };
    coro::run(loop, root());
    EXPECT_TRUE(has_value);
    EXPECT_FALSE(cancelled);
    EXPECT_EQ(value, 3);
}

TEST(ScopeNurseryPaths, NonCancellable_ValueBody_CompletesWithValue) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool has_value = false;
    int value = 0;

    auto root = [&]() -> coro::Task<void> {
        const auto outcome = co_await coro::non_cancellable([](coro::CancelScope&) -> coro::Task<int> {
            return value_after(1ms, 11);
        });
        has_value = outcome.has_value();
        if (outcome.has_value()) {
            value = *outcome;
        }
        co_return;
    };
    coro::run(loop, root());
    EXPECT_TRUE(has_value);
    EXPECT_EQ(value, 11);
}

// ---------------------------------------------------------------------------
// Combinators — body defect and body-initiated cancellation
// ---------------------------------------------------------------------------

TEST(ScopeNurseryPaths, WithCancelScope_BodyDefect_Propagates) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool caught = false;

    auto root = [&]() -> coro::Task<void> {
        try {
            co_await coro::with_cancel_scope([](coro::CancelScope&) -> coro::Task<void> {
                return throw_after(1ms, "value-body defect");
            });
        } catch (const std::runtime_error&) {
            caught = true;
        }
        co_return;
    };
    coro::run(loop, root());
    EXPECT_TRUE(caught);
}

TEST(ScopeNurseryPaths, NonCancellable_BodyDefect_Propagates) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool caught = false;

    auto root = [&]() -> coro::Task<void> {
        try {
            co_await coro::non_cancellable(
                [](coro::CancelScope&) -> coro::Task<void> { return throw_after(1ms, "shield defect"); });
        } catch (const std::runtime_error&) {
            caught = true;
        }
        co_return;
    };
    coro::run(loop, root());
    EXPECT_TRUE(caught);
}

TEST(ScopeNurseryPaths, WithCancelScope_BodyCancelsItsOwnScope_ReportsCancelled) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool cancelled = false;
    bool completed = true;

    auto root = [&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_cancel_scope([](coro::CancelScope& scope) -> coro::Task<void> {
            scope.cancel();
            try {
                co_await coro::sleep_for(1h);
            } catch (const coro::Cancelled&) {
            }
            co_return;
        });
        cancelled = outcome.cancelled;
        completed = outcome.completed;
        co_return;
    };
    coro::run(loop, root());
    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(completed);
}

// ---------------------------------------------------------------------------
// Group bodies — defect and cancellation exits
// ---------------------------------------------------------------------------

TEST(ScopeNurseryPaths, Nursery_BodyThrows_PropagatesBodyDefect) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool caught = false;
    bool child_ran = false;

    auto root = [&]() -> coro::Task<void> {
        try {
            co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
                group.spawn(sleep_then_flag(5ms, child_ran));
                return throw_after(1ms, "body defect");
            });
        } catch (const std::runtime_error&) {
            caught = true;
        }
        co_return;
    };
    coro::run(loop, root());
    EXPECT_TRUE(caught);
    EXPECT_FALSE(child_ran);
}

TEST(ScopeNurseryPaths, Nursery_BodyCancelled_PropagatesCancellation) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool cancelled = false;

    auto root = [&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_cancel_scope([](coro::CancelScope& scope) -> coro::Task<void> {
            // Cancel from a sibling child while the nursery body is parked on the drain.
            struct CancelOnFirstTurn {
                coro::CancelScope* scope;
                [[nodiscard]] coro::Task<void> run() const {
                    co_await coro::sleep_for(1ms);
                    scope->cancel();
                    co_return;
                }
            };
            const CancelOnFirstTurn canceller{&scope};
            // A nested nursery waits for its child, so the sibling cancellation lands first.
            co_await coro::with_nursery([&canceller](coro::Nursery& group) -> coro::Task<void> {
                group.spawn(canceller.run());
                co_return;
            });
        });
        cancelled = outcome.cancelled;
        co_return;
    };
    coro::run(loop, root());
    EXPECT_TRUE(cancelled);
}

TEST(ScopeNurseryPaths, SupervisorNursery_UnobservedChildFailure_IsReported) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool reported = false;

    auto root = [&]() -> coro::Task<void> {
        try {
            co_await coro::with_nursery(
                [](coro::Nursery& group) -> coro::Task<void> {
                    group.spawn_discard(throw_after(1ms, "discarded child"));
                    co_return;
                },
                coro::NurseryPolicy::SUPERVISOR);
        } catch (const coro::GroupError&) {
            reported = true;
        }
        co_return;
    };
    coro::run(loop, root());
    EXPECT_TRUE(reported);
}

TEST(ScopeNurseryPaths, SupervisorNursery_BodyDefectAfterChildFailure_ReportsBoth) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    std::size_t reported = 0;

    auto root = [&]() -> coro::Task<void> {
        try {
            co_await coro::with_nursery(
                [](coro::Nursery& group) -> coro::Task<void> {
                    group.spawn_discard(throw_after(1ms, "child defect"));
                    return throw_after(2ms, "body defect");
                },
                coro::NurseryPolicy::SUPERVISOR);
        } catch (const coro::GroupError& error) {
            reported = error.errors().size();
        }
        co_return;
    };
    coro::run(loop, root());
    EXPECT_EQ(reported, 2u);
}

TEST(ScopeNurseryPaths, SupervisorNursery_CancelledBodyWithChildFailure_ReportsChildOnly) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    std::size_t reported = 0;
    bool saw_cancellation = false;

    struct CancelSoon {
        coro::Nursery* group;

        [[nodiscard]] coro::Task<void> run() const {
            co_await coro::sleep_for(2ms);
            group->cancel();
            co_return;
        }
    };

    auto root = [&]() -> coro::Task<void> {
        try {
            co_await coro::with_nursery(
                [&](coro::Nursery& group) -> coro::Task<void> {
                    group.spawn_discard(throw_after(1ms, "child defect"));
                    const CancelSoon canceller{&group};
                    group.spawn_discard(canceller.run());
                    co_await coro::sleep_for(1h);
                    co_return;
                },
                coro::NurseryPolicy::SUPERVISOR);
        } catch (const coro::GroupError& error) {
            reported = error.errors().size();
        } catch (const coro::Cancelled&) {
            saw_cancellation = true;
        }
        co_return;
    };
    coro::run(loop, root());
    EXPECT_FALSE(saw_cancellation);
    EXPECT_EQ(reported, 1u);
}

TEST(ScopeNurseryPaths, SupervisorNursery_CleanBodyWithValueChildren_Completes) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    int sum = 0;

    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                group.spawn(value_after(1ms, 1));
                group.spawn(value_after(2ms, 2));
                while (auto result = co_await group.next<int>()) {
                    if (result->has_value()) {
                        sum += **result;
                    }
                }
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
        co_return;
    };
    coro::run(loop, root());
    EXPECT_EQ(sum, 3);
}

TEST(ScopeNurseryPaths, Nursery_RunsBodyThenDrains) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool child_done = false;
    bool body_done = false;

    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
            group.spawn(sleep_then_flag(1ms, child_done));
            body_done = true;
            co_return;
        });
        co_return;
    };
    coro::run(loop, root());
    EXPECT_TRUE(body_done);
    EXPECT_TRUE(child_done);
}

TEST(ScopeNurseryPaths, Nursery_DefectDrainsBeforeRethrow) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool child_done = false;
    bool caught = false;

    auto root = [&]() -> coro::Task<void> {
        try {
            co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
                group.spawn(sleep_then_flag(1ms, child_done));
                co_await throw_after(2ms, "nursery body defect");
            });
        } catch (const std::runtime_error&) {
            caught = true;
        }
        co_return;
    };
    coro::run(loop, root());
    EXPECT_TRUE(caught);
    EXPECT_TRUE(child_done);
}
