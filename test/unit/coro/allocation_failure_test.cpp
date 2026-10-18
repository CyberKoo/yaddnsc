// Isolated allocator fault injection: no production hooks and no effect on
// other test binaries. Only the calling thread's selected allocation fails.
#include <array>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "coro/coro.h"
#include "coro/detail/access.h"
#include "coro/detail/fd_entry.h"
#include "coro/detail/mutex_state.h"
#include "coro/detail/offload_job.h"
#include "coro/detail/timer_node.h"
#include "coro/detail/wait_operation.h"
#include "mocks/allocation_fault.h"

namespace {
thread_local int allocations_before_failure = -1;
thread_local std::size_t failure_size = 0;
thread_local bool fail_every_allocation = false;
thread_local std::size_t forbidden_allocations = 0;
thread_local bool record_frame_bytes = false;
thread_local std::size_t frame_bytes = 0;

void allocation_hook(std::size_t size) {
    if (record_frame_bytes) {
        frame_bytes += size;
    }
    if (fail_every_allocation) {
        ++forbidden_allocations;
        throw std::bad_alloc{};
    }
    if (allocations_before_failure == 0 && (failure_size == 0 || size == failure_size)) {
        allocations_before_failure = -1;
        throw std::bad_alloc{};
    }
    if (allocations_before_failure > 0) {
        --allocations_before_failure;
    }
}

class FailAllocation {
public:
    explicit FailAllocation(int after = 0) noexcept { allocations_before_failure = after; }

    FailAllocation(const FailAllocation&) = delete;
    FailAllocation& operator=(const FailAllocation&) = delete;

    ~FailAllocation() noexcept {
        allocations_before_failure = -1;
        failure_size = 0;
    }
};

// Arm this binary's allocation-fault hook before any test runs.
[[maybe_unused]] const bool allocation_hook_armed = [] {
    test::set_allocation_fault_hook(&allocation_hook);
    return true;
}();
}  // namespace

