// Isolated allocator fault injection: no production hooks and no effect on
// other test binaries. Only the calling thread's selected allocation fails.
#include <array>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include <coroutine>

#include <gtest/gtest.h>

#include "coro/coro.h"
#include "coro/detail/access.h"
#include "coro/detail/mutex_state.h"
#include "coro/detail/offload_job.h"
#include "coro/detail/timer_node.h"
#include "coro/detail/wait_node.h"

// Replacement allocators require the matching malloc/free ownership boundary.
// NOLINTBEGIN(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
namespace {
thread_local int allocations_before_failure = -1;
thread_local std::size_t failure_size = 0;

void* allocate(std::size_t size) {
    if (allocations_before_failure == 0 && (failure_size == 0 || size == failure_size)) {
        allocations_before_failure = -1;
        throw std::bad_alloc{};
    }
    if (allocations_before_failure > 0) {
        --allocations_before_failure;
    }
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) {
        return pointer;
    }
    throw std::bad_alloc{};
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
}  // namespace

// Test-only replacement allocation functions, paired with malloc/free.
void* operator new(std::size_t size) {
    return allocate(size);
}

void* operator new[](std::size_t size) {
    return allocate(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

// NOLINTEND(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)

namespace {

coro::Task<void> hold_token(std::shared_ptr<int> token) {
    EXPECT_TRUE(token);
    co_return;
}

TEST(CoroAllocationFailure, spawn_SlotAllocationThrows_ReleasesUnstartedFrame) {
    for (bool discard : {false, true}) {
        auto token = std::make_shared<int>(1);
        auto root = [&]() -> coro::Task<void> {
            co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
                auto child = hold_token(token);
                bool caught = false;
                {
                    const FailAllocation failure;
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
    std::array<coro::detail::WaitNode, 4> nodes{};
    const std::array scopes{&first, &leaf, &sibling, &shield};
    int notifications = 0;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].owner = &notifications;
        nodes[i].on_cancel = [](coro::detail::WaitNode& node) noexcept { ++*static_cast<int*>(node.owner); };
        coro::detail::ScopeAccess::add_waiter(*scopes[i], nodes[i]);
    }
    bool allocation_unused = false;
    {
        const FailAllocation failure;
        root.cancel();
        allocation_unused = allocations_before_failure == 0;
    }
    EXPECT_TRUE(allocation_unused);
    EXPECT_EQ(notifications, 3);
    EXPECT_FALSE(nodes[0].linked);
    EXPECT_FALSE(nodes[1].linked);
    EXPECT_FALSE(nodes[2].linked);
    EXPECT_TRUE(nodes[3].linked);
    EXPECT_FALSE(nodes[3].scheduled);
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
    queued.failed = [](void* owner, std::exception_ptr error) noexcept {
        try {
            std::rethrow_exception(std::move(error));
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
    std::array<coro::detail::WaitNode, 4096> nodes{};
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
    if (caught) {
        EXPECT_FALSE(nodes[queued].linked);
    }
    EXPECT_TRUE(state.locked);
    for (std::size_t i = 0; i < queued; ++i) {
        state.drop(nodes[i]);
    }
    state.release();
    EXPECT_FALSE(state.locked);
    EXPECT_TRUE(state.try_acquire());
    state.release();
}

}  // namespace
