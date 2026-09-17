//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_APPLICATION_PORTS_TASK_EXECUTOR_H
#define YADDNSC_APPLICATION_PORTS_TASK_EXECUTOR_H

#include <chrono>
#include <functional>

#include "domain/update/schedule_queue.h"
#include "domain/update/update_task.h"

/// TaskExecutor — execution port for scheduled update tasks.
///
/// The executor is the only component allowed to own a thread pool; it knows
/// nothing about plugin modules. Submitted work units reference drivers only
/// through the DriverGateway port (never a raw Driver*), so no dangling
/// driver pointer can outlive the module — the shutdown sequence drains the
/// executor before driver instances are destroyed.
class TaskExecutor {
public:
    /// Invoked (on an executor thread) when a task fails with a provider
    /// retry_after delay — the scheduler uses it to honour rate-limit
    /// backoff for the task's next deadline.
    using RetryHandler = std::function<void(domain::TaskId, std::chrono::seconds)>;

    virtual ~TaskExecutor() = default;

    /// Submit one scheduled task for execution.
    /// @return false when the executor is shutting down; the task is dropped
    ///         (shutdown discards pending work by design).
    virtual bool submit(domain::UpdateTask task) = 0;

    /// Block until every accepted task has finished.
    virtual void wait_idle() = 0;

    /// Stop accepting new tasks; submit() returns false afterwards.
    /// In-flight tasks are unaffected — drain them with wait_idle().
    virtual void shutdown() = 0;

    /// Install the retry_after handler. Called once during composition,
    /// before the scheduling loop starts submitting work.
    virtual void set_retry_handler(RetryHandler handler) = 0;
};

#endif // YADDNSC_APPLICATION_PORTS_TASK_EXECUTOR_H
