//
// SignalWatcher unit tests.
//
// NOTE: install() blocks SIGINT/SIGTERM on the calling thread for the rest of
// the process — this binary only runs these tests, so that is safe here.
// =============================================================================

#include "core/signal_watcher.h"

#include <chrono>
#include <csignal>
#include <thread>

#include <gtest/gtest.h>
#include <unistd.h>

namespace {
    /// Poll until the watcher requests a stop (or the timeout elapses).
    [[nodiscard]] bool wait_for_stop(SignalWatcher &watcher,
                                     std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (watcher.get_stop_source().stop_requested()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
} // namespace

// MUST be the first test in this binary: install() blocks SIGINT/SIGTERM
// process-wide and cannot be undone, so the "not installed" path can only be
// exercised before any install() call.
TEST(SignalWatcherUninstalled, ConstructWithoutInstall_Throws) {
    EXPECT_THROW(SignalWatcher{}, std::logic_error);
}

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

TEST(SignalWatcher, SecondSigint_EscalatesWithoutCrash) {
    SignalWatcher::install();
    SignalWatcher watcher;

    kill(getpid(), SIGINT);
    ASSERT_TRUE(wait_for_stop(watcher));

    // Second SIGINT escalates: the watcher sends SIGTERM to itself.  SIGTERM
    // is blocked (install()) and consumed by the watcher — the process must
    // survive and the stop state must remain set.
    kill(getpid(), SIGINT);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(watcher.get_stop_source().stop_requested());
}

TEST(SignalWatcher, ExternalSigterm_RequestsStop) {
    SignalWatcher::install();
    SignalWatcher watcher;

    kill(getpid(), SIGTERM);
    EXPECT_TRUE(wait_for_stop(watcher)) << "SIGTERM must request a graceful shutdown";
}
