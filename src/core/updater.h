//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_CORE_UPDATER_H
#define YADDNSC_CORE_UPDATER_H

#include <memory>

#include "mixin.h"

class Driver;
class HttpClient;
struct UpdateTask;
class ResolverDispatcher;

// ---------------------------------------------------------------------------
// Updater — executor that processes a single UpdateTask.
//
// Holds a non-owning reference to the ResolverDispatcher (initialized before
// any call to process()). The Driver is pre-resolved by the caller and passed
// directly into process(), eliminating the need for runtime string lookups.
//
// process() is thread-safe: it is marked const, owns no mutable state, and
// may be called concurrently from multiple pool threads.
// ---------------------------------------------------------------------------
class Updater {
public:
    explicit Updater(const ResolverDispatcher &resolver_pool);

    ~Updater();

    // Process with an externally-provided HttpClient (e.g. a mock in tests).
    // Never throws — all errors and outcomes are logged internally via SPDLOG.
    void process(const UpdateTask &task, const Driver &driver, HttpClient &http_client) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

#endif // YADDNSC_CORE_UPDATER_H
