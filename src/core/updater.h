//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_CORE_UPDATER_H
#define YADDNSC_CORE_UPDATER_H

#include <memory>
#include <vector>

#include "type.h"
#include "update_task.h"
#include "mixin.h"

class DriverManager;
class NetworkManager;
class ResolverBase;

// ---------------------------------------------------------------------------
// Updater — stateful executor that processes a single UpdateTask.
//
// The Updater holds non-owning references to the DriverManager and
// NetworkManager (both initialized before any call to process()), and pre-
// built ResolverBase instances used for DNS resolution.
//
// process() is thread-safe: it is marked const, owns no mutable state, and
// may be called concurrently from multiple pool threads.
// ---------------------------------------------------------------------------
class Updater {
public:
    Updater(DriverManager &driver_manager, NetworkManager &network_manager,
            const std::vector<std::shared_ptr<ResolverBase> > &resolvers);

    ~Updater();

    // Process one update task.  Never throws — all errors and outcomes are
    // logged internally via SPDLOG.
    void process(const UpdateTask &task) const;

private:
    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // YADDNSC_CORE_UPDATER_H
