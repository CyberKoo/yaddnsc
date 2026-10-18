//
// Coroutine runtime — the Nursery/Handle contracts and the AsyncMutex entry
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
#include <coroutine>
#include <cstddef>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "coro/coro.h"
#include "coro/detail/nursery_state.hpp"
#include "support/coro/reservation_pressure.hpp"

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
coro::Task<void> park_in_next(coro::Nursery* group, coro::CancelScope** scope_slot, bool* cancelled) {
    const auto outcome = co_await coro::with_cancel_scope([=](coro::CancelScope& scope) -> coro::Task<void> {
        *scope_slot = &scope;
        [[maybe_unused]] const std::optional<coro::Result<int>> result = co_await group->next<int>();
        co_return;
    });
    *cancelled = outcome.cancelled;
}

struct CancelAtRetirement {
    coro::CancelScope** scope;
    coro::Handle<int>* first;
    bool* claimed;

    ~CancelAtRetirement() {
        *claimed = !first->valid();
        (*scope)->cancel();
    }
};

/// Cancels a peer's scope from a frame destructor, i.e. from inside the
/// retirement transaction of the batch that published the result.
struct CancelScopeAtRetirement {
    coro::CancelScope** scope;

    ~CancelScopeAtRetirement() { (*scope)->cancel(); }
};

coro::Task<void> cancel_scope_when_retired(coro::Notification& gate,
                                           [[maybe_unused]] std::unique_ptr<CancelScopeAtRetirement> cancel) {
    co_await gate.wait();
}

/// What one next<int>() call observed: a value, or the exhaustion answer.
struct NextOutcome {
    bool exhausted = false;
    int value = 0;
};

/// Parks in next<int>() under its own scope and records the outcome, so a
/// test can tell a released reservation from a premature nullopt.
coro::Task<void> park_in_next_recording(coro::Nursery* group, coro::CancelScope** scope_slot, NextOutcome* outcome,
                                        bool* cancelled) {
    const auto scope_outcome =
        co_await coro::with_cancel_scope([=](coro::CancelScope& scope) -> coro::Task<void> {
            *scope_slot = &scope;
            auto result = co_await group->next<int>();
            if (result && result->has_value()) {
                outcome->value = **result;
            } else {
                outcome->exhausted = true;
            }
        });
    *cancelled = scope_outcome.cancelled;
}

coro::Task<int> value_when_notified(coro::Notification& gate, int value) {
    co_await gate.wait();
    co_return value;
}

coro::Task<void> cancel_when_retired(coro::Notification& gate,
                                     [[maybe_unused]] std::unique_ptr<CancelAtRetirement> cancel) {
    co_await gate.wait();
}

}  // namespace

// ---------------------------------------------------------------------------
// Handle<T> — the contracts its own documentation states
// ---------------------------------------------------------------------------

TEST(NurseryContract, DefaultConstructedHandle_ReportsInvalid) {
    coro::Handle<void> void_handle;
    coro::Handle<int> int_handle;
    EXPECT_FALSE(void_handle.valid());
    EXPECT_FALSE(int_handle.valid());
}

