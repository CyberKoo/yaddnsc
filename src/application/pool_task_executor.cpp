//
// Created by Kotarou on 2026/9/17.
//

#include "pool_task_executor.h"

#include <utility>

#include "update_workflow.h"

PoolTaskExecutor::PoolTaskExecutor(std::size_t thread_count, const UpdateWorkflow &workflow)
    : workflow_(workflow), pool_(thread_count) {
}

PoolTaskExecutor::~PoolTaskExecutor() {
    shutdown();
    wait_idle();
}

bool PoolTaskExecutor::submit(domain::UpdateTask task) {
    if (!accepting_.load(std::memory_order_acquire)) {
        return false;
    }

    pool_.detach_task([this, t = std::move(task)] { static_cast<void>(workflow_.run(t)); });
    return true;
}

void PoolTaskExecutor::wait_idle() {
    pool_.wait();
}

void PoolTaskExecutor::shutdown() {
    accepting_.store(false, std::memory_order_release);
}
