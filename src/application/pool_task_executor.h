//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_APPLICATION_POOL_TASK_EXECUTOR_H
#define YADDNSC_APPLICATION_POOL_TASK_EXECUTOR_H

#include <atomic>
#include <cstddef>

#include <BS_thread_pool.hpp>

#include "application/ports/task_executor.h"

class UpdateWorkflow;

/// PoolTaskExecutor — the TaskExecutor adapter that owns the application's
/// only BS::thread_pool (refactor/phase-3-scheduling-and-workflow.md §3.5).
///
/// Each submitted task runs the UpdateWorkflow on a pool thread. Work units
/// capture nothing but the task value and the workflow reference — no raw
/// driver pointer ever crosses into the pool. The destructor shuts down and
/// drains the pool, so tasks never touch the workflow (or, transitively, the
/// driver gateway) after the executor is gone.
class PoolTaskExecutor final : public TaskExecutor {
public:
    /// @param thread_count  Pool size (the composition root keeps the legacy
    ///                      estimate: total subdomains, capped at
    ///                      min(hardware cores, 4)).
    /// @param workflow      Use case executed per task (non-owning; owned by
    ///                      the composition root and drained-before-destroyed
    ///                      by this executor).
    PoolTaskExecutor(std::size_t thread_count, const UpdateWorkflow &workflow);

    /// Shut down (reject new tasks) and drain the pool before the base
    /// subobjects are destroyed.
    ~PoolTaskExecutor() override;

    /// Run one task on the pool. @return false when shut down (task dropped).
    bool submit(domain::UpdateTask task) override;

    /// Block until every accepted task has finished.
    void wait_idle() override;

    /// Stop accepting new tasks; in-flight tasks keep running.
    void shutdown() override;

private:
    const UpdateWorkflow &workflow_;
    std::atomic<bool> accepting_{true};
    // Declared last so destruction drains the pool before the workflow
    // reference could dangle (the destructor body drains explicitly anyway).
    BS::thread_pool<> pool_;
};

#endif // YADDNSC_APPLICATION_POOL_TASK_EXECUTOR_H
