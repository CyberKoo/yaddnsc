//
// Component test for SteadyClock (src/infrastructure/time/steady_clock.cpp).
//
// wait_until() parks the caller on a condition variable until the deadline;
// wake() must interrupt every waiter immediately (the rate-limit retry path
// moves deadlines sooner and relies on this). Threads + real time, so this
// sits with the component tests rather than the pure-logic unit set; the
// cross-thread wakeup uses the same 30ms settle idiom as the poll_fd tests.
// =============================================================================

#include "infrastructure/time/steady_clock.h"

#include <atomic>
#include <chrono>
#include <stop_token>
#include <string>
#include <thread>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

// wake() with no waiter parked is a safe no-op.
TEST(SteadyClockTest, WakeWithoutWaiterIsNoOp) {
    SteadyClock clock;
    EXPECT_NO_THROW(clock.wake());
}

// A waiter parked on a far-future deadline must return promptly once wake()
// bumps the epoch — long before the deadline and without a stop request.
TEST(SteadyClockTest, WakeInterruptsWaiterBeforeDeadline) {
    SteadyClock clock;
    const std::stop_source stop;

    std::atomic<bool> returned{false};
    std::jthread waiter([&] {
        const bool ok = clock.wait_until(clock.now() + 10s, stop.get_token());
        EXPECT_TRUE(ok) << "wait must report success when woken, not stopped";
        returned.store(true, std::memory_order_release);
    });

    // Let the waiter park on the condition variable before waking it.
    std::this_thread::sleep_for(30ms);
    const auto start = std::chrono::steady_clock::now();
    clock.wake();

    for (int i = 0; i < 3000 && !returned.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(1ms);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(returned.load(std::memory_order_acquire)) << "wake() did not release the waiter";
    EXPECT_LT(elapsed, 3s) << "waiter ran to its deadline instead of waking early";
}
