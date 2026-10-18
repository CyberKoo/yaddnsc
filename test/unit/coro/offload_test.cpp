//
// Coroutine runtime — offload.
//
// These tests use real worker threads; explicit gates hold running work across
// cancellation and root retirement without machine-speed-dependent windows.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "coro/coro.h"
#include "coro/detail/access.h"
#include "coro/detail/execution_record.h"
#include "coro/detail/offload_job.h"
#include "coro/detail/result_state.hpp"
#include "coro/detail/task_promise.hpp"

namespace {

using namespace std::chrono_literals;

struct ReleaseOnExit {
    std::atomic<bool>& gate;

    void release() const noexcept {
        gate.store(true);
        gate.notify_all();
    }

    ~ReleaseOnExit() noexcept { release(); }
};

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
    std::atomic<bool> release{false};

    auto worker = [&]() -> coro::Task<void> {
        co_await coro::offload([&]() -> int {
            started.store(true);
            release.wait(false);
            finished.store(true);
            return 1;
        });
    };
    auto task = [&]() -> coro::Task<void> {
        const ReleaseOnExit unblock{release};
        auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
            // Cancellation is unreachable until the callable is running: a job
            // whose pool worker has not started yet is dropped without running
            // it, so a deadline armed at admission would abandon nothing.
            co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
                group.spawn(worker());
                while (!started.load()) {
                    co_await coro::checkpoint();
                }
                EXPECT_FALSE(finished.load());
                scope.cancel();
                EXPECT_FALSE(finished.load());
            });
        });
        EXPECT_TRUE(outcome.cancelled);
        EXPECT_FALSE(finished.load());
    };

    coro::run(loop, task());
    EXPECT_TRUE(started.load());
    // Abandon ended the await, but run() must drain the actual worker. Do not
    // poll after return: that would hide a broken runtime lifetime guarantee.
    EXPECT_TRUE(finished.load());
}

TEST(Offload, offload_AbandonedWhileQueued_DropsJob) {
    coro::Loop loop;
    loop.set_offload_workers(1);
    std::atomic<bool> blocker_started{false};
    std::atomic<bool> release{false};
    std::atomic<bool> second_ran{false};
    bool timed_out = false;

    auto blocker = [&]() -> coro::Task<void> {
        co_await coro::offload([&]() {
            blocker_started.store(true);
            while (!release.load()) {
                std::this_thread::sleep_for(1ms);
            }
        });
        co_return;
    };
    auto second = [&]() -> coro::Task<void> {
        // The only worker must be busy with the blocker first, otherwise the
        // second job is admitted and runs instead of being abandoned in the
        // queue. The blocker holds the worker until released below, so the
        // abandonment window never closes on a slow machine.
        while (!blocker_started.load()) {
            co_await coro::checkpoint();
        }
        auto outcome = co_await coro::with_timeout(20ms, [&]() -> coro::Task<void> {
            try {
                co_await coro::offload([&]() { second_ran.store(true); });
            } catch (const coro::Cancelled&) {
                timed_out = true;
                throw;
            }
            co_return;
        });
        EXPECT_TRUE(outcome.timed_out);
        release.store(true);
        co_return;
    };
    auto task = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                group.spawn(blocker());
                group.spawn(second());
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
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
        co_await coro::with_nursery([&job](coro::Nursery& group) -> coro::Task<void> {
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
    std::atomic<bool> release{false};
    {
        coro::Loop loop;
        auto root = [&]() -> coro::Task<void> {
            const ReleaseOnExit unblock{release};
            auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
                    group.spawn(run_pooled_job([&started, &finished, &release] {
                        started.store(true);
                        release.wait(false);
                        finished.store(true);
                    }));
                    while (!started.load()) {
                        co_await coro::checkpoint();
                    }
                    EXPECT_FALSE(finished.load());
                    scope.cancel();
                    EXPECT_FALSE(finished.load());
                });
            });
            EXPECT_TRUE(outcome.cancelled);
            EXPECT_FALSE(finished.load());
        };
        coro::run(loop, root());
    }
    EXPECT_TRUE(started.load());
    EXPECT_TRUE(finished.load());
}

