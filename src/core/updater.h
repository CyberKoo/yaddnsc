//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_CORE_UPDATER_H
#define YADDNSC_CORE_UPDATER_H

#include <memory>

#include "type.h"
#include "update_task.h"
#include "mixin.h"

class DriverManager;
class NetworkManager;
class MultiResolver;

// ---------------------------------------------------------------------------
// Updater — stateful executor that processes a single UpdateTask.
//
// Holds non-owning references to the DriverManager, NetworkManager, and
// MultiResolver (all initialized before any call to process()).
//
// process() is thread-safe: it is marked const, owns no mutable state, and
// may be called concurrently from multiple pool threads.
// ---------------------------------------------------------------------------
class Updater {
public:
    Updater(const DriverManager &driver_manager, const NetworkManager &network_manager,
            const MultiResolver &resolver_pool);

    ~Updater();

    // Process one update task.  Never throws — all errors and outcomes are
    // logged internally via SPDLOG.
    void process(const UpdateTask &task) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

#endif // YADDNSC_CORE_UPDATER_H
