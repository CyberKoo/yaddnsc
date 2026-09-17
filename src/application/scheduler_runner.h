//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_APPLICATION_SCHEDULER_RUNNER_H
#define YADDNSC_APPLICATION_SCHEDULER_RUNNER_H

#include <stop_token>

#include "application/ports/clock.h"
#include "application/ports/log.h"
#include "application/ports/task_executor.h"

#include "domain/update/schedule_queue.h"

/// SchedulerRunner — drives the periodic scheduling loop
/// (refactor/phase-3-scheduling-and-workflow.md §3.2).
///
/// Responsibilities: read the Clock, wait for the next deadline, respond to
/// the stop token, pop due tasks from the ScheduleQueue and submit them to
/// the TaskExecutor. Once stop is requested the runner never pops again;
/// rescheduling during shutdown is not possible because the queue advances
/// deadlines at pop time and the runner is the queue's only driver.
///
/// It knows nothing about DNS, IP sources, drivers or HTTP, and does not own
/// the I/O cancellation token — the composition root wires stop → I/O cancel
/// separately.
///
/// @note run() must be called from a single thread; the queue is not
///       thread-safe by design.
class SchedulerRunner {
public:
    /// All references must outlive the runner (they do: the composition root
    /// owns every component).
    SchedulerRunner(domain::ScheduleQueue &queue, Clock &clock, TaskExecutor &executor, std::stop_token stop,
                    const Logger &logger);

    /// Pop-and-submit due tasks until stop is requested, waiting on the
    /// clock between rounds. Returns promptly after stop; in-flight tasks
    /// are the TaskExecutor's business, not the runner's.
    void run();

private:
    domain::ScheduleQueue &queue_;
    Clock &clock_;
    TaskExecutor &executor_;
    std::stop_token stop_;
    const Logger &logger_;
};

#endif // YADDNSC_APPLICATION_SCHEDULER_RUNNER_H
