#include <atomic>
#include <chrono>
#include <coroutine>
#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>

#include "coro/coro.h"
#include "coro/detail/wait_operation.h"
#include "coro/host.hpp"

namespace {

void count_notification(void* context) noexcept {
    ++*static_cast<int*>(context);
}

struct DisarmContext {
    coro::CancelObserver& other;
    int& calls;
};

void disarm_other(void* context) noexcept {
    auto& target = *static_cast<DisarmContext*>(context);
    ++target.calls;
    target.other.disarm();
}

TEST(CoroWaitProtocol, CancellationCallback_DisarmsSuccessorWithoutLosingOlderSubscription) {
    int a = 0, b = 0, c = 0;
    coro::CancelScope scope;
    coro::CancelObserver older{scope, &c, count_notification};
    coro::CancelObserver middle{scope, &b, count_notification};
    DisarmContext context{middle, a};
    coro::CancelObserver newer{scope, &context, disarm_other};
    scope.cancel();
    scope.cancel();
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 0);
    EXPECT_EQ(c, 1);
    EXPECT_FALSE(older.armed());
    EXPECT_FALSE(middle.armed());
    EXPECT_FALSE(newer.armed());
}

TEST(CoroWaitProtocol, AlreadyCancelledObserver_NotifiesInlineOnceWithoutArming) {
    int calls = 0;
    coro::CancelScope scope;
    scope.cancel();
    {
        coro::CancelObserver observer{scope, &calls, count_notification};
        EXPECT_EQ(calls, 1);
        EXPECT_FALSE(observer.armed());
        observer.disarm();
        observer.disarm();
    }
    EXPECT_EQ(calls, 1);
}

struct ArbitrationWait {
    coro::CancelScope& scope;
    int& cancellations;
    coro::detail::CancelSubscription subscription{};
    coro::detail::WaitOperation operation{subscription};
    bool fail_registration = false;

    ArbitrationWait(coro::CancelScope& target, int& calls, bool fail = false)
        : scope(target), cancellations(calls), fail_registration(fail) {}

    bool await_ready() const noexcept { return false; }

    template<typename Promise>
    void await_suspend(std::coroutine_handle<Promise> frame) {
        operation.begin(*frame.promise().exec);
        operation.subscribe_cancel(scope, this, [](void* context) noexcept {
            ++static_cast<ArbitrationWait*>(context)->cancellations;
            return coro::detail::CancelAction::RESUME;
        });
        operation.event_ready();
        operation.event_ready();  // ARMING must not enqueue either notification yet.
        if (fail_registration) {
            operation.rollback();
            throw std::runtime_error("registration failed after early completion");
        }
        operation.commit_suspend();
        scope.cancel();  // QUEUED cancellation still cleans up, but cannot requeue.
        operation.event_ready();
    }

    void await_resume() noexcept {
        operation.begin_resume();
        operation.finish();
    }
};

TEST(CoroWaitProtocol, EarlyEventThenQueuedCancellation_ResumesExactlyOnce) {
    int cancellations = 0;
    int resumes = 0;
    const auto outcome = coro::run(coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
        co_await ArbitrationWait{scope, cancellations};
        ++resumes;
    }));
    EXPECT_TRUE(outcome.cancelled);
    EXPECT_EQ(cancellations, 1);
    EXPECT_EQ(resumes, 1);
}

TEST(CoroWaitProtocol, RegistrationFailureAfterEarlyEvent_RollsBackWithoutQueuedResume) {
    int cancellations = 0;
    bool failed = false;
    const auto outcome = coro::run(coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
        try {
            co_await ArbitrationWait{scope, cancellations, true};
        } catch (const std::runtime_error&) {
            failed = true;
        }
        co_await coro::checkpoint();  // A stale queued resume would violate record membership.
        scope.cancel();
    }));
    EXPECT_TRUE(outcome.cancelled);
    EXPECT_TRUE(failed);
    EXPECT_EQ(cancellations, 0);
}

