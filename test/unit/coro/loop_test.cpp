#include <chrono>
#include <coroutine>
#include <functional>
#include <initializer_list>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include "coro/coro.h"
#include "coro/detail/pipe.h"
#include "support/util/fd.hpp"

namespace {

struct CountingClock final : coro::Clock {
    coro::ManualClock clock;
    mutable int reads = 0;

    coro::TimePoint now() const noexcept override {
        ++reads;
        return clock.now();
    }

    bool manual() const noexcept override { return true; }

    void jump_to(coro::TimePoint time) noexcept override { clock.jump_to(time); }
};

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
        auto [read_end, write_end] = coro::detail::make_pipe();
        ASSERT_TRUE(read_end);
        ASSERT_TRUE(write_end);
        const int fd = read_end.get();
        read_end.reset();
        std::error_code observed{};
        auto root = [fd, writable, &observed]() -> coro::Task<void> {
            // A closed fd resolves through the error_code channel, like every
            // other failure of the wait.
            observed = co_await (writable ? coro::wait_writable(fd) : coro::wait_readable(fd));
        };
        EXPECT_NO_THROW(coro::run(loop, root()));
        EXPECT_EQ(observed, std::make_error_code(std::errc::bad_file_descriptor));
    }
}

TEST(FdWait, Hangup_RemainsReadinessRatherThanAnInvalidDescriptor) {
    coro::Loop loop;
    auto [read_end, write_end] = coro::detail::make_pipe();
    ASSERT_TRUE(read_end);
    ASSERT_TRUE(write_end);
    write_end.reset();
    auto root = [fd = read_end.get()]() -> coro::Task<void> {
        // Hangup arrives as readiness: the wait resolves successfully.
        EXPECT_FALSE(co_await coro::wait_readable(fd));
    };
    EXPECT_NO_THROW(coro::run(loop, root()));
}

TEST(Loop, TimerFreeReadyWork_DoesNotReadClock) {
    CountingClock clock;
    coro::Loop loop{clock};
    auto root = []() -> coro::Task<void> {
        for (int i = 0; i < 8; ++i) {
            EXPECT_EQ(co_await forty_two(), 42);
            co_await coro::checkpoint();
        }
    };
    coro::run(loop, root());
    EXPECT_EQ(clock.reads, 0);
}

TEST(Loop, TimersAddedAfterTimerFreeTurns_StillAdvanceAndFire) {
    using namespace std::chrono_literals;
    CountingClock clock;
    coro::Loop loop{clock};
    auto root = [&]() -> coro::Task<void> {
        for (int i = 0; i < 2; ++i) {
            co_await coro::checkpoint();
            const int before = clock.reads;
            co_await coro::sleep_for(5ms);
            EXPECT_GT(clock.reads, before);
        }
    };
    coro::run(loop, root());
    EXPECT_EQ(clock.clock.now(), coro::TimePoint{} + 10ms);
}

TEST(Loop, SequentialLoopsRunIndependently) {
    {
        coro::Loop first;
        EXPECT_EQ(coro::run(first, forty_two()), 42);
    }
    coro::Loop second;
    EXPECT_EQ(coro::run(second, forty_two()), 42);
}
