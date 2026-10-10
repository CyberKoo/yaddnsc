//
// composition — stop-signal bookkeeping outside the event loop's watch
// (implementation).
//

#include "startup_signals.h"

#include <csignal>
#include <atomic>

namespace {

/// Lock-free on every supported target (the loop's own signal handler relies on
/// the same property), which is what makes the handler async-signal-safe.
std::atomic<int> g_sigint_count{0};
std::atomic<int> g_sigterm_count{0};

/// Async-signal-safe: increments a counter and nothing else. SA_RESTART lets
/// the interrupted startup step finish; the caller checks the counters between
/// steps, so shutdown stays graceful instead of half-completing file I/O.
extern "C" void count_startup_signal(int sig) {
    if (sig == SIGINT) {
        g_sigint_count.fetch_add(1, std::memory_order_relaxed);
    } else if (sig == SIGTERM) {
        g_sigterm_count.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

void Composition::install_startup_signal_handlers() {
    struct sigaction action {};
    action.sa_handler = &count_startup_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);
}

app::StartupSignalCounts Composition::startup_signal_counts() noexcept {
    return {.sigint = g_sigint_count.load(std::memory_order_relaxed),
            .sigterm = g_sigterm_count.load(std::memory_order_relaxed)};
}
