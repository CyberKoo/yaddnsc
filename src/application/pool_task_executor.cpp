//
// Created by Kotarou on 2026/9/17.
//

#include "pool_task_executor.h"

#include <chrono>
#include <functional>
#include <type_traits>
#include <utility>

#include "domain/error/error.h"
#include "domain/update/schedule_queue.h"
#include "domain/update/update_task.h"

#include "BS_thread_pool.hpp"
#include "update_workflow.h"

PoolTaskExecutor::PoolTaskExecutor(std::size_t thread_count, const UpdateWorkflow& workflow)
    : workflow_(workflow), pool_(thread_count) {}

PoolTaskExecutor::~PoolTaskExecutor() {
    shutdown();
    wait_idle();
}

bool PoolTaskExecutor::submit(domain::UpdateTask task) {
    if (!accepting_.load(std::memory_order_acquire)) {
        return false;
    }

    pool_.detach_task([this, t = std::move(task)] {
        const auto result = workflow_.run(t);
        // A provider retry_after (rate limit) is handed back to the
        // scheduler so the task's next deadline honours the backoff.
        if (!result && result.error().retry_after_seconds > 0 && retry_handler_) {
            retry_handler_(domain::TaskId{t.domain_index, t.subdomain_index},
                           std::chrono::seconds(result.error().retry_after_seconds));
        }
    });
    return true;
}

void PoolTaskExecutor::wait_idle() {
    pool_.wait();
}

void PoolTaskExecutor::shutdown() {
    accepting_.store(false, std::memory_order_release);
}
