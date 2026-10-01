//
// The un-installed path of SignalWatcher, in its own process.
//
// SignalWatcher::install() blocks SIGINT/SIGTERM and sets a guard that is a
// one-way latch — it cannot be undone. Asserting the "not installed" path
// alongside the installed cases would therefore make the assertion depend on
// running before all of them: an implicit ordering constraint that a shuffle,
// a gtest_filter change, or one added test would silently break.
//
// A separate executable gets a fresh process image, so the case is
// order-independent and repeatable. Do NOT merge this file back into
// signal_watcher_test.cpp.
// =============================================================================

#include <stdexcept>

#include <gtest/gtest.h>

#include "infrastructure/process/signal_watcher.h"

TEST(SignalWatcherUninstalled, ConstructWithoutInstall_Throws) {
    EXPECT_THROW(SignalWatcher{}, std::logic_error);
}
