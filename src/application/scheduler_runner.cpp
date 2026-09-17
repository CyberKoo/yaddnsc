//
// Created by Kotarou on 2026/9/17.
//

#include "scheduler_runner.h"

SchedulerRunner::SchedulerRunner(domain::ScheduleQueue &queue, Clock &clock, TaskExecutor &executor,
                                 std::stop_token stop, const Logger &logger)
    : queue_(queue), clock_(clock), executor_(executor), stop_(std::move(stop)), logger_(logger) {
    YLOG_INFO(logger_, "Scheduler initialised with {} tasks", queue_.size());
}

void SchedulerRunner::run() {
    while (!stop_.stop_requested()) {
        for (auto &task: queue_.pop_due(clock_.now())) {
            // A false return means the executor is shutting down; the task is
            // dropped, matching the legacy shutdown semantics (pending work is
            // discarded once stop was requested).
            executor_.submit(std::move(task));
        }

        const auto next = queue_.time_until_next(clock_.now());
        // An empty queue waits only for stop — same as the legacy scheduler's
        // empty-heap wait.
        const auto deadline = next ? clock_.now() + *next : domain::TimePoint::max();
        if (!clock_.wait_until(deadline, stop_)) {
            break;
        }
    }
}
