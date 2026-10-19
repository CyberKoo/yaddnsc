// Isolated POSIX syscall fault injection; production code has no test hooks.
#include <array>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <new>
#include <string>
#include <system_error>

#include <gtest/gtest.h>
#include <poll.h>
#include <sys/types.h>
#include <unistd.h>

#include "coro/coro.h"
#include "coro/detail/access.h"
#include "coro/detail/fd_entry.h"
#include "coro/detail/pipe.h"
#include "mocks/allocation_fault.h"
#include "support/util/fd.hpp"

namespace {
int next_write_error = 0;
int next_poll_error = 0;
// Unlike next_poll_error, this one is not consumed: it models a sustained
// condition (e.g. memory pressure) rather than a transient fault.
int persistent_poll_error = 0;
int write_calls = 0;
int poll_calls = 0;
bool fail_allocations_on_poll = false;
int allocation_attempts = 0;

void fail_allocation(std::size_t) {
    ++allocation_attempts;
    throw std::bad_alloc{};
}
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
        if (fail_allocations_on_poll) {
            test::set_allocation_fault_hook(&fail_allocation);
        }
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
    auto [read_end, write_end] = coro::detail::make_pipe();
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
    auto [read_end, write_end] = coro::detail::make_pipe();
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

TEST(CoroSyscallFailure, SustainedPollFailure_AllocationUnavailable_FailsWaitWithoutAllocating) {
    coro::Loop loop;
    auto [read_end, write_end] = coro::detail::make_pipe();
    ASSERT_TRUE(read_end);
    ASSERT_TRUE(write_end);
    std::error_code observed;
    auto root = [&]() -> coro::Task<void> { observed = co_await coro::wait_readable(read_end.get()); };
    poll_calls = 0;
    allocation_attempts = 0;
    persistent_poll_error = ENOMEM;
    fail_allocations_on_poll = true;
    coro::run(loop, root());
    test::set_allocation_fault_hook(nullptr);
    fail_allocations_on_poll = false;
    persistent_poll_error = 0;
    EXPECT_EQ(allocation_attempts, 0);
    EXPECT_EQ(observed, std::error_code(ENOMEM, std::generic_category()));
    EXPECT_GE(poll_calls, 8);
}

TEST(CoroSyscallFailure, Dispatch_CallbackRegistersDescriptors_PreservesRemainingSnapshot) {
    for (const int error : {0, ENOMEM}) {
        coro::Loop loop;
        auto [read_end, write_end] = coro::detail::make_pipe();
        ASSERT_TRUE(read_end);
        ASSERT_TRUE(write_end);
        const char byte = 1;
        ASSERT_EQ(::write(write_end.get(), &byte, 1), 1);

        coro::Notification completed;

        struct Callbacks {
            coro::Loop& loop;
            coro::Notification& completed;
            std::array<coro::detail::FdEntry, 32> initial{};
            std::array<coro::detail::FdEntry, 256> added{};
            std::size_t fired = 0;
            bool failed = false;

            static void fire(void* context, short) noexcept {
                auto& self = *static_cast<Callbacks*>(context);
                if (++self.fired == 1) {
                    // Grow both dispatch buffers while later callbacks remain
                    // in the current snapshot. Registration does not dispatch.
                    try {
                        for (auto& entry : self.added) {
                            entry.fd = self.initial.front().fd;
                            entry.events = POLLIN;
                            entry.fn = &Callbacks::fire;
                            entry.context = &self;
                            coro::detail::LoopAccess::add_fd(self.loop, entry);
                        }
                    } catch (...) {
                        self.failed = true;
                        self.completed.notify();
                        return;
                    }
                }
                if (self.fired == self.initial.size()) {
                    // Finish this dispatch experiment before the root's queued
                    // retirement; later poll phases must not start another round.
                    for (auto& entry : self.initial) {
                        coro::detail::LoopAccess::remove_fd(self.loop, entry);
                    }
                    for (auto& entry : self.added) {
                        coro::detail::LoopAccess::remove_fd(self.loop, entry);
                    }
                    self.completed.notify();
                }
            }
        } callbacks{loop, completed};

        for (auto& entry : callbacks.initial) {
            entry.fd = read_end.get();
            entry.events = POLLIN;
            entry.fn = &Callbacks::fire;
            entry.context = &callbacks;
            coro::detail::LoopAccess::add_fd(loop, entry);
        }
        persistent_poll_error = error;
        coro::run(loop, [&completed]() -> coro::Task<void> { co_await completed.wait(); }());
        persistent_poll_error = 0;
        for (auto& entry : callbacks.initial) {
            coro::detail::LoopAccess::remove_fd(loop, entry);
        }
        for (auto& entry : callbacks.added) {
            coro::detail::LoopAccess::remove_fd(loop, entry);
        }
        EXPECT_FALSE(callbacks.failed);
        EXPECT_EQ(callbacks.fired, callbacks.initial.size()) << "poll errno: " << error;
    }
}

TEST(CoroSyscallFailure, Dispatch_UnregisteringAndDestroyingPeer_RevokesOnlyItsSnapshot) {
    for (const int error : {0, ENOMEM}) {
        SCOPED_TRACE(error);
        coro::Loop loop;
        auto [read_end, write_end] = coro::detail::make_pipe();
        ASSERT_TRUE(read_end);
        ASSERT_TRUE(write_end);
        const char byte = 1;
        ASSERT_EQ(::write(write_end.get(), &byte, 1), 1);

        coro::Notification completed;

        struct Callbacks {
            coro::Loop& loop;
            coro::Notification& completed;
            coro::detail::FdEntry first{};
            std::unique_ptr<coro::detail::FdEntry> victim = std::make_unique<coro::detail::FdEntry>();
            coro::detail::FdEntry survivor{};
            int first_calls = 0;
            int peer_calls = 0;

            static void remove_peer(void* context, short) noexcept {
                auto& self = *static_cast<Callbacks*>(context);
                ++self.first_calls;
                coro::detail::LoopAccess::remove_fd(self.loop, self.first);
                coro::detail::LoopAccess::remove_fd(self.loop, *self.victim);
                self.victim.reset();
                // Wake independently of the survivor: over-invalidating the
                // snapshot must fail a count assertion rather than hang.
                self.completed.notify();
            }

            static void fire_peer(void* context, short) noexcept {
                auto& self = *static_cast<Callbacks*>(context);
                ++self.peer_calls;
                coro::detail::LoopAccess::remove_fd(self.loop, self.survivor);
            }
        } callbacks{loop, completed};

        callbacks.first = {
            .fd = read_end.get(), .events = POLLIN, .fn = &Callbacks::remove_peer, .context = &callbacks};
        // Both peers deliberately share fd, callback and context. Revocation
        // must identify the registration, not any of those borrowed values.
        *callbacks.victim = {
            .fd = read_end.get(), .events = POLLIN, .fn = &Callbacks::fire_peer, .context = &callbacks};
        callbacks.survivor = {
            .fd = read_end.get(), .events = POLLIN, .fn = &Callbacks::fire_peer, .context = &callbacks};
        coro::detail::LoopAccess::add_fd(loop, callbacks.first);
        coro::detail::LoopAccess::add_fd(loop, *callbacks.victim);
        coro::detail::LoopAccess::add_fd(loop, callbacks.survivor);
        persistent_poll_error = error;
        coro::run(loop, [&completed]() -> coro::Task<void> { co_await completed.wait(); }());
        persistent_poll_error = 0;
        coro::detail::LoopAccess::remove_fd(loop, callbacks.first);
        coro::detail::LoopAccess::remove_fd(loop, callbacks.survivor);
        if (callbacks.victim != nullptr) {
            coro::detail::LoopAccess::remove_fd(loop, *callbacks.victim);
        }
        EXPECT_EQ(callbacks.first_calls, 1);
        EXPECT_EQ(callbacks.peer_calls, 1);
        EXPECT_EQ(callbacks.victim, nullptr);
    }
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
