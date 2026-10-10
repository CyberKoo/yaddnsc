//
// composition — stop-signal bookkeeping outside the event loop's watch.
//
// The loop's signal watcher only exists while the loop runs. Before it (config
// load, dlopen, TLS context build) and after it (drain, dlclose) a minimal
// counting handler covers the gap: it records arrivals asynchronously, the
// blocking startup steps check the counters between steps, the run root folds
// them into its shutdown state, and the final drain checks once more so a late
// Ctrl-C is never swallowed. The Loop's destructor restores this handler, which
// is what keeps the tail covered.
//

#ifndef YADDNSC_COMPOSITION_STARTUP_SIGNALS_H
#define YADDNSC_COMPOSITION_STARTUP_SIGNALS_H

#include "application/services.h"

namespace Composition {

/// Install the counting handler for SIGINT/SIGTERM. Idempotent enough for the
/// single run command; never restored to the default (the process exits with
/// the counters in place).
void install_startup_signal_handlers();

/// Snapshot of the signals observed so far.
[[nodiscard]] app::StartupSignalCounts startup_signal_counts() noexcept;

}  // namespace Composition

#endif  // YADDNSC_COMPOSITION_STARTUP_SIGNALS_H
