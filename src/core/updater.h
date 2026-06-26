//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_CORE_UPDATER_H
#define YADDNSC_CORE_UPDATER_H

#include <memory>
#include <vector>

#include "type.h"
#include "update_task.h"
#include "base_classes.h"

class DriverManager;
class NetworkManager;

// ---------------------------------------------------------------------------
// Updater — stateful executor that processes a single UpdateTask.
//
// The Updater holds non-owning references to the DriverManager and
// NetworkManager (both initialised before any call to process()), and a
// list of pre-resolved DnsServer entries.
//
// process() is thread-safe: it is marked const, owns no mutable state, and
// may be called concurrently from multiple pool threads.
// ---------------------------------------------------------------------------
class Updater : public RestrictedClass {
public:
    Updater(DriverManager &driver_manager, NetworkManager &network_manager, std::vector<DnsServer> dns_servers);

    ~Updater();

    // Process one update task.  Never throws — all errors and outcomes are
    // logged internally via SPDLOG.
    void process(const UpdateTask &task) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // YADDNSC_CORE_UPDATER_H
