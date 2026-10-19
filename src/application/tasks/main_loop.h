#ifndef YADDNSC_APPLICATION_TASKS_MAIN_LOOP_H
#define YADDNSC_APPLICATION_TASKS_MAIN_LOOP_H

#include <memory>

#include "coro/task.hpp"

namespace domain {
struct RuntimeConfig;
}

namespace app {
struct RuntimeServices;

/// How the run ended. The process exit status is the command handler's to
/// choose — the loop reports which of these happened, not how the OS should
/// treat it.
enum class MainLoopOutcome {
    /// A stop request ended the run: the work drained cleanly.
    GracefulShutdown,
    /// Startup validation rejected the configuration, so nothing was
    /// published against it.
    ConfigurationRejected,
    /// Every subdomain task retired without a stop request.
    UpdateTasksEnded,
};

/// Validate driver parameters, then run one supervised task per subdomain.
/// A stop request cancels the work scope; the process adapter handles repeated
/// Ctrl-C throughout startup, execution and drain. Already-latched requests
/// prevent work from starting. Root-child defects cancel the run and propagate;
/// subdomain defects remain isolated by the nested supervisor group.
/// Gateway and service borrows remain alive until root children have drained.
/// Returns the named outcome; ancestor cancellation propagates. Call inside
/// coro::run with both stop callbacks installed.
[[nodiscard]] coro::Task<MainLoopOutcome> main_loop(std::shared_ptr<const domain::RuntimeConfig> config,
                                                    RuntimeServices services);

}  // namespace app

#endif  // YADDNSC_APPLICATION_TASKS_MAIN_LOOP_H
