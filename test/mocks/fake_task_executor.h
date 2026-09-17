//
// FakeTaskExecutor — recording TaskExecutor test double.
//
// Submitted tasks are recorded (never executed); wait_submitted() lets a test
// block until the scheduler runner has dispatched the expected number of
// tasks. After shutdown() further submissions are rejected, mirroring the
// port contract.
// =============================================================================

#ifndef YADDNSC_TEST_MOCKS_FAKE_TASK_EXECUTOR_H
#define YADDNSC_TEST_MOCKS_FAKE_TASK_EXECUTOR_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

#include "application/ports/task_executor.h"

class FakeTaskExecutor final : public TaskExecutor {
public:
    bool submit(domain::UpdateTask task) override {
        {
            std::lock_guard lock(mtx_);
            if (shutdown_) {
                return false;
            }
            submitted_.push_back(std::move(task));
        }
        cv_.notify_all();
        return true;
    }

    void wait_idle() override { wait_idle_calls_.fetch_add(1); }

    void shutdown() override {
        std::lock_guard lock(mtx_);
        shutdown_ = true;
    }

    /// Block until at least `n` tasks were submitted (or the timeout fires).
    bool wait_submitted(std::size_t n, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) {
        std::unique_lock lock(mtx_);
        return cv_.wait_for(lock, timeout, [this, n] { return submitted_.size() >= n; });
    }

    [[nodiscard]] std::vector<domain::UpdateTask> submitted() const {
        std::lock_guard lock(mtx_);
        return submitted_;
    }

    [[nodiscard]] bool is_shutdown() const {
        std::lock_guard lock(mtx_);
        return shutdown_;
    }

    /// How often wait_idle() was called (the lifecycle drains exactly once).
    [[nodiscard]] int wait_idle_calls() const {
        return wait_idle_calls_.load();
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<domain::UpdateTask> submitted_;
    bool shutdown_{false};
    std::atomic<int> wait_idle_calls_{0};
};

#endif // YADDNSC_TEST_MOCKS_FAKE_TASK_EXECUTOR_H
