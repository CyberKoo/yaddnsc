#include <stdexcept>

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

TEST(FdWait, ClosedNonnegativeDescriptor_IsRejectedInBothDirections) {
    for (const bool writable : {false, true}) {
        coro::Loop loop;
        auto [read_end, write_end] = Utils::make_pipe();
        ASSERT_TRUE(read_end);
        ASSERT_TRUE(write_end);
        const int fd = read_end.get();
        read_end.reset();
        auto root = [fd, writable]() -> coro::Task<void> {
            co_await (writable ? coro::wait_writable(fd) : coro::wait_readable(fd));
        };
        EXPECT_THROW(coro::run(loop, root()), std::invalid_argument);
    }
}

TEST(FdWait, Hangup_RemainsReadinessRatherThanAnInvalidDescriptor) {
    coro::Loop loop;
    auto [read_end, write_end] = Utils::make_pipe();
    ASSERT_TRUE(read_end);
    ASSERT_TRUE(write_end);
    write_end.reset();
    auto root = [fd = read_end.get()]() -> coro::Task<void> { co_await coro::wait_readable(fd); };
    EXPECT_NO_THROW(coro::run(loop, root()));
}

TEST(Loop, SequentialLoopsRunIndependently) {
    {
        coro::Loop first;
        EXPECT_EQ(coro::run(first, forty_two()), 42);
    }
    coro::Loop second;
    EXPECT_EQ(coro::run(second, forty_two()), 42);
}