namespace {

coro::Task<void> hold_token(std::shared_ptr<int> token) {
    EXPECT_TRUE(token);
    co_return;
}

TEST(CoroAllocationFailure, spawn_EntryOrRegistryAllocationThrows_ReleasesUnstartedFrame) {
    for (const int after : {0, 1}) {
        for (bool discard : {false, true}) {
            auto token = std::make_shared<int>(1);
            auto root = [&]() -> coro::Task<void> {
                co_await coro::with_nursery([&](coro::Nursery& group) -> coro::Task<void> {
                    auto child = hold_token(token);
                    bool caught = false;
                    {
                        const FailAllocation failure{after};
                        try {
                            if (discard) {
                                group.spawn_discard(std::move(child));
                            } else {
                                group.spawn(std::move(child));
                            }
                        } catch (const std::bad_alloc&) {
                            caught = true;
                        }
                    }
                    EXPECT_TRUE(caught);
                    EXPECT_EQ(token.use_count(), 1);
                    co_return;
                });
            };
            coro::run(root());
            EXPECT_EQ(token.use_count(), 1);
        }
    }
}

TEST(CoroAllocationFailure, add_timer_HeapGrowthThrows_LeavesTimerUnarmedAndReusable) {
    coro::Loop loop;
    coro::detail::TimerNode timer;
    bool caught = false;
    {
        const FailAllocation failure;
        try {
            coro::detail::LoopAccess::add_timer(loop, timer, loop.now(), nullptr, nullptr);
        } catch (const std::bad_alloc&) {
            caught = true;
        }
    }
    EXPECT_TRUE(caught);
    EXPECT_FALSE(timer.in_heap);
    coro::detail::LoopAccess::remove_timer(loop, timer);
    coro::detail::LoopAccess::add_timer(loop, timer, loop.now(), nullptr, nullptr);
    EXPECT_TRUE(timer.in_heap);
    coro::detail::LoopAccess::remove_timer(loop, timer);
    EXPECT_FALSE(timer.in_heap);
}

TEST(CoroAllocationFailure, add_fd_StorageGrowthThrows_LeavesEntryUnlinkedAndReusable) {
    // Exercise table growth and both dispatch-buffer allocations.
    for (const int after : {0, 1, 2}) {
        coro::Loop loop;
        coro::detail::FdEntry entry;
        bool caught = false;
        {
            const FailAllocation failure{after};
            try {
                coro::detail::LoopAccess::add_fd(loop, entry);
            } catch (const std::bad_alloc&) {
                caught = true;
            }
        }
        EXPECT_TRUE(caught);
        EXPECT_FALSE(entry.linked);
        coro::detail::LoopAccess::remove_fd(loop, entry);
        coro::detail::LoopAccess::add_fd(loop, entry);
        EXPECT_TRUE(entry.linked);
        EXPECT_EQ(entry.index, 1u);  // only the self-pipe preceded this registration
        coro::detail::LoopAccess::remove_fd(loop, entry);
        EXPECT_FALSE(entry.linked);
    }
}

TEST(CoroAllocationFailure, ScopeRegistrationAndRemoval_DoNotAllocate) {
    coro::CancelScope root;
    bool completed = false;
    bool allocation_unused = false;
    {
        const FailAllocation failure;
        try {
            coro::CancelScope first{&root};
            coro::CancelScope second{&first};
            completed = true;
        } catch (const std::bad_alloc&) {
            // The old vector-backed child registration consumed the fault.
        }
        allocation_unused = allocations_before_failure == 0;
    }
    EXPECT_TRUE(completed);
    EXPECT_TRUE(allocation_unused);
}

TEST(CoroAllocationFailure, ScopeCancellation_TraversesBranchesWithoutAllocating) {
    coro::CancelScope root;
    coro::CancelScope first{&root};
    coro::CancelScope leaf{&first};
    coro::CancelScope sibling{&root};
    coro::CancelScope shield{&root, true};
    std::array<coro::detail::CancelSubscription, 4> nodes{};
    const std::array scopes{&first, &leaf, &sibling, &shield};
    int notifications = 0;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].arm(*scopes[i], &notifications, [](void* context) noexcept {
            ++*static_cast<int*>(context);
            return coro::detail::CancelAction::WAIT_EXTERNAL;
        });
    }
    bool allocation_unused = false;
    {
        const FailAllocation failure;
        root.cancel();
        allocation_unused = allocations_before_failure == 0;
    }
    EXPECT_TRUE(allocation_unused);
    EXPECT_EQ(notifications, 3);
    EXPECT_FALSE(nodes[0].armed());
    EXPECT_FALSE(nodes[1].armed());
    EXPECT_FALSE(nodes[2].armed());
    EXPECT_TRUE(nodes[3].armed());
    shield.cancel();
    EXPECT_EQ(notifications, 4);
}

TEST(CoroAllocationFailure, offload_SubmissionAllocationThrows_RollsBackRealCapacity) {
    coro::Loop loop;
    loop.set_offload_workers(1);
    EXPECT_EQ(loop.offload_workers(), 1u);
    std::atomic<bool> ran{false};
    coro::detail::OffloadAdmission admission;
    // The callback must match OffloadAdmission::failed's ownership signature.
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    admission.failed = [](void*, std::exception_ptr) noexcept {};
    admission.job = [&] { ran.store(true); };
    {
        const FailAllocation failure;
        EXPECT_THROW(coro::detail::LoopAccess::admit_offload(loop, admission), std::bad_alloc);
    }
    EXPECT_FALSE(admission.submitted);
    EXPECT_FALSE(admission.linked);
    EXPECT_FALSE(ran.load());
    EXPECT_EQ(coro::run(loop, coro::offload([] { return 42; })), 42);
}

