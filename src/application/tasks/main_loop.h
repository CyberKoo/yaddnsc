#ifndef YADDNSC_APPLICATION_TASKS_MAIN_LOOP_H
#define YADDNSC_APPLICATION_TASKS_MAIN_LOOP_H

#include <memory>

#include "coro/task.hpp"

namespace domain {
struct RuntimeConfig;
}

namespace app {
struct RuntimeServices;

/// Validate driver parameters, then run one supervised task per subdomain.
/// A stop request cancels the work scope; the process adapter handles repeated
/// Ctrl-C throughout startup, execution and drain. Already-latched requests
/// prevent work from starting. Root-child defects cancel the run and propagate;
/// subdomain defects remain isolated by the nested supervisor group.
/// Gateway and service borrows remain alive until root children have drained.
/// Returns EXIT_SUCCESS on graceful shutdown and EXIT_FAILURE on configuration
/// rejection or an unexpected end of all update tasks. Ancestor cancellation
/// propagates. Call inside coro::run with both stop callbacks installed.
[[nodiscard]] coro::Task<int> main_loop(std::shared_ptr<const domain::RuntimeConfig> config, RuntimeServices services);

}  // namespace app

#endif  // YADDNSC_APPLICATION_TASKS_MAIN_LOOP_H
