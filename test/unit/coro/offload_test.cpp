//
// Coroutine runtime — offload.
//
// These tests use the system clock and real worker threads. Timing margins are
// generous so the assertions do not depend on machine speed.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include "coro/coro.h"

namespace {

using namespace std::chrono_literals;

coro::Task<void> run_pooled_job(std::function<void()> job) {
    co_await coro::offload(std::move(job));
    co_return;
}

TEST(Offload, defaultOffloadWorkers_NoExplicitSetting_FollowsSizingPolicy) {
    // The production default: min(hardware cores, 4), at least 2. A default-
    // constructed loop must never fall through to the library's
    // hardware_concurrency() fallback.
    EXPECT_EQ(coro::Loop::default_offload_workers(), std::max(2u, std::min(std::thread::hardware_concurrency(), 4u)));
}

TEST(Offload, offloadPool_NoExplicitSetting_UsesDefaultWorkerCount) {
    coro::Loop loop;
    EXPECT_EQ(loop.offload_workers(), coro::Loop::default_offload_workers());
}

TEST(Offload, offload_Callable_ReturnsResultFromWorkerThread) {
    coro::Loop loop;
    std::thread::id loop_thread;
    std::thread::id worker_thread;
    int value = 0;

    auto task = [&loop_thread, &worker_thread, &value]() -> coro::Task<void> {
        loop_thread = std::this_thread::get_id();
        auto result = co_await coro::offload([&worker_thread]() -> int {
            worker_thread = std::this_thread::get_id();
            return 7;
        });
        value = result;
        co_return;
    };

    coro::run(loop, task());
    EXPECT_EQ(value, 7);
    EXPECT_NE(loop_thread, worker_thread);
}

TEST(Offload, offload_VoidCallable_ReturnsSuccess) {
    coro::Loop loop;
    std::atomic<bool> ran{false};

    auto task = [&ran]() -> coro::Task<void> {
        co_await coro::offload([&ran]() { ran.store(true); });
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(ran.load());
}

TEST(Offload, offload_CallableThrows_RethrowsAtAwait) {
    bool caught = false;
    auto task = []() -> coro::Task<void> {
        // The await rethrows; the result is unreachable.
        [[maybe_unused]] auto result = co_await coro::offload([]() -> int { throw std::runtime_error("worker boom"); });
        co_return;
    };

    try {
        coro::run(task());
        FAIL() << "the worker defect must be rethrown at the await";
    } catch (const std::runtime_error& error) {
        caught = std::string(error.what()) == "worker boom";
    }
    EXPECT_TRUE(caught);
}

TEST(Offload, offload_AbandonedWhileRunning_JobStillFinishes) {
    coro::Loop loop;
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    bool timed_out = false;

    auto task = [&started, &finished, &timed_out]() -> coro::Task<void> {
        auto outcome = co_await coro::with_timeout(20ms, [&started, &finished, &timed_out]() -> coro::Task<void> {
            try {
                co_await coro::offload([&started, &finished]() -> int {
                    started.store(true);
                    std::this_thread::sleep_for(200ms);
                    finished.store(true);
                    return 1;
                });
            } catch (const coro::Cancelled&) {
                timed_out = true;
                throw;
            }
            co_return;
        });
        EXPECT_TRUE(outcome.timed_out);
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(timed_out);
    EXPECT_TRUE(started.load());
    // The abandoned job still runs to completion; its result is discarded.
    for (int i = 0; i < 200 && !finished.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(finished.load());
}

TEST(Offload, offload_AbandonedWhileQueued_DropsJob) {
    coro::Loop loop;
    loop.set_offload_workers(1);
    std::atomic<bool> second_ran{false};
    bool timed_out = false;

    auto blocker = []() -> coro::Task<void> {
        co_await coro::offload([]() { std::this_thread::sleep_for(300ms); });
        co_return;
    };
    auto second = [&second_ran, &timed_out]() -> coro::Task<void> {
        // Let the only worker become busy with the blocker.
        co_await coro::sleep_for(20ms);
        auto outcome = co_await coro::with_timeout(20ms, [&second_ran, &timed_out]() -> coro::Task<void> {
            try {
                co_await coro::offload([&second_ran]() { second_ran.store(true); });
            } catch (const coro::Cancelled&) {
                timed_out = true;
                throw;
            }
            co_return;
        });
        EXPECT_TRUE(outcome.timed_out);
        co_return;
    };
    auto task = [&blocker, &second]() -> coro::Task<void> {
        co_await coro::supervisor_group([&blocker, &second](coro::TaskGroup& group) -> coro::Task<void> {
            group.spawn(blocker());
            group.spawn(second());
            co_return;
        });
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(timed_out);
    EXPECT_FALSE(second_ran.load());
}

TEST(Offload, offload_MoreJobsThanWorkers_BoundsConcurrency) {
    coro::Loop loop;
    loop.set_offload_workers(2);
    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    std::atomic<int> completed{0};

    auto job = [&active, &peak, &completed]() {
        const int current = active.fetch_add(1) + 1;
        int seen = peak.load();
        while (current > seen && !peak.compare_exchange_weak(seen, current)) {
        }
        std::this_thread::sleep_for(20ms);
        active.fetch_sub(1);
        completed.fetch_add(1);
    };

    auto task = [&job]() -> coro::Task<void> {
        co_await coro::task_group([&job](coro::TaskGroup& group) -> coro::Task<void> {
            for (int i = 0; i < 6; ++i) {
                group.spawn(run_pooled_job(job));
            }
            co_return;
        });
        co_return;
    };

    coro::run(loop, task());
    EXPECT_EQ(completed.load(), 6);
    EXPECT_LE(peak.load(), 2);
}

TEST(Offload, offload_DeferredTemporaryCallable_OwnsCallableBeforeStarting) {
    auto task = coro::offload([data = std::make_unique<std::string>("owned")] { return *data; });
    EXPECT_EQ(coro::run(std::move(task)), "owned");
}

TEST(Offload, offload_DeferredLvalueCallable_CopiesBeforeCallerChangesIt) {
    std::function<int()> callable = [] { return 42; };
    auto task = coro::offload(callable);
    callable = [] { return 7; };
    EXPECT_EQ(coro::run(std::move(task)), 42);
}

TEST(Offload, loop_DestroyedWithAbandonedWorker_JoinsBeforeClosingWakePipe) {
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    {
        coro::Loop loop;
        auto root = [&]() -> coro::Task<void> {
            auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
                    group.spawn(run_pooled_job([&started, &finished] {
                        started.store(true);
                        std::this_thread::sleep_for(50ms);
                        finished.store(true);
                    }));
                    while (!started.load()) {
                        co_await coro::checkpoint();
                    }
                    scope.cancel();
                });
            });
            EXPECT_TRUE(outcome.cancelled);
        };
        coro::run(loop, root());
    }
    EXPECT_TRUE(started.load());
    EXPECT_TRUE(finished.load());
}

}  // namespace