TEST(CoroWaitProtocol, MutexGrantedThenCancelled_ReturnsLockBeforeThrowing) {
    coro::run([]() -> coro::Task<void> {
        coro::AsyncMutex mutex;
        coro::Notification armed;
        coro::CancelScope* waiter_scope = nullptr;
        bool cancelled = false;
        bool acquired = false;
        auto held = co_await mutex.lock();
        co_await coro::with_nursery([&](coro::Nursery& nursery) -> coro::Task<void> {
            auto waiter = nursery.spawn(coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                waiter_scope = &scope;
                armed.notify();
                try {
                    auto guard = co_await mutex.lock();
                    acquired = true;
                } catch (const coro::Cancelled&) {
                    cancelled = true;
                    throw;
                }
            }));
            co_await armed.wait();
            held.unlock();           // The mutex has handed its lock to the parked waiter.
            waiter_scope->cancel();  // Cancel before that waiter resumes.
            co_await waiter;
            EXPECT_TRUE(cancelled);
            EXPECT_FALSE(acquired);
            auto next = co_await mutex.lock();
            EXPECT_TRUE(static_cast<bool>(next));
        });
    }());
}

TEST(CoroWaitProtocol, NotificationQueuedThenCancelled_PreservesPendingNotification) {
    coro::run([]() -> coro::Task<void> {
        coro::Notification notification, armed;
        coro::CancelScope* waiter_scope = nullptr;
        bool cancelled = false;
        co_await coro::with_nursery([&](coro::Nursery& nursery) -> coro::Task<void> {
            auto waiter = nursery.spawn(coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                waiter_scope = &scope;
                armed.notify();
                try {
                    co_await notification.wait();
                } catch (const coro::Cancelled&) {
                    cancelled = true;
                    throw;
                }
            }));
            co_await armed.wait();
            notification.notify();
            waiter_scope->cancel();
            co_await waiter;
            EXPECT_TRUE(cancelled);
            co_await notification.wait();
        });
    }());
}

TEST(CoroWaitProtocol, CancelledHandleReservation_CanBeConsumedByAnotherWait) {
    coro::run([]() -> coro::Task<void> {
        coro::Notification produce, armed;
        coro::CancelScope* waiter_scope = nullptr;
        bool cancelled = false;
        co_await coro::with_nursery([&](coro::Nursery& nursery) -> coro::Task<void> {
            auto producer = nursery.spawn([](coro::Notification& ready) -> coro::Task<int> {
                co_await ready.wait();
                co_return 42;
            }(produce));
            auto waiter = nursery.spawn(coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                waiter_scope = &scope;
                armed.notify();
                try {
                    [[maybe_unused]] const auto result = co_await producer;
                } catch (const coro::Cancelled&) {
                    cancelled = true;
                    throw;
                }
            }));
            co_await armed.wait();
            EXPECT_FALSE(producer.valid());  // reserved, but not consumed
            waiter_scope->cancel();
            co_await waiter;
            EXPECT_TRUE(cancelled);
            EXPECT_TRUE(producer.valid());
            produce.notify();
            EXPECT_EQ(co_await producer, 42);
        });
    }());
}

struct WorkerGate {
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> capture_destroyed{false};

    void unblock() noexcept {
        release.store(true, std::memory_order_release);
        release.notify_one();
    }
};

