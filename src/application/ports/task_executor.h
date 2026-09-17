//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_APPLICATION_PORTS_TASK_EXECUTOR_H
#define YADDNSC_APPLICATION_PORTS_TASK_EXECUTOR_H

#include "domain/update/update_task.h"

/// TaskExecutor — execution port for scheduled update tasks
/// (refactor/phase-3-scheduling-and-workflow.md §3.5).
///
/// The executor is the only component allowed to own a thread pool; it knows
/// nothing about plugin modules. Submitted work units reference drivers only
/// through the DriverGateway port (never a raw Driver*), so no dangling
/// driver pointer can outlive the module — the shutdown sequence drains the
/// executor before driver instances are destroyed.
class TaskExecutor {
public:
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
};

#endif // YADDNSC_APPLICATION_PORTS_TASK_EXECUTOR_H
