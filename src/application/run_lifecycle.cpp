//
// Created by Kotarou on 2026/9/17.
//

#include "run_lifecycle.h"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "application/ports/clock.h"
#include "application/ports/log.h"
#include "application/ports/network_interfaces.h"
#include "application/ports/task_executor.h"
#include "support/fmt.hpp"
#include "support/util/cancellation_token.hpp"

RunLifecycle::RunLifecycle(std::shared_ptr<const domain::RuntimeConfig> config,
                           ShutdownSignals shutdown,
                           RunEnvironment env)
    : config_(std::move(config)),
      shutdown_(std::move(shutdown)),
      env_(env),
      queue_(config_, env_.clock.now()),
      runner_(queue_, env_.clock, env_.executor, shutdown_.stop.get_token(), env_.logger),
      stop_cb_(shutdown_.stop.get_token(), [this] { shutdown_.cancellation.trigger(); }) {
    // Rate-limit backoff: a task that fails with retry_after reports it back
    // to the runner, which moves the task's next deadline accordingly.
    env_.executor.set_retry_handler(
        [this](domain::TaskId id, std::chrono::seconds delay) { runner_.request_retry(id, delay); });
}

RunResult RunLifecycle::run() {
    YLOG_INFO(env_.logger, "All available interfaces: {}", fmt::join(env_.interfaces.names(), ", "));

    // Explicit shutdown sequence:
    //   1. stop is requested through shutdown_.stop (e.g. by SignalWatcher);
    //      the stop callback triggers the CancellationSource, aborting
    //      in-flight blocking I/O;
    //   2. the runner stops popping new tasks and returns;
    runner_.run(shutdown_.cancellation.token());
    //   3. the executor stops accepting new tasks (pending work dropped);
    env_.executor.shutdown();
    //   4. in-flight updates drain before any driver instance may be
    //      destroyed or module unloaded by the caller's scope.
    env_.executor.wait_idle();
    YLOG_INFO(env_.logger, "All tasks drained, shutting down");
    return {};
}
