//
// Coroutine runtime — one live loop per process, enforced by the constructor:
// the signal handler's file-scope state (loop.cpp) cannot serve two loops, so
// a second live loop fails loudly instead of misrouting the first loop's
// signals.
//

#include <stdexcept>

#include <gtest/gtest.h>

#include "coro/coro.h"

namespace {

coro::Task<int> forty_two() {
    co_return 42;
}

}  // namespace

TEST(Loop, SecondLiveLoop_ConstructionThrows_FirstStaysUsable) {
    coro::Loop first;
    EXPECT_THROW(coro::Loop second, std::logic_error);
    coro::ManualClock clock;
    EXPECT_THROW(coro::Loop with_clock(clock), std::logic_error);
    // The failed constructions leave the first loop fully functional.
    EXPECT_EQ(coro::run(first, forty_two()), 42);
}

TEST(Loop, SequentialLoops_ReacquireTheSlot) {
    {
        coro::Loop first;
        EXPECT_EQ(coro::run(first, forty_two()), 42);
    }
    coro::Loop second;
    EXPECT_EQ(coro::run(second, forty_two()), 42);
}