TEST(CoroAllocationFailure, offload_PoolInitializationThrows_LaterAdmissionSucceeds) {
    coro::Loop loop;
    loop.set_offload_workers(1);
    coro::detail::OffloadAdmission admission;
    // The callback must match OffloadAdmission::failed's ownership signature.
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    admission.failed = [](void*, std::exception_ptr) noexcept {};
    admission.job = [] {};
    {
        const FailAllocation failure;
        EXPECT_THROW(coro::detail::LoopAccess::admit_offload(loop, admission), std::bad_alloc);
    }
    EXPECT_FALSE(admission.submitted);
    EXPECT_FALSE(admission.linked);
    EXPECT_EQ(coro::run(loop, coro::offload([] { return 42; })), 42);
}

TEST(CoroAllocationFailure, offload_DeferredSubmissionFailure_CompletesAndDispatchesSuccessor) {
    coro::Loop loop;
    loop.set_offload_workers(1);
    std::atomic<bool> release{false};
    std::atomic<bool> successor_ran{false};
    bool failed = false;
    coro::detail::OffloadAdmission blocker;
    // The callback must match OffloadAdmission::failed's ownership signature.
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    blocker.failed = [](void*, std::exception_ptr) noexcept { std::terminate(); };
    blocker.job = [&] { release.wait(false); };
    coro::detail::LoopAccess::admit_offload(loop, blocker);
    coro::detail::OffloadAdmission queued;
    queued.owner = &failed;
    // The callback signature is an ownership contract fixed by OffloadAdmission,
    // so `error` must keep being taken by value even though the body only reads it.
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    queued.failed = [](void* owner, std::exception_ptr error) noexcept {
        try {
            std::rethrow_exception(error);
        } catch (const std::bad_alloc&) {
            *static_cast<bool*>(owner) = true;
        } catch (...) {
            std::terminate();
        }
    };
    queued.job = [] { ADD_FAILURE() << "failed submission must not execute"; };
    coro::detail::LoopAccess::admit_offload(loop, queued);
    EXPECT_TRUE(queued.linked);
    auto root = [&]() -> coro::Task<void> {
        // Only fail the pool wrapper's std::function allocation, not inbox or
        // coroutine allocations needed to deliver its failure completion.
        struct PoolCapture {
            coro::Loop* loop;
            std::function<void()> job;
        };
        const FailAllocation failure;
        failure_size = sizeof(PoolCapture);
        release.store(true);
        release.notify_one();
        while (!failed) {
            co_await coro::checkpoint();
        }
        co_await coro::offload_joined([&] { successor_ran.store(true); });
        EXPECT_TRUE(failed);
        EXPECT_FALSE(queued.linked);
        EXPECT_FALSE(queued.submitted);
    };
    coro::run(loop, root());
    EXPECT_TRUE(successor_ran.load());
}

TEST(CoroAllocationFailure, MutexQueueAllocationThrows_NoScopeRegistrationOrLockLeak) {
    coro::detail::MutexState state;
    EXPECT_TRUE(state.try_acquire());
    // std::deque grows in blocks. Fail its next allocation without assuming
    // how many pointers this standard library stores in the current block.
    std::array<coro::detail::MutexWaiter, 4096> nodes{};
    std::size_t queued = 0;
    bool caught = false;
    {
        const FailAllocation failure;
        try {
            for (; queued < nodes.size(); ++queued) {
                state.enqueue(&nodes[queued]);
            }
        } catch (const std::bad_alloc&) {
            caught = true;
        }
    }
    EXPECT_TRUE(caught);
    EXPECT_EQ(state.queue.size(), queued);
    EXPECT_TRUE(state.locked);
    for (std::size_t i = 0; i < queued; ++i) {
        state.drop(nodes[i]);
    }
    state.release();
    EXPECT_FALSE(state.locked);
    EXPECT_TRUE(state.try_acquire());
    state.release();
}

coro::Task<int> allocation_chain(int depth) {
    if (depth == 0)
        co_return 1;
    co_return 1 + co_await allocation_chain(depth - 1);
}