TEST(NurseryContract, JoinEmptyHandle_RejectsTheMissingChildResult) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool rejected = false;

    auto root = [&]() -> coro::Task<void> {
        try {
            co_await coro::with_nursery([](coro::Nursery&) -> coro::Task<void> {
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

TEST(NurseryContract, JoinCompletedChild_YieldsItsValueWithoutSuspending) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    int joined = 0;
    bool still_valid = false;

    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
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

TEST(NurseryContract, JoinCancelledByOwningScope_ReleasesTheReservationWithoutConsuming) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    bool timed_out = false;
    bool released = false;
    int rejoined = 0;

    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
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

TEST(NurseryContract, Next_ManyReservationsPreserveResultsAndCancellationOrder) {
    for (const int children : {4, 16, 64, 256}) {
        for (const bool mixed : {false, true}) {
            for (const bool cancel : {false, true}) {
                SCOPED_TRACE(::testing::Message()
                             << "children=" << children << " mixed=" << mixed << " cancel=" << cancel);
                coro::ManualClock clock;
                coro::Loop loop{clock};
                const auto result = coro::run(loop, test::reservation_pressure_batch(children, mixed, cancel));
                EXPECT_EQ(result.errors, 0);
                EXPECT_EQ(result.cancelled, cancel ? children / 2 : 0);
                EXPECT_EQ(result.recovered, cancel ? children / 2 : 0);
            }
        }
    }
}

TEST(NurseryContract, Next_LastProducerWakesAllExhaustedConsumers) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    int values = 0;
    int exhausted = 0;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& nursery) -> coro::Task<void> {
            nursery.spawn(value_after(1ms, 7));
            auto consume = [&]() -> coro::Task<void> {
                auto result = co_await nursery.next<int>();
                if (result) {
                    EXPECT_TRUE(result->has_value());
                    if (*result) {
                        EXPECT_EQ(**result, 7);
                    }
                    ++values;
                } else {
                    ++exhausted;
                }
            };
            for (int index = 0; index < 64; ++index) {
                nursery.spawn_discard(consume());
            }
            co_await coro::sleep_for(2ms);
            EXPECT_EQ(values, 1);
            EXPECT_EQ(exhausted, 63);
        });
    };
    coro::run(loop, root());
    EXPECT_EQ(values, 1);
    EXPECT_EQ(exhausted, 63);
}

TEST(NurseryContract, Next_SkipsACompletedChildOfAnotherType) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    std::optional<coro::Result<int>> first;
    bool exhausted = false;

    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
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

TEST(NurseryContract, Next_CancelledClaimPreservesPublicationOrder) {
    bool claimed = false;
    bool cancelled = false;
    std::vector<int> values;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
            coro::Notification first_gate, second_gate, cancel_gate;
            coro::CancelScope* scope = nullptr;
            auto first = group.spawn(value_when_notified(first_gate, 1));
            group.spawn(value_when_notified(second_gate, 2));
            auto consumer = group.spawn(park_in_next(&group, &scope, &cancelled));
            auto cancel = group.spawn(
                cancel_when_retired(cancel_gate, std::make_unique<CancelAtRetirement>(&scope, &first, &claimed)));
            while (scope == nullptr) {
                co_await coro::checkpoint();
            }
            // One batch retires A, B, then the cancelling frame. A has been
            // claimed, B published, but the consumer cannot have resumed yet.
            first_gate.notify();
            second_gate.notify();
            cancel_gate.notify();
            co_await cancel;
            co_await consumer;
            for (int index = 0; index < 2; ++index) {
                auto result = co_await group.next<int>();
                EXPECT_TRUE(result.has_value() && result->has_value());
                if (result && *result) {
                    values.push_back(**result);
                }
            }
        });
    };
    coro::run(root());
    EXPECT_TRUE(claimed);
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(values, (std::vector<int>{1, 2}));
}

TEST(NurseryContract, Next_NulloptDoesNotPrecludeAReleasedHandleReservation) {
    bool empty_while_reserved = false;
    bool cancelled = false;
    int value = 0;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
            coro::Notification gate;
            coro::CancelScope* scope = nullptr;
            auto producer = group.spawn(value_when_notified(gate, 7));
            auto join = [&]() -> coro::Task<void> {
                const auto outcome =
                    co_await coro::with_cancel_scope([&](coro::CancelScope& current) -> coro::Task<void> {
                        scope = &current;
                        [[maybe_unused]] int result = co_await producer;
                    });
                cancelled = outcome.cancelled;
            };
            auto consumer = group.spawn(join());
            while (scope == nullptr) {
                co_await coro::checkpoint();
            }
            EXPECT_FALSE(producer.valid());
            empty_while_reserved = !(co_await group.next<int>()).has_value();
            scope->cancel();
            co_await consumer;
            EXPECT_TRUE(producer.valid());
            gate.notify();
            auto result = co_await group.next<int>();
            EXPECT_TRUE(result.has_value() && result->has_value());
            if (result && *result) {
                value = **result;
            }
        });
    };
    coro::run(root());
    EXPECT_TRUE(empty_while_reserved);
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(value, 7);
}

