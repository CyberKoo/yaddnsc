//
// Created by Kotarou on 2026/6/29.
//

#include "scheduler.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <stop_token>
#include <utility>

#include "config/config.h"

#include "update_task.h"

#include "fmt.hpp"
#include <spdlog/spdlog.h>

// ---------------------------------------------------------------------------
// ScheduleEntry — a single node in the scheduling min-heap.
// Kept private inside the .cpp; never exposed to callers.
// ---------------------------------------------------------------------------
struct ScheduleEntry {
    std::chrono::steady_clock::time_point deadline;
    int update_interval{};
    int force_update_interval{};
    std::chrono::steady_clock::time_point last_force_update;
    UpdateTask task;

    // std::priority_queue is a max-heap by default, so we invert the comparison.
    bool operator>(const ScheduleEntry &other) const {
        return deadline > other.deadline;
    }
};

// ---------------------------------------------------------------------------
// Scheduler::Impl — pure timer queue state.
// ---------------------------------------------------------------------------

struct Scheduler::Impl {
    Impl(Config::AppConfig config, std::stop_token stop_token);

    [[nodiscard]] static bool check_force_update(ScheduleEntry &entry, std::chrono::steady_clock::time_point now);

    std::vector<UpdateTask> pop_all_due();

    bool wait_for_next();

    [[nodiscard]] bool has_pending() const;

    // ---- config ------------------------------------------------------------
    Config::AppConfig config_;

    // ---- stop --------------------------------------------------------------
    std::stop_token stop_token_;

    // ---- scheduling state --------------------------------------------------
    std::priority_queue<ScheduleEntry, std::vector<ScheduleEntry>, std::greater<> > heap_;

    // ---- synchronisation ---------------------------------------------------
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::stop_callback<std::function<void()> > stop_cb_; // notifies cv_ when stop fires
};

Scheduler::Impl::Impl(Config::AppConfig config, std::stop_token stop_token)
    : config_(std::move(config)), stop_token_(std::move(stop_token)),
      stop_cb_(stop_token_, [this] { cv_.notify_all(); }) {
    for (const auto &[name, update_interval, force_update, driver, subdomains]: config_.domains) {
        for (const auto &subdomain: subdomains) {
            const auto fqdn = fmt::format("{}.{}", subdomain.name, name);
            const auto effective_interval = subdomain.update_interval > 0 ? subdomain.update_interval : update_interval;

            heap_.push(ScheduleEntry{
                .deadline = std::chrono::steady_clock::now(),
                .update_interval = effective_interval,
                .force_update_interval = force_update,
                .last_force_update = {},
                .task = {
                    .config = subdomain,
                    .domain_name = name,
                    .driver_name = driver,
                    .fqdn = fqdn,
                    .force_update = false,
                },
            });
        }
    }

    SPDLOG_INFO("Scheduler initialised with {} tasks", heap_.size());
}

bool Scheduler::Impl::check_force_update(ScheduleEntry &entry, std::chrono::steady_clock::time_point now) {
    if (entry.force_update_interval <= 0) {
        return false;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - entry.last_force_update).count();

    if (elapsed >= entry.force_update_interval) {
        entry.last_force_update = now;
        return true;
    }

    return false;
}

std::vector<UpdateTask> Scheduler::Impl::pop_all_due() {
    std::lock_guard lock(mtx_);
    const auto now = std::chrono::steady_clock::now();

    std::vector<UpdateTask> due;
    while (!heap_.empty() && now >= heap_.top().deadline) {
        auto entry = heap_.top();
        heap_.pop();

        entry.task.force_update = check_force_update(entry, now);
        due.push_back(entry.task); // copy — entry stays intact for re-queue

        // Re-queue the entry with its next deadline for the next cycle.
        entry.deadline = now + std::chrono::seconds(entry.update_interval);
        entry.task.force_update = false;
        heap_.push(std::move(entry));
        cv_.notify_one();
    }

    return due;
}

bool Scheduler::Impl::wait_for_next() {
    std::unique_lock lock(mtx_);

    if (heap_.empty()) {
        cv_.wait(lock, [this] { return stop_token_.stop_requested(); });
    } else {
        cv_.wait_until(lock, heap_.top().deadline, [this] { return stop_token_.stop_requested(); });
    }

    return !stop_token_.stop_requested();
}

bool Scheduler::Impl::has_pending() const {
    std::lock_guard lock(mtx_);
    return !heap_.empty();
}

// ---------------------------------------------------------------------------
// Scheduler public API — thin delegation to Impl
// ---------------------------------------------------------------------------

Scheduler::Scheduler(const Config::AppConfig &config, std::stop_token stop_token)
    : impl_(std::make_unique<Impl>(config, std::move(stop_token))) {
}

Scheduler::~Scheduler() = default;

std::vector<UpdateTask> Scheduler::pop_all_due() {
    return impl_->pop_all_due();
}

bool Scheduler::wait_for_next() {
    return impl_->wait_for_next();
}

bool Scheduler::has_pending() const {
    return impl_->has_pending();
}
