//
// Coroutine runtime — current_time() reads the loop clock without exposing
// the Loop.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <chrono>
#include <utility>

#include <gtest/gtest.h>

#include "infrastructure/coro/coro.h"

namespace {

using namespace std::chrono_literals;

coro::Task<coro::TimePoint> read_time() {
    co_return co_await coro::current_time();
}

coro::Task<std::pair<coro::TimePoint, coro::TimePoint>> read_around_sleep(std::chrono::milliseconds delay) {
    const coro::TimePoint before = co_await coro::current_time();
    co_await coro::sleep_for(delay);
    const coro::TimePoint after = co_await coro::current_time();
    co_return {before, after};
}

}  // namespace

TEST(CurrentTime, current_time_InsideRun_YieldsTheLoopClock) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    clock.advance(42ms);

    coro::TimePoint observed{};
    coro::run(loop, [&]() -> coro::Task<void> {
        observed = co_await read_time();
        co_return;
    }());

    EXPECT_EQ(observed - coro::TimePoint{}, 42ms);
}

TEST(CurrentTime, current_time_AroundSleep_TracksClockAdvances) {
    coro::ManualClock clock;
    coro::Loop loop{clock};

    std::pair<coro::TimePoint, coro::TimePoint> observed{};
    coro::run(loop, [&]() -> coro::Task<void> {
        observed = co_await read_around_sleep(10ms);
        co_return;
    }());

    // The loop jumped the manual clock to the sleep's deadline in between.
    EXPECT_EQ(observed.second - observed.first, 10ms);
}
