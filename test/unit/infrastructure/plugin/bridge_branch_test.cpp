//
// Unit tests for the branch arms of src/infrastructure/plugin/bridge.cpp that
// the gateway's happy path never reaches: ingress closure, closed-group
// rejection, and the two dispatch-failure arms of Bridge::exchange.
//
// Everything runs in memory. No plugin is dlopen'd and no socket or file is
// touched: host faults are injected with a test-local allocator that fails
// exactly one allocation, inside a window no other code runs in.
// =============================================================================

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdlib>
#include <expected>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "coro/checkpoint.hpp"
#include "coro/group.hpp"
#include "coro/loop.h"
#include "coro/offload.hpp"
#include "coro/run.hpp"
#include "coro/task.hpp"
#include "infrastructure/http/types.h"
#include "infrastructure/plugin/bridge.h"
#include "yaddnsc/sdk/driver_abi.h"

namespace coro {
class TaskGroup;
}  // namespace coro

// Replacement allocators require the matching malloc/free ownership boundary.
// NOLINTBEGIN(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
namespace {

/// Allocations left before the next one on this thread throws. -1 disarms.
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

/// Arms one allocation failure on the calling thread. Keep the window as
/// narrow as the call under test — everything else in the process still
/// allocates normally.
class FailNextAllocation {
public:
    FailNextAllocation() noexcept { allocations_before_failure = 0; }

    FailNextAllocation(const FailNextAllocation&) = delete;
    FailNextAllocation& operator=(const FailNextAllocation&) = delete;

    ~FailNextAllocation() noexcept { allocations_before_failure = -1; }
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

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
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

/// A cycle that never does anything: start_cycle only publishes it.
coro::Task<void> noop_cycle() {
    co_return;
}

[[nodiscard]] std::shared_ptr<plugin::BridgeCall> make_call() {
    auto call = std::make_shared<plugin::BridgeCall>();
    call->url = "http://127.0.0.1:1/unreachable";
    return call;
}

constexpr std::chrono::seconds kWaitBudget{5};

}  // namespace

// ===========================================================================
//  start_cycle — closed-group ingress
// ===========================================================================

TEST(BridgeIngress, StartCycle_AfterGroupClosed_ThrowsInsteadOfPublishing) {
    coro::Loop loop;
    std::unique_ptr<plugin::Bridge> bridge;
    bool caught = false;

    auto root = [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            bridge = std::make_unique<plugin::Bridge>(loop, group, http::Options{}, kWaitBudget);
            co_return;
        });
        // The group exited: its ingress token closed, so the dead group is
        // never dereferenced and the cycle is refused as a value.
        try {
            bridge->start_cycle(noop_cycle());
        } catch (const std::logic_error&) {
            caught = true;
        }
    };
    coro::run(loop, root());

    EXPECT_TRUE(caught);
}

TEST(BridgeIngress, StartCycle_SlotAllocationFailure_PropagatesAndKeepsTheGroupUsable) {
    coro::Loop loop;
    bool caught = false;
    bool second_cycle_published = false;

    auto root = [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            plugin::Bridge bridge{loop, group, http::Options{}, kWaitBudget};
            auto cycle = noop_cycle();
            {
                // Slot allocation precedes ownership transfer, so the group is
                // left untouched and can still accept work afterwards.
                const FailNextAllocation failure;
                try {
                    bridge.start_cycle(std::move(cycle));
                } catch (const std::bad_alloc&) {
                    caught = true;
                }
            }
            try {
                bridge.start_cycle(noop_cycle());
                second_cycle_published = true;
            } catch (const std::bad_alloc&) {
                ADD_FAILURE() << "a failed spawn must not poison the group";
            }
            co_return;
        });
    };
    coro::run(loop, root());

    EXPECT_TRUE(caught);
    EXPECT_TRUE(second_cycle_published);
}

// ===========================================================================
//  cancel() — the two no-op arms
// ===========================================================================

TEST(BridgeIngress, Cancel_NullCall_DoesNotReachTheLoop) {
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            plugin::Bridge bridge{loop, group, http::Options{}, kWaitBudget};
            bridge.cancel(nullptr);
            // Nothing was posted, so the loop simply resumes and drains the
            // group: reaching the line after the checkpoint is the proof.
            co_await coro::checkpoint();
        });
    };
    coro::run(loop, root());
}

TEST(BridgeIngress, Cancel_CallWithoutLiveExchange_LatchesWithoutCancellingAScope) {
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            plugin::Bridge bridge{loop, group, http::Options{}, kWaitBudget};
            auto call = make_call();
            // No exchange ever started, so the posted lookup finds no scope.
            bridge.cancel(call);
            co_await coro::checkpoint();
            co_await coro::checkpoint();

            EXPECT_TRUE(call->cancelled.load(std::memory_order_acquire));
            EXPECT_EQ(call->live_scope, nullptr);
        });
    };
    coro::run(loop, root());
}

// ===========================================================================
//  exchange() — loop-side admission
// ===========================================================================

TEST(BridgeIngress, Exchange_AlreadyCancelledCall_IsRejectedAtAdmission) {
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            plugin::Bridge bridge{loop, group, http::Options{}, kWaitBudget};
            auto call = make_call();
            call->cancelled.store(true, std::memory_order_release);

            const auto response = co_await coro::offload_joined([&] { return bridge.exchange(call); });

            EXPECT_FALSE(response);
            EXPECT_EQ(response.error().code, YADDNSC_STATUS_CANCELLED);
        });
    };
    coro::run(loop, root());
}

// ===========================================================================
//  exchange() — dispatch failure without a cycle
// ===========================================================================

TEST(BridgeIngress, Exchange_WithoutCycle_PostFailureReportsInternalError) {
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            plugin::Bridge bridge{loop, group, http::Options{}, kWaitBudget};
            auto call = make_call();
            // call->cycle stays null: this is a direct Bridge user, so the
            // defect has no cycle to be recorded on.
            const auto response = co_await coro::offload_joined([&] {
                const FailNextAllocation failure;
                return bridge.exchange(call);
            });

            EXPECT_FALSE(response);
            EXPECT_EQ(response.error().code, YADDNSC_STATUS_INTERNAL_ERROR);
            EXPECT_EQ(response.error().message, "the request could not be dispatched");
            EXPECT_EQ(response.error().retry_after_seconds, 0u);
        });
    };
    coro::run(loop, root());
}
