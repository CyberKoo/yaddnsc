//
// Created by Kotarou on 2026/6/29.
//

#include "scheduler.h"

#include <queue>
#include <thread>
#include <mutex>
#include <chrono>
#include <stop_token>
#include <condition_variable>

#include <spdlog/spdlog.h>
#include <BS_thread_pool.hpp>

#include "fmt.hpp"
#include "updater.h"
#include "update_task.h"
#include "config/config.h"

// ---------------------------------------------------------------------------
// Scheduler::Impl — owns all scheduling state and the thread pool.
// ---------------------------------------------------------------------------

struct Scheduler::Impl {
    explicit Impl(const Config::AppConfig &config, Updater &updater);

    ~Impl();

    void set_stop_source(std::stop_source ss);

    void run();

    void build_initial_schedule();

    bool check_force_update(const SubdomainEntry &entry, std::chrono::steady_clock::time_point now);

    unsigned int estimated_pool_size() const;

    // ---- scheduling state --------------------------------------------------
    std::priority_queue<SubdomainEntry, std::vector<SubdomainEntry>, std::greater<> > schedule_;
    std::vector<DomainState> domain_states_;

    // ---- synchronisation for scheduler sleep -------------------------------
    std::mutex cv_mtx_;
    std::condition_variable cv_;

    // ---- thread pool -------------------------------------------------------
    BS::thread_pool<> pool_;

    // ---- stop source (injected by SignalHandler) ---------------------------
    std::stop_source stop_source_;

    // ---- injected dependencies ---------------------------------------------
    Updater &updater_;
    const Config::AppConfig &config_;
};

Scheduler::Impl::Impl(const Config::AppConfig &config, Updater &updater) : updater_(updater), config_(config) {
}

Scheduler::Impl::~Impl() {
    schedule_ = {};
    if (pool_.get_tasks_queued() > 0) {
        SPDLOG_INFO("Waiting for {} pending task(s) to complete...", pool_.get_tasks_queued());
        pool_.wait();
    }
}

void Scheduler::Impl::set_stop_source(std::stop_source ss) {
    stop_source_ = std::move(ss);
}

void Scheduler::Impl::run() {
    // Resize the thread pool based on the total number of subdomains.
    pool_.reset(estimated_pool_size());

    // Build the initial schedule (one SubdomainEntry per subdomain).
    build_initial_schedule();

    auto st = stop_source_.get_token();

    // Register a stop callback that wakes up the scheduler immediately.
    std::stop_callback cb(st, [this] { cv_.notify_all(); });

    std::unique_lock lock(cv_mtx_);

    while (!st.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();

        // Pop all entries whose deadline has passed.
        while (!schedule_.empty() && now >= schedule_.top().deadline) {
            auto entry = std::move(const_cast<SubdomainEntry &>(schedule_.top()));
            schedule_.pop();

            // Copy the task for the pool — the entry's copy must remain
            // valid for re-queuing into the schedule heap.
            auto update_task = entry.task;
            update_task.force_update = check_force_update(entry, now);

            // Submit a single subdomain update task to the shared pool.
            pool_.detach_task(
                [&updater = updater_, task = std::move(update_task)] {
                    try {
                        updater.process(task);
                    } catch (const std::exception &e) {
                        SPDLOG_ERROR("Unhandled exception during update for {}: {}", task.fqdn, e.what());
                    }
                }
            );

            // Re-schedule this entry for the next interval.
            entry.deadline = now + std::chrono::seconds(entry.update_interval);
            schedule_.push(std::move(entry));
        }

        // Sleep until the nearest deadline (or indefinitely if the heap is empty).
        if (schedule_.empty()) {
            cv_.wait(lock, [&st] { return st.stop_requested(); });
        } else {
            cv_.wait_until(lock, schedule_.top().deadline, [&st] { return st.stop_requested(); });
        }
    }
}

void Scheduler::Impl::build_initial_schedule() {
    domain_states_.reserve(config_.domains.size());

    for (size_t di = 0; di < config_.domains.size(); ++di) {
        const auto &[name, update_interval, force_update, driver, subdomains] = config_.domains[di];

        domain_states_.emplace_back();

        for (const auto &subdomain: subdomains) {
            const auto fqdn = fmt::format("{}.{}", subdomain.name, name);

            // Use per-subdomain update_interval if set (> 0), otherwise
            // fall back to the domain-level value.
            const auto effective_interval = subdomain.update_interval > 0 ? subdomain.update_interval : update_interval;

            schedule_.push(SubdomainEntry{
                .deadline = std::chrono::steady_clock::now(),
                .update_interval = effective_interval,
                .force_update_interval = force_update,
                .domain_idx = di,
                .task =
                {
                    .subdomain = subdomain,
                    .domain_name = name,
                    .driver_name = driver,
                    .fqdn = fqdn,
                    .force_update = false,
                },
            });
        }
    }

    SPDLOG_INFO("Scheduler initialised with {} tasks", schedule_.size());
}

bool Scheduler::Impl::check_force_update(const SubdomainEntry &entry, std::chrono::steady_clock::time_point now) {
    if (entry.force_update_interval <= 0) {
        return false;
    }

    auto &state = domain_states_[entry.domain_idx];
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - state.last_force_update).count();

    if (elapsed >= entry.force_update_interval) {
        state.last_force_update = now;
        return true;
    }

    return false;
}

unsigned int Scheduler::Impl::estimated_pool_size() const {
    unsigned int total_subdomains = 0;
    const auto thread_count = std::thread::hardware_concurrency();

    for (const auto &domain: config_.domains) {
        total_subdomains += static_cast<unsigned int>(domain.subdomains.size());
    }

    if (total_subdomains < 2 || thread_count < 2) {
        return 2;
    }

    if (total_subdomains < thread_count) {
        return total_subdomains;
    }

    return thread_count;
}

// ---------------------------------------------------------------------------
// Scheduler public API — thin delegation to Impl
// ---------------------------------------------------------------------------

Scheduler::Scheduler(const Config::AppConfig &config, Updater &updater)
    : impl_(std::make_unique<Impl>(config, updater)) {
}

Scheduler::~Scheduler() = default;

void Scheduler::set_stop_source(std::stop_source ss) {
    impl_->set_stop_source(std::move(ss));
}

void Scheduler::run() {
    impl_->run();
}