TEST(CoroAllocationFailure, RootAndInlineActivation_AllocateOnlyCoroutineFrames) {
    for (const int depth : {0, 1, 8, 64}) {
        coro::Loop loop;
        int value = 0;
        std::size_t allocations = 0;
        frame_bytes = 0;
        record_frame_bytes = true;
        {
            const test::AllocationCounter counter;
            value = coro::run(loop, allocation_chain(depth));
            allocations = counter.count();
        }
        record_frame_bytes = false;
        // All chain frames coexist at the leaf: their actual allocation sizes,
        // not sizeof(promise), give this chain's peak frame residency.
        RecordProperty("peak_frame_bytes_depth_" + std::to_string(depth), std::to_string(frame_bytes));
        EXPECT_EQ(value, depth + 1);
        EXPECT_EQ(allocations, static_cast<std::size_t>(depth + 1));
    }
}

struct PayloadCleanupState {
    const void* slot = nullptr;
    bool capture = true;
    std::function<void()> action;
};

struct PayloadCleanupProbe {
    PayloadCleanupState& state;

    explicit PayloadCleanupProbe(PayloadCleanupState& target) noexcept : state(target) {
        if (state.capture)
            state.slot = this;
    }

    PayloadCleanupProbe(PayloadCleanupProbe&& other) noexcept : PayloadCleanupProbe(other.state) {}

    PayloadCleanupProbe(const PayloadCleanupProbe&) = delete;
    PayloadCleanupProbe& operator=(const PayloadCleanupProbe&) = delete;
    PayloadCleanupProbe& operator=(PayloadCleanupProbe&&) = delete;

    ~PayloadCleanupProbe() noexcept {
        if (state.slot == this) {
            state.slot = nullptr;
            if (state.action)
                state.action();
        }
    }
};

coro::Task<PayloadCleanupProbe> cleanup_payload(PayloadCleanupState& state) {
    co_return PayloadCleanupProbe{state};
}

coro::Task<void> cleanup_spawn_child(std::shared_ptr<int> token, int& started) {
    EXPECT_TRUE(token);
    ++started;
    co_return;
}

TEST(CoroAllocationFailure, ConsumedPayloadCleanup_CanGrowOpenNurseryOrRollBackSpawnOom) {
    for (const bool inject_failure : {false, true}) {
        PayloadCleanupState state;
        auto token = std::make_shared<int>(1);
        int started = 0;
        int callbacks = 0;
        bool caught = false;
        coro::run(coro::with_nursery([&](coro::Nursery& nursery) -> coro::Task<void> {
            nursery.spawn(cleanup_payload(state));     // Only the registry retains this entry.
            nursery.spawn_discard(hold_token(token));  // Keep a later entry for swap-and-pop.
            co_await coro::checkpoint();
            state.capture = false;  // Track the stored payload, not subsequent consumer moves.
            state.action = [&] {
                ++callbacks;
                auto child = cleanup_spawn_child(token, started);
                if (inject_failure) {
                    const FailAllocation failure;
                    try {
                        nursery.spawn_discard(std::move(child));
                    } catch (const std::bad_alloc&) {
                        caught = true;
                    }
                } else {
                    nursery.spawn_discard(std::move(child));
                    for (int i = 1; i < 32; ++i)
                        nursery.spawn_discard(cleanup_spawn_child(token, started));
                }
            };
            auto value = co_await nursery.next<PayloadCleanupProbe>();
            EXPECT_TRUE(value && value->has_value());
            state.action = {};
        }));
        EXPECT_EQ(callbacks, 1);
        EXPECT_EQ(caught, inject_failure);
        EXPECT_EQ(started, inject_failure ? 0 : 32);
        EXPECT_EQ(token.use_count(), 1);
    }
}

coro::Task<int> retained_value(int value) {
    co_return value;
}

