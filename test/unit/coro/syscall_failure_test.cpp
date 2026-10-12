// Isolated POSIX syscall fault injection; production code has no test hooks.
#include <cerrno>
#include <chrono>
#include <cstddef>

#include <gtest/gtest.h>
#include <poll.h>
#include <sys/types.h>

#include "coro/coro.h"

namespace {
int next_write_error = 0;
int next_poll_error = 0;
int write_calls = 0;
int poll_calls = 0;
}  // namespace

extern "C" ssize_t __real_write(int fd, const void* buffer, std::size_t size);
extern "C" int __real_poll(pollfd* fds, nfds_t count, int timeout);

extern "C" ssize_t __wrap_write(int fd, const void* buffer, std::size_t size) {
    ++write_calls;
    if (next_write_error != 0) {
        errno = next_write_error;
        next_write_error = 0;
        return -1;
    }
    return __real_write(fd, buffer, size);
}

extern "C" int __wrap_poll(pollfd* fds, nfds_t count, int timeout) {
    ++poll_calls;
    if (next_poll_error != 0) {
        errno = next_poll_error;
        next_poll_error = 0;
        return -1;
    }
    return __real_poll(fds, count, timeout);
}

namespace {

coro::Task<void> finish() {
    co_return;
}

TEST(CoroSyscallFailure, InterruptedWake_RetriesWithoutDroppingInboxCallback) {
    coro::Loop loop;
    bool delivered = false;
    write_calls = 0;
    next_write_error = EINTR;
    loop.post([&] { delivered = true; });
    EXPECT_EQ(write_calls, 2);
    EXPECT_EQ(next_write_error, 0);
    coro::run(loop, finish());
    EXPECT_TRUE(delivered);
}

TEST(CoroSyscallFailure, FullWakePipe_DoesNotTreatBackpressureAsFatal) {
    coro::Loop loop;
    bool delivered = false;
    write_calls = 0;
    next_write_error = EAGAIN;
    loop.post([&] { delivered = true; });
    EXPECT_EQ(write_calls, 1);
    EXPECT_EQ(next_write_error, 0);
    coro::run(loop, finish());
    EXPECT_TRUE(delivered);
}

TEST(CoroSyscallFailure, FailedWake_TerminatesRatherThanLosingDelivery) {
    EXPECT_DEATH(
        {
            coro::Loop loop;
            next_write_error = EIO;
            loop.post([] {});
        },
        "");
}

TEST(CoroSyscallFailure, InterruptedPoll_ReprocessesLoopAndEventuallyResumesWaiter) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto root = []() -> coro::Task<void> { co_await coro::sleep_for(std::chrono::milliseconds{1}); };
    poll_calls = 0;
    next_poll_error = EINTR;
    coro::run(loop, root());
    EXPECT_EQ(next_poll_error, 0);
    EXPECT_GE(poll_calls, 2);
    EXPECT_EQ(clock.now().time_since_epoch(), std::chrono::milliseconds{1});
}

TEST(CoroSyscallFailure, FailedPoll_TerminatesRatherThanSpinning) {
    EXPECT_DEATH(
        {
            coro::Loop loop;
            next_poll_error = ENOMEM;
            coro::run(loop, finish());
        },
        "");
}

}  // namespace
