//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_CORE_MANAGER_H
#define YADDNSC_CORE_MANAGER_H

#include <memory>

#include "config/config.h"
#include "mixin.h"

class Manager {
public:
    explicit Manager(Config::AppConfig config);

    ~Manager();

    void load_drivers() const;

    void validate_config() const;

    // Install a signal-handling thread that catches SIGINT/SIGTERM via
    // sigwait() and requests graceful shutdown.  Must be called after
    // validate_config() and before run().  Signals must have been
    // blocked in the main thread (with pthread_sigmask) before the
    // Manager was constructed so that all threads inherit the mask.
    void install_signal_handler() const;

    // Run the scheduler loop.  Uses the stop_source set up by
    // install_signal_handler().  Blocks until a stop is requested.
    void run() const;

private:
    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;

    class Impl;

    std::unique_ptr<Impl> impl_;
};

#endif //YADDNSC_CORE_MANAGER_H
