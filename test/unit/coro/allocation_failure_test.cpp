// Isolated allocator fault injection: no production hooks and no effect on
// other test binaries. Only the calling thread's selected allocation fails.
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <utility>

#include <gtest/gtest.h>

#include "coro/coro.h"
#include "coro/detail/access.h"
#include "coro/detail/timer_node.h"

namespace {
thread_local int allocations_before_failure = -1;

void* allocate(std::size_t size) {
    if (allocations_before_failure == 0) {
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

    ~FailAllocation() noexcept { allocations_before_failure = -1; }
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

}  // namespace