TEST(Offload, Joined_CancellationWaitsForWorkerBeforeUnwindingBorrowedResources) {
    coro::ManualClock clock;
    coro::Loop loop(clock);
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> release{false};
    coro::CancelScope* worker_scope = nullptr;
    bool unwound = false;

    auto worker = [&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
            worker_scope = &scope;
            auto borrowed = std::make_shared<int>(42);
            try {
                co_await coro::offload_joined([&, weak = std::weak_ptr<int>{borrowed}] {
                    started.store(true);
                    release.wait(false);
                    // Do not dereference a destroyed frame even if joining is
                    // broken: the weak borrow still detects premature unwind.
                    const auto resource = weak.lock();
                    EXPECT_NE(resource, nullptr);
                    if (resource) {
                        EXPECT_EQ(*resource, 42);
                    }
                    finished.store(true);
                });
            } catch (const coro::Cancelled&) {
                EXPECT_TRUE(finished.load());
                unwound = true;
                throw;
            }
        });
        EXPECT_TRUE(outcome.cancelled);
        worker_scope = nullptr;
    };
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
            const ReleaseOnExit unblock{release};
            auto handle = group.spawn(worker());
            while (!started.load()) {
                co_await coro::checkpoint();
            }
            EXPECT_FALSE(finished.load());
            worker_scope->cancel();
            // The join cannot complete while the worker is held. A virtual
            // deadline drains all ready cancellation work before releasing it.
            const auto held = co_await coro::with_timeout(1ms, [&]() -> coro::Task<void> { co_await handle; });
            EXPECT_TRUE(held.timed_out);
            EXPECT_FALSE(held.completed);
            EXPECT_FALSE(unwound);
            EXPECT_FALSE(finished.load());
            unblock.release();
            co_await handle;
            EXPECT_TRUE(unwound);
            EXPECT_TRUE(finished.load());
        });
    };
    coro::run(loop, root());
    EXPECT_TRUE(started.load());
}

TEST(Offload, Joined_CooperativeWorkerReceivesStopAndIsJoined) {
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};

    auto worker = [&]() -> coro::Task<void> {
        co_await coro::offload_joined([&](const std::stop_token& token) {
            started.store(true);
            while (!token.stop_requested()) {
                std::this_thread::sleep_for(1ms);
            }
            stopped.store(true);
        });
    };
    auto root = [&]() -> coro::Task<void> {
        auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
            // Cancel only after the worker is running: a queued job is dropped
            // without running it, and no stop token would be observed.
            co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
                group.spawn(worker());
                while (!started.load()) {
                    co_await coro::checkpoint();
                }
                scope.cancel();
            });
        });
        EXPECT_TRUE(outcome.cancelled);
        EXPECT_TRUE(stopped.load());
    };
    coro::run(root());
}

TEST(Offload, Joined_AlreadyCancelled_DoesNotStartCallable) {
    bool ran = false;
    auto root = [&]() -> coro::Task<void> {
        auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
            scope.cancel();
            co_await coro::offload_joined([&](const std::stop_token&) { ran = true; });
        });
        EXPECT_TRUE(outcome.cancelled);
    };
    coro::run(root());
    EXPECT_FALSE(ran);
}

TEST(Offload, Joined_AdmissionCancellation_SkipsWorkWithoutWaitingForBusyWorker) {
    coro::Loop loop;
    loop.set_offload_workers(1);
    std::atomic<bool> blocker_started{false};
    std::atomic<bool> blocker_finished{false};
    std::atomic<bool> release{false};
    bool ran = false;
    auto blocker = [&]() -> coro::Task<void> {
        co_await coro::offload([&] {
            blocker_started.store(true);
            // Hold the only worker until the joined job's admission deadline
            // below has fired; a fixed sleep would reopen a race between the
            // deadline and the blocker's remaining runtime on a slow machine.
            while (!release.load()) {
                std::this_thread::sleep_for(1ms);
            }
            blocker_finished.store(true);
        });
    };
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
            group.spawn(blocker());
            while (!blocker_started.load()) {
                co_await coro::checkpoint();
            }
            auto outcome = co_await coro::with_timeout(
                10ms, [&]() -> coro::Task<void> { co_await coro::offload_joined([&] { ran = true; }); });
            EXPECT_TRUE(outcome.timed_out);
            EXPECT_FALSE(blocker_finished.load());
            release.store(true);
        });
    };
    coro::run(loop, root());
    EXPECT_FALSE(ran);
}

