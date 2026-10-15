#include "coro/stop_signals.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include <coroutine>

#include <gtest/gtest.h>
#include <unistd.h>

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

TEST(StopSignals, SigtermWakesAnAlreadyWaitingLoop) {
    const coro::StopSignals stop;
    coro::Loop loop;
    std::thread sender([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ::kill(::getpid(), SIGTERM);
    });
    coro::run(loop, stop.wait());
    sender.join();
    EXPECT_TRUE(stop.requested());
}

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
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        std::_Exit(2);
    }
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ::kill(::getpid(), SIGINT);
    }).detach();
    char byte = 0;
    [[maybe_unused]] const auto result = ::read(pipe_fds[0], &byte, 1);
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