TEST(CoroWaitProtocol, Abandon_RetiresAwaitingFrameBeforeWorkerCompletion) {
    auto worker = std::make_shared<WorkerGate>();
    coro::run([&worker]() -> coro::Task<void> {
        coro::CancelScope* waiter_scope = nullptr;
        bool cancelled = false;
        co_await coro::with_nursery([&](coro::Nursery& nursery) -> coro::Task<void> {
            auto waiter = nursery.spawn(coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                waiter_scope = &scope;
                try {
                    co_await coro::offload([worker] {
                        worker->started.store(true, std::memory_order_release);
                        worker->release.wait(false, std::memory_order_acquire);
                        worker->finished.store(true, std::memory_order_release);
                    });
                } catch (const coro::Cancelled&) {
                    cancelled = true;
                    throw;
                }
            }));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!worker->started.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
                co_await coro::checkpoint();
            }
            EXPECT_TRUE(worker->started.load(std::memory_order_acquire));
            if (waiter_scope != nullptr) {
                waiter_scope->cancel();
            }
            co_await waiter;
            EXPECT_TRUE(cancelled);
            EXPECT_FALSE(worker->finished.load(std::memory_order_acquire));
            worker->unblock();  // The original awaiting frame is already retired.
        });
    }());
    EXPECT_TRUE(worker->finished.load(std::memory_order_acquire));
}

TEST(CoroWaitProtocol, CompletionLease_RunOrDisposeReleasesTheOnlyOwner) {
    for (const bool dispatch : {true, false}) {
        std::weak_ptr<coro::detail::OffloadSlot> weak;
        {
            coro::Loop loop;
            auto state = std::make_shared<coro::detail::OffloadState<int>>();
            weak = state;
            state->completed.store(true, std::memory_order_release);
            state->completion_lease = state;
            state->completion_node.context = state.get();
            using Awaitable = coro::detail::OffloadAwaitable<decltype([] { return 1; })>;
            state->completion_node.run = &Awaitable::completion_trampoline;
            state->completion_node.dispose = &Awaitable::dispose_completion;
            coro::detail::LoopAccess::post_node(loop, state->completion_node);
            state.reset();
            EXPECT_FALSE(weak.expired());
            if (dispatch) {
                coro::run(loop, []() -> coro::Task<void> { co_return; }());
                EXPECT_TRUE(weak.expired());
            }
        }
        EXPECT_TRUE(weak.expired());
    }
}

struct CaptureProbe {
    std::shared_ptr<WorkerGate> worker;

    explicit CaptureProbe(std::shared_ptr<WorkerGate> state) : worker(std::move(state)) {}

    ~CaptureProbe() noexcept { worker->capture_destroyed.store(true, std::memory_order_release); }
};

TEST(CoroWaitProtocol, JoinedCancellation_WaitsForWorkerAndCallableDestruction) {
    auto worker = std::make_shared<WorkerGate>();
    bool resumed = false;
    coro::run([&worker, &resumed]() -> coro::Task<void> {
        coro::CancelScope* waiter_scope = nullptr;
        co_await coro::with_nursery([&](coro::Nursery& nursery) -> coro::Task<void> {
            auto waiter = nursery.spawn(coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                waiter_scope = &scope;
                try {
                    co_await coro::offload_joined([worker, probe = std::make_unique<CaptureProbe>(worker)] {
                        worker->started.store(true, std::memory_order_release);
                        worker->release.wait(false, std::memory_order_acquire);
                        worker->finished.store(true, std::memory_order_release);
                    });
                } catch (const coro::Cancelled&) {
                    EXPECT_TRUE(worker->finished.load(std::memory_order_acquire));
                    EXPECT_TRUE(worker->capture_destroyed.load(std::memory_order_acquire));
                    resumed = true;
                    throw;
                }
            }));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!worker->started.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
                co_await coro::checkpoint();
            }
            EXPECT_TRUE(worker->started.load(std::memory_order_acquire));
            if (waiter_scope != nullptr) {
                waiter_scope->cancel();
            }
            co_await coro::checkpoint();
            co_await coro::checkpoint();
            EXPECT_FALSE(resumed);
            EXPECT_FALSE(worker->capture_destroyed.load(std::memory_order_acquire));
            worker->unblock();
            co_await waiter;
        });
    }());
    EXPECT_TRUE(resumed);
}

}  // namespace
