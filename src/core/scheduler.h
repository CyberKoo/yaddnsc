//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_CORE_SCHEDULER_H
#define YADDNSC_CORE_SCHEDULER_H

#include <memory>
#include <vector>
#include <stop_token>

#include "mixin.h"

namespace Config {
    struct AppConfig;
}

struct UpdateTask;

/// Scheduler — pure timer queue for periodic DDNS update tasks.
///
/// Maintains a min-heap of internal nodes sorted by deadline.
///   - Constructor populates the heap from the config.
///   - pop_all_due()  returns tasks whose deadline has passed,
///                    and automatically re-queues each one with
///                    its next deadline.
///   - wait_for_next() blocks until the nearest deadline or stop.
///
/// Holds no reference to the Updater or any thread pool — the caller
/// (Manager) is responsible for executing the returned tasks.
///
/// @note Thread-safe: all public methods acquire an internal mutex.
///       wait_for_next() and pop_all_due() must not be called concurrently
///       (the caller's loop owns the scheduling sequence).
class Scheduler {
public:
    /// Construct and populate the schedule from config.
    /// @param config      Application config with domain/subdomain definitions.
    /// @param stop_token  Token used to wake the scheduler on shutdown.
    explicit Scheduler(const Config::AppConfig &config, std::stop_token stop_token);

    ~Scheduler();

    /// Return all tasks whose deadline has passed.
    ///
    /// Each returned task is automatically re-queued with its next deadline
    /// internally. The force_update flag is evaluated before returning.
    /// @return  A vector of tasks ready to execute.
    [[nodiscard]] std::vector<UpdateTask> pop_all_due();

    /// Block until the nearest task deadline is reached or stop is requested.
    /// @return false if stop was requested (caller should exit the loop).
    [[nodiscard]] bool wait_for_next();

    /// Check if there are any pending tasks in the heap.
    /// @return true if at least one task is scheduled.
    [[nodiscard]] bool has_pending() const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;

    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

#endif // YADDNSC_CORE_SCHEDULER_H
