//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_APPLICATION_RUN_LIFECYCLE_H
#define YADDNSC_APPLICATION_RUN_LIFECYCLE_H

#include <functional>
#include <memory>
#include <stop_token>

#include "application/scheduler_runner.h"

#include "domain/update/schedule_queue.h"

class Clock;
class TaskExecutor;
class NetworkInterfaces;
class Logger;

namespace Utils {
    class CancellationSource;
}

namespace domain {
    struct RuntimeConfig;
}

/// Result of one `run` execution. Currently a unit result: the run either
/// completes the shutdown sequence normally or never returns (fatal errors
/// escape as exceptions before the loop starts).
struct RunResult {};

/// RunLifecycle — the `run` command's application lifecycle object.
///
/// The composition root owns every concrete component (catalog, dispatcher,
/// gateway, workflow, executor, clock, logger) and hands this object the
/// ports it needs; this object owns the scheduling loop and the shutdown
/// sequence:
///
///   construction
///   → bind stop_source to BOTH scheduler stop (stop token into the runner)
///     and I/O cancellation (stop callback triggers the CancellationSource)
///   → build the schedule queue
///   run()
///   → SchedulerRunner pops due tasks until stop is requested
///   → TaskExecutor stops accepting new tasks (pending work is dropped —
///     shutdown discards any rescheduling by design)
///   → in-flight tasks drain (wait_idle)
///   → return RunResult; the caller's scope then releases the driver catalog
///
/// The shutdown steps are explicit statements in run(), never an emergent
/// property of member destruction order.
class RunLifecycle {
public:
    /// All references must outlive the lifecycle object (they do: the
    /// composition root owns every component on the stack of the run path).
    /// The stop → I/O-cancel binding is registered here so that a stop
    /// requested before run() still cancels blocking I/O.
    RunLifecycle(std::shared_ptr<const domain::RuntimeConfig> config, std::stop_source stop_source,
                 std::shared_ptr<Utils::CancellationSource> cancel_source, Clock &clock, TaskExecutor &executor,
                 const NetworkInterfaces &interfaces, const Logger &logger);

    /// Drive the scheduling loop until stop is requested, then shut down in
    /// the explicit order documented above. Blocks; call from the run thread.
    RunResult run();

private:
    std::shared_ptr<const domain::RuntimeConfig> config_;
    std::shared_ptr<Utils::CancellationSource> cancel_source_;
    Clock &clock_;
    TaskExecutor &executor_;
    const NetworkInterfaces &interfaces_;
    const Logger &logger_;
    domain::ScheduleQueue queue_;
    SchedulerRunner runner_;
    std::stop_source stop_source_;
    std::stop_callback<std::function<void()>> stop_cb_;
};

#endif // YADDNSC_APPLICATION_RUN_LIFECYCLE_H
