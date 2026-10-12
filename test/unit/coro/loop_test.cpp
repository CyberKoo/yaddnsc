#include <gtest/gtest.h>

#include "coro/coro.h"

namespace {

coro::Task<int> forty_two() {
    co_return 42;
}

}  // namespace

TEST(Loop, LiveLoopsKeepInboxAndWakeStateIndependent) {
    coro::Loop first;
    coro::ManualClock clock;
    coro::Loop second{clock};
    bool first_posted = false;
    bool second_posted = false;
    first.post([&first_posted] { first_posted = true; });
    second.post([&second_posted] { second_posted = true; });
    EXPECT_EQ(coro::run(first, forty_two()), 42);
    EXPECT_TRUE(first_posted);
    EXPECT_FALSE(second_posted);
    EXPECT_EQ(coro::run(second, forty_two()), 42);
    EXPECT_TRUE(second_posted);
}

TEST(Loop, SequentialLoopsRunIndependently) {
    {
        coro::Loop first;
        EXPECT_EQ(coro::run(first, forty_two()), 42);
    }
    coro::Loop second;
    EXPECT_EQ(coro::run(second, forty_two()), 42);
}
