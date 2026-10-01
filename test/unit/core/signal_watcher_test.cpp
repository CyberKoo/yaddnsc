//
// SignalWatcher unit tests.
//
// NOTE: install() blocks SIGINT/SIGTERM on the calling thread for the rest of
// the process — this binary only runs these tests, so that is safe here.
// =============================================================================

#include "infrastructure/process/signal_watcher.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <compare>
#include <csignal>
#include <stdexcept>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <unistd.h>

#ifdef YADDNSC_TEST_WRAP_SIGWAIT
namespace {
std::atomic<bool> fail_sigwait{false};
}

extern "C" int __real_sigwait(const sigset_t* set, int* signal);

extern "C" int __wrap_sigwait(const sigset_t* set, int* signal) {
    if (fail_sigwait.exchange(false)) {
        return EINVAL;  // Deliberately leave the output parameter untouched.
    }
    return __real_sigwait(set, signal);
}
#endif

namespace {
/// Poll until the watcher requests a stop (or the timeout elapses).
[[nodiscard]] bool wait_for_stop(SignalWatcher& watcher, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (watcher.get_stop_source().stop_requested()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
}  // namespace

// The "not installed" path lives in its OWN test binary
// (core/signal_watcher_uninstalled_test.cpp) and is deliberately not here.
//
// install() blocks SIGINT/SIGTERM for the process and cannot be undone, so
// the guard it sets is a one-way latch. Asserting the un-installed path in
// this file would make the assertion depend on running before every other
// case in the binary — an implicit ordering constraint that a shuffle, a
// gtest_filter change, or a new test would silently break. A separate
// executable gets a fresh process image, so the case is order-independent.

#ifdef YADDNSC_TEST_WRAP_SIGWAIT
TEST(SignalWatcher, Sigwait_FailsWithoutWritingSignal_RequestsStop) {
    SignalWatcher::install();
    fail_sigwait.store(true);
    SignalWatcher watcher;
    EXPECT_TRUE(wait_for_stop(watcher));
}
#endif

TEST(SignalWatcher, InstallThenConstruct_NoThrow) {
    SignalWatcher::install();
    EXPECT_NO_THROW({ SignalWatcher watcher; });
}

TEST(SignalWatcher, StopSourceIsFunctional) {
    SignalWatcher::install();
    SignalWatcher watcher;
    auto stop_source = watcher.get_stop_source();
    EXPECT_TRUE(stop_source.stop_possible());
    EXPECT_FALSE(stop_source.stop_requested());

    stop_source.request_stop();
    EXPECT_TRUE(stop_source.stop_requested());
}

TEST(SignalWatcher, CanRecreateAfterDestruction) {
    SignalWatcher::install();
    {
        SignalWatcher watcher;
        EXPECT_TRUE(watcher.get_stop_source().stop_possible());
    }
    // The singleton guard must be released on destruction — creating a second
    // watcher after the first is destroyed must succeed.
    SignalWatcher watcher2;
    EXPECT_TRUE(watcher2.get_stop_source().stop_possible());
}

TEST(SignalWatcher, Sigusr2IsIgnored) {
    SignalWatcher::install();
    SignalWatcher watcher;

    // SIGUSR2 is reserved as the destructor's wake-up signal and must never
    // be treated as a user signal: delivering it must not trigger a stop
    // request. (Without the SIGUSR2 reservation this delivery would instead
    // terminate the test process via the signal's default action.)
    kill(getpid(), SIGUSR2);
    // Give the watcher thread a moment to consume the signal.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(watcher.get_stop_source().stop_requested());
}

TEST(SignalWatcher, SecondInstance_Throws) {
    SignalWatcher::install();
    SignalWatcher watcher;
    // Only one instance may exist at a time.
    EXPECT_THROW(SignalWatcher{}, std::logic_error);
}

TEST(SignalWatcher, FirstSigint_RequestsStop) {
    SignalWatcher::install();
    SignalWatcher watcher;

    kill(getpid(), SIGINT);
    EXPECT_TRUE(wait_for_stop(watcher)) << "first SIGINT must initiate graceful shutdown";
}

TEST(SignalWatcher, SecondSigint_ForcesImmediateTermination) {
    // Death test: the child presses Ctrl-C twice; the second SIGINT must
    // terminate the process directly with the conventional 128+SIGINT status
    // (escalating through the blocked SIGTERM would be a no-op).
    EXPECT_EXIT(
        {
            SignalWatcher::install();
            SignalWatcher watcher;

            kill(getpid(), SIGINT);
            if (!wait_for_stop(watcher)) {
                ::_exit(1);  // graceful stop did not latch
            }
            kill(getpid(), SIGINT);

            // Surviving past this point means the escalation did not fire.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            ::_exit(2);
        },
        ::testing::ExitedWithCode(128 + SIGINT), "");
}

TEST(SignalWatcher, ExternalSigterm_RequestsStop) {
    SignalWatcher::install();
    SignalWatcher watcher;

    kill(getpid(), SIGTERM);
    EXPECT_TRUE(wait_for_stop(watcher)) << "SIGTERM must request a graceful shutdown";
}
