//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_APPLICATION_SCHEDULER_RUNNER_H
#define YADDNSC_APPLICATION_SCHEDULER_RUNNER_H

#include <chrono>
#include <mutex>
#include <stop_token>
#include <utility>
#include <vector>

#include "domain/update/schedule_queue.h"

class Clock;
class Logger;
class TaskExecutor;

namespace Utils {
class CancellationToken;
}

/// SchedulerRunner — drives the periodic scheduling loop.
///
/// Responsibilities: read the Clock, wait for the next deadline, respond to
/// the stop token, pop due tasks from the ScheduleQueue and submit them to
/// the TaskExecutor. Once stop is requested the runner never pops again;
/// rescheduling during shutdown is not possible because the queue advances
/// deadlines at pop time and the runner is the queue's only driver.
///
/// It knows nothing about DNS, IP sources, drivers or HTTP; the I/O
/// cancellation token passes through run() as a parameter (owned by the
/// composition root) on its way to the executor.
///
/// @note run() must be called from a single thread; the queue is not
///       thread-safe by design.
class SchedulerRunner {
public:
    /// All references must outlive the runner (they do: the composition root
    /// owns every component).
    SchedulerRunner(domain::ScheduleQueue& queue,
                    Clock& clock,
                    TaskExecutor& executor,
                    std::stop_token stop,
                    const Logger& logger);

    /// Pop-and-submit due tasks until stop is requested, waiting on the
    /// clock between rounds. Returns promptly after stop; in-flight tasks
    /// are the TaskExecutor's business, not the runner's.
    /// @param token  I/O cancellation token forwarded to every submitted task.
    void run(const Utils::CancellationToken& token);

    /// Thread-safe: called from executor pool threads when a task's update
    /// failed with a provider retry_after delay. Moves the task's next
    /// deadline to now + delay and wakes the scheduling loop so the new
    /// deadline is honoured before the next pop.
    void request_retry(domain::TaskId id, std::chrono::seconds delay);

private:
    domain::ScheduleQueue& queue_;
    Clock& clock_;
    TaskExecutor& executor_;
    std::stop_token stop_;
    const Logger& logger_;

    // Retry requests arrive on pool threads; the runner drains them on its
    // own thread at the top of every scheduling round.
    std::mutex retry_mtx_;
    std::vector<std::pair<domain::TaskId, std::chrono::seconds>> pending_retries_;
};

#endif  // YADDNSC_APPLICATION_SCHEDULER_RUNNER_H
