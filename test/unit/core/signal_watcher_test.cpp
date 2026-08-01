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
