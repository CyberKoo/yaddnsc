#include "coro/stop_signals.h"

#include <array>
#include <cerrno>
#include <coroutine>
#include <csignal>
#include <cstdlib>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>
#include <unistd.h>

#if defined(__linux__) && defined(YADDNSC_TEST_WRAP_STOP_POLL)
#include <poll.h>
#endif

#include "coro/cancel_scope.h"
#include "coro/loop.h"
#include "coro/run.hpp"
#include "coro/scope.hpp"

#ifdef YADDNSC_TEST_WRAP_SIGACTION
namespace {
int failing_signal = 0;
}  // namespace

// NOLINTNEXTLINE(bugprone-reserved-identifier)  // ld --wrap requires this exact symbol name
extern "C" int __real_sigaction(int signal, const struct sigaction* action, struct sigaction* previous) noexcept;

// NOLINTNEXTLINE(bugprone-reserved-identifier)  // ld --wrap requires this exact symbol name
extern "C" int __wrap_sigaction(int signal, const struct sigaction* action, struct sigaction* previous) noexcept {
    if (action != nullptr && signal == failing_signal) {
        failing_signal = 0;
        errno = EIO;
        return -1;
    }
    return __real_sigaction(signal, action, previous);
}
#endif

#if defined(__linux__) && defined(YADDNSC_TEST_WRAP_STOP_POLL)
namespace {
struct PollBoundaryProbe {
    bool entered = false;
    bool interrupted = false;
    bool woke_after_interrupt = false;
};

PollBoundaryProbe* poll_boundary_probe = nullptr;
}  // namespace

// NOLINTNEXTLINE(bugprone-reserved-identifier)  // ld --wrap requires this exact symbol name
extern "C" int __real_poll(pollfd* fds, nfds_t count, int timeout);

// NOLINTNEXTLINE(bugprone-reserved-identifier)  // ld --wrap requires this exact symbol name
extern "C" int __wrap_poll(pollfd* fds, nfds_t count, int timeout) {
    auto* probe = poll_boundary_probe;
    if (probe == nullptr || probe->entered || timeout == 0) {
        const int result = __real_poll(fds, count, timeout);
        if (probe != nullptr && probe->interrupted && result > 0) {
            probe->woke_after_interrupt = true;
        }
        return result;
    }
    probe->entered = true;
    EXPECT_EQ(timeout, -1);
    EXPECT_GE(count, 2u);  // Loop self-pipe plus the registered stop pipe.
    EXPECT_EQ(__real_poll(fds, count, 0), 0);

    sigset_t blocked{};
    sigset_t previous{};
    ::sigemptyset(&blocked);
    ::sigaddset(&blocked, SIGTERM);
    if (::sigprocmask(SIG_BLOCK, &blocked, &previous) != 0 || ::raise(SIGTERM) != 0) {
        ADD_FAILURE() << "Could not queue a masked SIGTERM";
        std::abort();
    }
    sigset_t pending{};
    EXPECT_EQ(::sigpending(&pending), 0);
    EXPECT_EQ(::sigismember(&pending, SIGTERM), 1);

    // Only the kernel wait atomically unmasks the pending signal. A flag set
    // before poll cannot prove this boundary; ppoll's EINTR result does.
    sigset_t unblocked = previous;
    ::sigdelset(&unblocked, SIGTERM);
    const int result = ::ppoll(fds, count, nullptr, &unblocked);
    const int error = errno;
    probe->interrupted = result == -1 && error == EINTR;
    if (::sigprocmask(SIG_SETMASK, &previous, nullptr) != 0) {
        ADD_FAILURE() << "Could not restore signal mask";
        std::abort();
    }
    errno = error;
    return result;
}
#endif

