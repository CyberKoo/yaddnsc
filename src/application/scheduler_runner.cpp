//
// Created by Kotarou on 2026/9/17.
//

#include "scheduler_runner.h"

#include <optional>

#include "application/ports/clock.h"
#include "application/ports/log.h"
#include "application/ports/task_executor.h"
#include "domain/update/time_types.h"
#include "domain/update/update_task.h"

SchedulerRunner::SchedulerRunner(domain::ScheduleQueue& queue, SchedulerEnvironment env, std::stop_token stop)
    : queue_(queue), env_(env), stop_(std::move(stop)) {
    YLOG_INFO(env_.logger, "Scheduler initialised with {} tasks", queue_.size());
}

void SchedulerRunner::run(const Utils::CancellationToken& token) {
    while (!stop_.stop_requested()) {
        // Apply retry_after reschedules reported since the last round BEFORE
        // popping, so a rate-limited task is not re-executed at its old
        // (sooner) deadline.
        {
            std::vector<std::pair<domain::TaskId, std::chrono::seconds>> retries;
            {
                std::lock_guard lock(retry_mtx_);
                retries.swap(pending_retries_);
            }
            if (!retries.empty()) {
                const auto now = env_.clock.now();
                for (const auto& [id, delay] : retries) {
                    queue_.reschedule(id, now + delay);
                }
            }
        }

        for (auto& task : queue_.pop_due(env_.clock.now())) {
            // A false return means the executor is shutting down; the task is
            // dropped, matching the legacy shutdown semantics (pending work is
            // discarded once stop was requested).
            env_.executor.submit(std::move(task), token);
        }

        // Read the clock once: with two reads a concurrent time jump (a fake
        // clock advanced from another thread) could land between them and push
        // the deadline past the very next due entry, parking the loop forever.
        const auto now = env_.clock.now();
        const auto next = queue_.time_until_next(now);
        // An empty queue waits only for stop — same as the legacy scheduler's
        // empty-heap wait.
        const auto deadline = next ? now + *next : domain::TimePoint::max();
        if (!env_.clock.wait_until(deadline, stop_)) {
            break;
        }
    }
}

void SchedulerRunner::request_retry(domain::TaskId id, std::chrono::seconds delay) {
    {
        std::lock_guard lock(retry_mtx_);
        pending_retries_.emplace_back(id, delay);
    }
    env_.clock.wake();
}
