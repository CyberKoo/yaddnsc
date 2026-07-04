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

// ---------------------------------------------------------------------------
// Scheduler — pure timer queue for periodic DDNS update tasks.
//
// Maintains a min-heap of internal nodes sorted by deadline.
//   - build_initial_schedule()  populates the heap from the config.
//   - pop_all_due()             returns tasks whose deadline has passed,
//                               and automatically re-queues each one with
//                               its next deadline.
//   - wait_for_next()           blocks until the nearest deadline or stop.
//
// Holds no reference to the Updater or any thread pool — the caller
// (Manager) is responsible for executing the returned tasks.
// ---------------------------------------------------------------------------
class Scheduler {
public:
    explicit Scheduler(const Config::AppConfig &config, std::stop_token stop_token);

    ~Scheduler();

    // Return all tasks whose deadline has passed.  Each returned task is
    // automatically re-queued with its next deadline internally.
    // force_update is evaluated before returning.
    [[nodiscard]] std::vector<UpdateTask> pop_all_due();

    // Block until the nearest task deadline is reached or stop is
    // requested.  Returns false if stop was requested.
    bool wait_for_next();

    // True if there are any pending tasks in the heap.
    [[nodiscard]] bool has_pending() const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;

    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

#endif // YADDNSC_CORE_SCHEDULER_H