namespace {

#ifdef YADDNSC_TEST_WRAP_SIGACTION
TEST(StopSignals, HandlerInstallationFailureRestoresDispositionsAndReleasesOwnership) {
    const std::array signals{SIGINT, SIGTERM};
    for (const int failure : signals) {
        SCOPED_TRACE(failure);
        std::array<struct sigaction, 2> before{};
        for (std::size_t i = 0; i < signals.size(); ++i) {
            ASSERT_EQ(::sigaction(signals[i], nullptr, &before[i]), 0);
        }

        failing_signal = failure;
        EXPECT_THROW(coro::StopSignals{}, std::system_error);
        EXPECT_EQ(failing_signal, 0);
        failing_signal = 0;

        for (std::size_t i = 0; i < signals.size(); ++i) {
            SCOPED_TRACE(signals[i]);
            struct sigaction after{};
            ASSERT_EQ(::sigaction(signals[i], nullptr, &after), 0);
            EXPECT_EQ(before[i].sa_handler, after.sa_handler);
            EXPECT_EQ(before[i].sa_flags, after.sa_flags);
            for (int signal = 1; signal < NSIG; ++signal) {
                EXPECT_EQ(::sigismember(&before[i].sa_mask, signal), ::sigismember(&after.sa_mask, signal));
            }
        }
        const coro::StopSignals next;
        EXPECT_FALSE(next.requested());
    }
}
#endif

TEST(StopSignals, EarlierSigintCompletesWaitWithoutArmingAnotherHandler) {
    const coro::StopSignals stop;
    EXPECT_FALSE(stop.requested());
    ::kill(::getpid(), SIGINT);
    EXPECT_TRUE(stop.requested());
    coro::Loop loop;
    EXPECT_NO_THROW(coro::run(loop, stop.wait()));
}

TEST(StopSignals, SigtermAfterFdRegistrationCompletesWait) {
    const coro::StopSignals stop;
    coro::Loop loop;
    bool registered = false;
    // The self-pipe was registered in Loop's constructor, before this sink.
    // add_fd emits this trace only after committing the stop wait registration.
    loop.set_trace_sink(
        [](void* context, std::string_view message, const std::source_location& where) noexcept {
            if (std::string_view(where.function_name()).find("add_fd") != std::string_view::npos &&
                message.starts_with("Registered fd ")) {
                *static_cast<bool*>(context) = true;
                ::kill(::getpid(), SIGTERM);
            }
        },
        &registered);
    coro::run(loop, stop.wait());
    EXPECT_TRUE(registered);
    EXPECT_TRUE(stop.requested());
}

#if defined(__linux__) && defined(YADDNSC_TEST_WRAP_STOP_POLL)
TEST(StopSignals, SigtermAtKernelWaitBoundaryInterruptsPollAndWakesLoop) {
    const coro::StopSignals stop;
    coro::Loop loop;
    PollBoundaryProbe probe;

    struct ResetProbe {
        ~ResetProbe() noexcept { poll_boundary_probe = nullptr; }
    } reset;

    poll_boundary_probe = &probe;

    EXPECT_FALSE(stop.requested());
    EXPECT_NO_THROW(coro::run(loop, stop.wait()));
    EXPECT_TRUE(probe.entered);
    EXPECT_TRUE(probe.interrupted);
    EXPECT_TRUE(probe.woke_after_interrupt);
    EXPECT_TRUE(stop.requested());
}
#endif

TEST(StopSignals, LatchedSignal_StillObservesCallerCancellation) {
    const coro::StopSignals stop;
    ::kill(::getpid(), SIGTERM);
    ASSERT_TRUE(stop.requested());
    bool after_wait = false;
    auto root = [&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
            scope.cancel();
            co_await stop.wait();
            after_wait = true;
        });
        EXPECT_TRUE(outcome.cancelled);
        EXPECT_FALSE(outcome.completed);
    };
    coro::run(root());
    EXPECT_FALSE(after_wait);
}

TEST(StopSignals, RepeatedSigtermKeepsTheStopLatched) {
    const coro::StopSignals stop;
    ::kill(::getpid(), SIGTERM);
    ::kill(::getpid(), SIGTERM);
    coro::Loop loop;
    coro::run(loop, stop.wait());
    EXPECT_TRUE(stop.requested());
}

void two_sigints_while_blocked() {
    const coro::StopSignals stop;
    ::kill(::getpid(), SIGINT);
    sigset_t blocked{};
    sigset_t previous{};
    ::sigemptyset(&blocked);
    ::sigaddset(&blocked, SIGINT);
    if (::sigprocmask(SIG_BLOCK, &blocked, &previous) != 0 || ::kill(::getpid(), SIGINT) != 0) {
        std::_Exit(2);
    }
    sigset_t pending{};
    if (::sigpending(&pending) != 0 || ::sigismember(&pending, SIGINT) != 1) {
        std::_Exit(2);
    }
    // The pending second signal cannot run until this blocking syscall
    // atomically unmasks it; no sender scheduling or pre-call flag is involved.
    ::sigdelset(&previous, SIGINT);
    ::sigsuspend(&previous);
    std::_Exit(3);
}

TEST(StopSignals, SecondSigintTerminatesEvenDuringBlockingStartup) {
    EXPECT_EXIT(two_sigints_while_blocked(), ::testing::ExitedWithCode(128 + SIGINT), "");
}

TEST(StopSignals, RestoresHandlersAndAllowsTheNextRun) {
    struct sigaction before{};
    struct sigaction after{};
    ASSERT_EQ(::sigaction(SIGTERM, nullptr, &before), 0);
    {
        const coro::StopSignals stop;
        ::kill(::getpid(), SIGTERM);
        EXPECT_TRUE(stop.requested());
        EXPECT_THROW(coro::StopSignals{}, std::logic_error);
    }
    ASSERT_EQ(::sigaction(SIGTERM, nullptr, &after), 0);
    EXPECT_EQ(before.sa_handler, after.sa_handler);
    const coro::StopSignals next;
    EXPECT_FALSE(next.requested());
}

}  // namespace
