//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_CORE_SCHEDULER_H
#define YADDNSC_CORE_SCHEDULER_H

#include <memory>
#include <stop_token>

#include "mixin.h"

namespace Config {
    struct AppConfig;
}

class Updater;

// ---------------------------------------------------------------------------
// Scheduler — drives the periodic DDNS update loop.
//
// Maintains a min-heap of SubdomainEntry nodes sorted by deadline.  The
// run() method pops entries whose deadline has passed, submits them to a
// shared thread pool via the Updater, re-queues them with their next
// deadline, and sleeps until the nearest future deadline.
//
// The stop_source is injected at construction time (typically from
// SignalHandler).  The event loop exits when a stop is requested.
// ---------------------------------------------------------------------------
class Scheduler {
public:
    explicit Scheduler(const Config::AppConfig &config, Updater &updater, std::stop_source stop_source);

    ~Scheduler();

    // Run the scheduler loop.  Blocks until a stop is requested.
    void run();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

#endif // YADDNSC_CORE_SCHEDULER_H