TEST(Offload, RootExit_PumpsInboxUntilAbandonedWorkersFinish) {
    coro::Loop loop;
    std::atomic<bool> callback{false};
    std::atomic<bool> worker_started{false};
    std::atomic<bool> worker_done{false};
    std::atomic<bool> release{false};
    const ReleaseOnExit unblock{release};
    const ReleaseOnExit unblock_callback{callback};
    coro::CancelScope root_scope;
    coro::detail::ExecutionRecord root_record;
    coro::detail::ResultState<void> root_result;

    struct Retirement {
        const ReleaseOnExit& unblock;
        bool retired = false;

        static void on_retired(void* context, coro::detail::ExecutionRecord& record) noexcept {
            auto& self = *static_cast<Retirement*>(context);
            self.retired = record.phase() == coro::detail::ExecutionPhase::RETIRED;
            self.unblock.release();
        }
    } retirement{unblock};

    auto worker = [&]() -> coro::Task<void> {
        co_await coro::offload([&] {
            worker_started.store(true);
            release.wait(false);
            loop.post([&] {
                EXPECT_TRUE(retirement.retired);
                EXPECT_EQ(root_record.phase(), coro::detail::ExecutionPhase::RETIRED);
                callback.store(true);
                callback.notify_all();
            });
            callback.wait(false);
            worker_done.store(true);
        });
    };
    auto root = [&]() -> coro::Task<void> {
        auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
            // Cancellation is unreachable until the callable is running: a job
            // whose pool worker has not started yet is dropped without running
            // it, so a deadline armed at admission would abandon nothing and
            // leave the test asserting nothing.
            co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
                group.spawn(worker());
                while (!worker_started.load()) {
                    co_await coro::checkpoint();
                }
                EXPECT_FALSE(worker_done.load());
                scope.cancel();
                EXPECT_FALSE(worker_done.load());
            });
        });
        EXPECT_TRUE(outcome.cancelled);
    };
    auto task = root();
    coro::detail::TaskAccess::start_root(task, root_record, root_result, loop, root_scope);
    // Unlike a body-return flag, this callback runs only after frame destruction,
    // result publication and the root's transition to RETIRED.
    root_record.bind_completion<nullptr, &Retirement::on_retired>(&retirement);
    loop.run();
    EXPECT_EQ(root_record.phase(), coro::detail::ExecutionPhase::RETIRED);
    EXPECT_TRUE(root_result.ready());
    EXPECT_EQ(root_result.content(), coro::detail::ResultContent::VALUE);
    EXPECT_TRUE(retirement.retired);
    // Abandon ended the child's await, but run() must keep pumping the inbox
    // for the still-running worker. Do not poll after return: that would hide a
    // broken runtime lifetime guarantee.
    EXPECT_TRUE(callback.load());
    EXPECT_TRUE(worker_done.load());
}

struct LvalueCallable {
    int operator()() & { return 42; }

    int operator()() && = delete;
};

struct CooperativeLvalueCallable {
    int operator()(const std::stop_token& token) & { return token.stop_requested() ? 0 : 43; }

    int operator()(const std::stop_token&) && = delete;
};

TEST(Offload, LvalueQualifiedCallable_IsInvokedAsAnOwnedLvalue) {
    EXPECT_EQ(coro::run(coro::offload(LvalueCallable{})), 42);
    EXPECT_EQ(coro::run(coro::offload_joined(LvalueCallable{})), 42);
    EXPECT_EQ(coro::run(coro::offload_joined(CooperativeLvalueCallable{})), 43);
}