TEST(CoroAllocationFailure, RetiredEntries_InvalidHandlesRetainOnlyBookkeepingStorage) {
    constexpr std::size_t COUNT = 256;
    std::array<coro::Handle<int>, COUNT> handles;
    std::size_t retained_bytes = 0;
    std::size_t allocations = 0;
    coro::run(coro::with_nursery([&](coro::Nursery& nursery) -> coro::Task<void> {
        auto warm = nursery.spawn(retained_value(0));
        EXPECT_EQ(co_await warm, 0);  // Keep registry capacity, not its first entry.
        for (std::size_t index = 0; index < COUNT; ++index) {
            auto task = retained_value(static_cast<int>(index));
            frame_bytes = 0;
            record_frame_bytes = true;
            {
                const test::AllocationCounter counter;
                handles[index] = nursery.spawn(std::move(task));
                allocations += counter.count();
            }
            record_frame_bytes = false;
            retained_bytes += frame_bytes;  // Only entry+control block: frame was created earlier.
            EXPECT_EQ(co_await handles[index], static_cast<int>(index));
        }
    }));
    for (auto& handle : handles)
        EXPECT_FALSE(handle.valid());
    EXPECT_EQ(allocations, COUNT);
    RecordProperty("invalid_handle_count", std::to_string(COUNT));
    RecordProperty("retained_entry_bytes", std::to_string(retained_bytes));
    RecordProperty("retained_bytes_per_entry", std::to_string(retained_bytes / COUNT));
}

struct CleanupFlag {
    bool& destroyed;

    explicit CleanupFlag(bool& flag) noexcept : destroyed(flag) {}

    ~CleanupFlag() noexcept { destroyed = true; }
};

coro::Task<void> wait_for_cancel(coro::Notification& armed, bool& finished) {
    armed.notify();
    try {
        co_await coro::sleep_for(std::chrono::hours{1});
    } catch (const coro::Cancelled&) {
        finished = true;
        throw;
    }
}

TEST(CoroAllocationFailure, ClosingCancellationAndDrain_RequireNoFrameworkAllocation) {
    bool finished = false, destroyed = false, failed = false;
    forbidden_allocations = 0;
    auto root = [&]() -> coro::Task<void> {
        coro::Notification armed;
        try {
            co_await coro::with_nursery(std::make_unique<CleanupFlag>(destroyed),
                                        [&](coro::Nursery& nursery, std::unique_ptr<CleanupFlag>&) -> coro::Task<void> {
                                            nursery.spawn_discard(wait_for_cancel(armed, finished));
                                            co_await armed.wait();
                                            fail_every_allocation = true;
                                            nursery.cancel();
                                        });
        } catch (const coro::Cancelled&) {
            failed = true;
        } catch (...) {
            failed = true;
        }
        fail_every_allocation = false;
    };
    coro::run(root());
    EXPECT_FALSE(failed);
    EXPECT_TRUE(finished);
    EXPECT_TRUE(destroyed);
    EXPECT_EQ(forbidden_allocations, 0U);
}

coro::Task<void> allocation_defect() {
    throw std::runtime_error("unobserved child");
    co_return;
}

TEST(CoroAllocationFailure, ErrorAggregationOom_InvalidatesHandlesAndReleasesResourcesBeforePropagation) {
    bool destroyed = false;
    bool caught = false;
    coro::Handle<void> retained;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            std::make_unique<CleanupFlag>(destroyed),
            [&](coro::Nursery& nursery, std::unique_ptr<CleanupFlag>&) -> coro::Task<void> {
                retained = nursery.spawn(allocation_defect());
                co_await coro::checkpoint();
                co_await coro::checkpoint();
                allocations_before_failure = 0;  // The next allocation is post-drain aggregation.
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    {
        const FailAllocation restore{-1};
        try {
            coro::run(root());
        } catch (const std::bad_alloc&) {
            caught = true;
        }
    }
    EXPECT_TRUE(caught);
    EXPECT_TRUE(destroyed);
    EXPECT_FALSE(retained.valid());
}

}  // namespace