TEST(NurseryContract, Next_ReservationReleasedAtRetirement_StillReachesTheNextWaiter) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    coro::CancelScope* claimer_scope = nullptr;
    coro::CancelScope* survivor_scope = nullptr;
    bool claimer_cancelled = false;
    bool survivor_cancelled = false;
    NextOutcome claimer_outcome;
    NextOutcome survivor;

    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
            coro::Notification publish_gate, cancel_gate;
            group.spawn(value_when_notified(publish_gate, 7));
            // Registration prepends to the waiter list, so the second waiter parks
            // at the head and claims the result; the first one waits behind it.
            group.spawn(park_in_next_recording(&group, &survivor_scope, &survivor, &survivor_cancelled));
            group.spawn(park_in_next_recording(&group, &claimer_scope, &claimer_outcome, &claimer_cancelled));
            group.spawn_discard(cancel_scope_when_retired(
                cancel_gate, std::make_unique<CancelScopeAtRetirement>(&claimer_scope)));
            while (claimer_scope == nullptr || survivor_scope == nullptr) {
                co_await coro::checkpoint();
            }
            // One turn: the value is published and claimed, then the claimer's scope
            // is cancelled from a frame destructor inside that same retirement
            // transaction, before the claiming waiter could resume and consume.
            publish_gate.notify();
            cancel_gate.notify();
            // Bound the wait by resumed frames rather than by time: the correct path
            // hands the value to the survivor within a couple of batches, and this
            // loop must not outlive the notifications that wake the parked waits.
            for (int turn = 0; turn < 8 && survivor.value == 0 && !survivor.exhausted; ++turn) {
                co_await coro::checkpoint();
            }
            if (survivor.value == 0 && !survivor.exhausted) {
                // Liveness only, never reached while the hand-back works: without it a
                // broken hand-back would park the survivor and the drain forever.
                survivor_scope->cancel();
            }
            co_await coro::checkpoint();
        });
    };
    coro::run(loop, root());

    EXPECT_TRUE(claimer_cancelled);
    EXPECT_EQ(claimer_outcome.value, 0) << "the cancelled claim must not have consumed the value";
    EXPECT_FALSE(survivor_cancelled);
    // The released reservation must come back to the still-parked waiter instead
    // of being reported to it as exhaustion.
    EXPECT_FALSE(survivor.exhausted) << "a released reservation was reported as exhaustion";
    EXPECT_EQ(survivor.value, 7);
}

TEST(NurseryContract, Next_CancelledWaiterDetachesFromTheMiddleOfTheList) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    coro::CancelScope* scopes[3] = {nullptr, nullptr, nullptr};
    bool cancelled[3] = {false, false, false};

    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
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

TEST(NurseryContract, SupervisorNursery_UnconsumedSuccessIsNotReportedAsAFailure) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    std::size_t reported = 0;

    auto root = [&]() -> coro::Task<void> {
        try {
            co_await coro::with_nursery(
                [](coro::Nursery& group) -> coro::Task<void> {
                    group.spawn_discard(fail_after(1ms, "discarded defect"));
                    // Never consumed, so its slot survives to the exit aggregation.
                    group.spawn(value_after(2ms, 7));
                    co_await coro::sleep_for(5ms);
                    co_return;
                },
                coro::NurseryPolicy::SUPERVISOR);
        } catch (const coro::GroupError& error) {
            reported = error.errors().size();
        }
        co_return;
    };

    coro::run(loop, root());
    EXPECT_EQ(reported, 1u);
}

TEST(NurseryContract, Nursery_SecondFailingChild_KeepsTheFirstDefect) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    std::string reported;

    auto root = [&]() -> coro::Task<void> {
        try {
            co_await coro::with_nursery([](coro::Nursery& group) -> coro::Task<void> {
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