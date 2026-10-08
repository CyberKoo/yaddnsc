//
// Coroutine runtime — offload and SerialLane.
//
// These tests use the system clock and real worker threads. Timing margins are
// generous so the assertions do not depend on machine speed.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "infrastructure/coro/coro.h"

namespace {

using namespace std::chrono_literals;

coro::Task<void> run_pooled_job(std::function<void()> job) {
    auto result = co_await coro::offload(std::move(job));
    EXPECT_TRUE(result.has_value());
    co_return;
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
        EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            value = *result;
        }
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
        auto result = co_await coro::offload([&ran]() { ran.store(true); });
        EXPECT_TRUE(result.has_value());
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
        auto outcome = co_await coro::with_timeout(
            20ms, [&started, &finished, &timed_out](coro::CancelScope& scope) -> coro::Task<void> {
                auto result = co_await coro::offload([&started, &finished]() -> int {
                    started.store(true);
                    std::this_thread::sleep_for(200ms);
                    finished.store(true);
                    return 1;
                });
                EXPECT_FALSE(result.has_value());
                timed_out = scope.timed_out();
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
        auto result = co_await coro::offload([]() { std::this_thread::sleep_for(300ms); });
        EXPECT_TRUE(result.has_value());
        co_return;
    };
    auto second = [&second_ran, &timed_out]() -> coro::Task<void> {
        // Let the only worker become busy with the blocker.
        co_await coro::sleep_for(20ms);
        auto outcome =
            co_await coro::with_timeout(20ms, [&second_ran, &timed_out](coro::CancelScope& scope) -> coro::Task<void> {
                auto result = co_await coro::offload([&second_ran]() { second_ran.store(true); });
                EXPECT_FALSE(result.has_value());
                timed_out = scope.timed_out();
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

// ---------------------------------------------------------------------------
// SerialLane
// ---------------------------------------------------------------------------

TEST(SerialLane, submit_FirstJobAbandoned_KeepsSubmissionOrder) {
    coro::Loop loop;
    coro::SerialLane lane;
    std::mutex order_mutex;
    std::vector<int> order;
    bool first_abandoned = false;

    auto work = [&order_mutex, &order](int id, int delay_ms) {
        return [&order_mutex, &order, id, delay_ms]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            const std::lock_guard lock(order_mutex);
            order.push_back(id);
        };
    };

    auto task = [&lane, &work, &first_abandoned]() -> coro::Task<void> {
        auto abandoned = co_await coro::with_timeout(
            10ms, [&lane, &work, &first_abandoned](coro::CancelScope& scope) -> coro::Task<void> {
                auto result = co_await lane.submit(work(1, 80));
                first_abandoned = !result.has_value() && scope.timed_out();
                co_return;
            });
        EXPECT_TRUE(abandoned.timed_out);
        // Submitted while the abandoned job is still running on the worker: the
        // lane must not start it until that job has completed.
        auto second = co_await lane.submit(work(2, 0));
        EXPECT_TRUE(second.has_value());
        co_return;
    };

    coro::run(loop, task());
    EXPECT_TRUE(first_abandoned);
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
}

TEST(SerialLane, submit_SequentialJobs_ReturnsValuesInOrder) {
    coro::Loop loop;
    coro::SerialLane lane;
    std::vector<int> results;

    auto task = [&lane, &results]() -> coro::Task<void> {
        for (int i = 0; i < 4; ++i) {
            auto result = co_await lane.submit([i]() -> int { return i * 10; });
            EXPECT_TRUE(result.has_value());
            if (result.has_value()) {
                results.push_back(*result);
            }
        }
        co_return;
    };

    coro::run(loop, task());
    EXPECT_EQ(results, (std::vector<int>{0, 10, 20, 30}));
}

}  // namespace
