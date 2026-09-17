//
// Created by Kotarou on 2026/9/17.
//

#include "run_lifecycle.h"

#include "application/ports/clock.h"
#include "application/ports/log.h"
#include "application/ports/network_interfaces.h"
#include "application/ports/task_executor.h"

#include "support/fmt.hpp"
#include "support/util/cancellation_token.hpp"

RunLifecycle::RunLifecycle(std::shared_ptr<const domain::RuntimeConfig> config, std::stop_source stop_source,
                           const Utils::CancellationSource &cancellation, Clock &clock, TaskExecutor &executor,
                           const NetworkInterfaces &interfaces, const Logger &logger)
    : config_(std::move(config)),
      cancellation_(cancellation),
      clock_(clock),
      executor_(executor),
      interfaces_(interfaces),
      logger_(logger),
      queue_(config_, clock_.now()),
      runner_(queue_, clock_, executor_, stop_source.get_token(), logger_),
      stop_source_(std::move(stop_source)),
      stop_cb_(stop_source_.get_token(), [this] { cancellation_.trigger(); }) {
    // Rate-limit backoff: a task that fails with retry_after reports it back
    // to the runner, which moves the task's next deadline accordingly.
    executor_.set_retry_handler(
            [this](domain::TaskId id, std::chrono::seconds delay) { runner_.request_retry(id, delay); });
}

RunResult RunLifecycle::run() {
    YLOG_INFO(logger_, "All available interfaces: {}", fmt::join(interfaces_.names(), ", "));

    // Explicit shutdown sequence:
    //   1. stop is requested through stop_source_ (e.g. by SignalWatcher);
    //      the stop callback triggers the CancellationSource, aborting
    //      in-flight blocking I/O;
    //   2. the runner stops popping new tasks and returns;
    runner_.run();
    //   3. the executor stops accepting new tasks (pending work dropped);
    executor_.shutdown();
    //   4. in-flight updates drain before any driver instance may be
    //      destroyed or module unloaded by the caller's scope.
    executor_.wait_idle();
    YLOG_INFO(logger_, "All tasks drained, shutting down");
    return {};
}