TEST(Offload, Admission_RepeatedCancellation_ReleasesCapturesBeforeWorkerFinishes) {
    coro::Loop loop;
    loop.set_offload_workers(1);
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    bool ran = false;
    auto blocker = [&]() -> coro::Task<void> {
        co_await coro::offload_joined([&] {
            started.store(true);
            started.notify_one();
            release.wait(false);
        });
    };
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
            group.spawn(blocker());
            while (!started.load()) {
                co_await coro::checkpoint();
            }
            for (int i = 0; i < 32; ++i) {
                auto lease = std::make_shared<int>(i);
                const std::weak_ptr<int> weak = lease;
                auto queued = coro::offload([lease = std::move(lease), &ran] { ran = true; });
                auto outcome =
                    co_await coro::with_timeout(1ms, [&]() -> coro::Task<void> { co_await std::move(queued); });
                EXPECT_TRUE(outcome.timed_out);
                EXPECT_TRUE(weak.expired());
            }
            release.store(true);
            release.notify_one();
        });
    };
    coro::run(loop, root());
    EXPECT_FALSE(ran);
}

TEST(Offload, Joined_DefectAfterCancellation_IsStillReported) {
    auto root = []() -> coro::Task<void> {
        co_await coro::with_nursery([](coro::Nursery& group) -> coro::Task<void> {
            std::atomic<bool> started{false};
            EXPECT_THROW(co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                             auto cancel = [&]() -> coro::Task<void> {
                                 while (!started.load()) {
                                     co_await coro::checkpoint();
                                 }
                                 scope.cancel();
                             };
                             group.spawn_discard(cancel());
                             co_await coro::offload_joined([&](const std::stop_token& stop) {
                                 std::atomic<bool> cancelled{false};
                                 const std::stop_callback notify{stop, [&] {
                                                                     cancelled.store(true);
                                                                     cancelled.notify_one();
                                                                 }};
                                 started.store(true);
                                 cancelled.wait(false);
                                 throw std::runtime_error("defect during worker cleanup");
                             });
                         }),
                         std::runtime_error);
        });
    };
    coro::run(root());
}

TEST(Offload, Admission_FifoSurvivesRunningCancellationAndHoldsCapacityUntilActualCompletion) {
    for (bool joined : {false, true}) {
        coro::Loop loop;
        loop.set_offload_workers(1);
        std::atomic<bool> started{false};
        std::atomic<bool> release{false};
        coro::CancelScope* running_scope = nullptr;
        bool caller_finished = false;
        std::vector<int> order;  // accessed by the single worker, read only after drain
        auto running = [&]() -> coro::Task<void> {
            const auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                running_scope = &scope;
                auto work = [&] {
                    started.store(true);
                    release.wait(false);
                };
                if (joined) {
                    co_await coro::offload_joined(work);
                } else {
                    co_await coro::offload(work);
                }
            });
            EXPECT_TRUE(outcome.cancelled);
            caller_finished = true;
        };
        auto root = [&]() -> coro::Task<void> {
            co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
                auto running_handle = group.spawn(running());
                while (!started.load()) {
                    co_await coro::checkpoint();
                }
                std::array<coro::detail::OffloadAdmission, 3> queued;
                for (std::size_t i = 0; i < queued.size(); ++i) {
                    // Fixed callback signature; failure is fatal in this test.
                    // NOLINTNEXTLINE(performance-unnecessary-value-param)
                    queued[i].failed = [](void*, std::exception_ptr) noexcept { std::terminate(); };
                    queued[i].job = [&, i] {
                        EXPECT_TRUE(release.load());
                        order.push_back(static_cast<int>(i));
                    };
                    coro::detail::LoopAccess::admit_offload(loop, queued[i]);
                    EXPECT_TRUE(queued[i].linked);
                    EXPECT_FALSE(queued[i].submitted);
                }
                running_scope->cancel();
                if (!joined) {
                    while (!caller_finished) {
                        co_await coro::checkpoint();
                    }
                }
                EXPECT_EQ(caller_finished, !joined);
                for (const auto& admission : queued) {
                    EXPECT_TRUE(admission.linked);
                    EXPECT_FALSE(admission.submitted);
                }
                release.store(true);
                release.notify_one();
                // This sentinel queues behind the three admissions and proves
                // their nodes have been submitted before their storage leaves.
                co_await coro::offload_joined([] {});
                co_await running_handle;
                EXPECT_EQ(order, (std::vector<int>{0, 1, 2}));
            });
        };
        coro::run(loop, root());
    }
}

}  // namespace
