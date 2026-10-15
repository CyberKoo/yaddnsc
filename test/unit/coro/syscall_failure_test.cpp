// Isolated POSIX syscall fault injection; production code has no test hooks.
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <system_error>

#include <coroutine>

#include <gtest/gtest.h>
#include <poll.h>
#include <sys/types.h>
#include <unistd.h>

#include "coro/coro.h"
#include "support/util/fd.hpp"

namespace {
int next_write_error = 0;
int next_poll_error = 0;
// Unlike next_poll_error, this one is not consumed: it models a sustained
// condition (e.g. memory pressure) rather than a transient fault.
int persistent_poll_error = 0;
int write_calls = 0;
int poll_calls = 0;
}  // namespace

// NOLINTNEXTLINE(bugprone-reserved-identifier)  // ld --wrap requires this exact symbol name
extern "C" ssize_t __real_write(int fd, const void* buffer, std::size_t size);
// NOLINTNEXTLINE(bugprone-reserved-identifier)  // ld --wrap requires this exact symbol name
extern "C" int __real_poll(pollfd* fds, nfds_t count, int timeout);

// NOLINTNEXTLINE(bugprone-reserved-identifier)  // ld --wrap requires this exact symbol name
extern "C" ssize_t __wrap_write(int fd, const void* buffer, std::size_t size) {
    ++write_calls;
    if (next_write_error != 0) {
        errno = next_write_error;
        next_write_error = 0;
        return -1;
    }
    return __real_write(fd, buffer, size);
}

// NOLINTNEXTLINE(bugprone-reserved-identifier)  // ld --wrap requires this exact symbol name
extern "C" int __wrap_poll(pollfd* fds, nfds_t count, int timeout) {
    ++poll_calls;
    if (persistent_poll_error != 0) {
        errno = persistent_poll_error;
        return -1;
    }
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

TEST(CoroSyscallFailure, TransientPollFailure_IsAbsorbedAndWaitStillCompletes) {
    coro::Loop loop;
    auto [read_end, write_end] = Utils::make_pipe();
    ASSERT_TRUE(read_end);
    ASSERT_TRUE(write_end);
    const char byte = 1;
    ASSERT_EQ(::write(write_end.get(), &byte, 1), 1);
    std::error_code observed = std::make_error_code(std::errc::io_error);  // any non-empty sentinel
    auto root = [&]() -> coro::Task<void> { observed = co_await coro::wait_readable(read_end.get()); };
    poll_calls = 0;
    persistent_poll_error = 0;
    next_poll_error = ENOMEM;  // one transient failure, then the real poll runs
    coro::run(loop, root());
    EXPECT_EQ(next_poll_error, 0);
    EXPECT_FALSE(observed);  // the wait survived the hiccup and resolved normally
    EXPECT_GE(poll_calls, 2);
}

TEST(CoroSyscallFailure, SustainedPollFailure_FailsWaitWithErrnoAndLoopSurvives) {
    coro::Loop loop;
    auto [read_end, write_end] = Utils::make_pipe();
    ASSERT_TRUE(read_end);
    ASSERT_TRUE(write_end);  // nothing written: only the shed failure can resolve the wait
    std::error_code observed;
    auto root = [&]() -> coro::Task<void> { observed = co_await coro::wait_readable(read_end.get()); };
    poll_calls = 0;
    persistent_poll_error = ENOMEM;
    coro::run(loop, root());
    persistent_poll_error = 0;
    // The wait failed with the real poll errno instead of killing the process,
    // and run() returned normally after the root task completed.
    EXPECT_EQ(observed, std::error_code(ENOMEM, std::generic_category()));
    EXPECT_GE(poll_calls, 8);  // shed threshold reached
}

TEST(CoroSyscallFailure, StructuralPollError_StaysFatal) {
    // EINVAL/EFAULT indict the loop's own call construction, a defect — the
    // fatal dispatch-failure path still applies to it.
    EXPECT_DEATH(
        {
            coro::Loop loop;
            next_poll_error = EINVAL;
            coro::run(loop, finish());
        },
        "");
}

}  // namespace
