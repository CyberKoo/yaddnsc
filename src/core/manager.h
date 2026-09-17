//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_CORE_MANAGER_H
#define YADDNSC_CORE_MANAGER_H

#include <memory>
#include <stop_token>

#include "domain/config/runtime_config.h"
#include "infrastructure/plugin/abi_driver_gateway.h"

class ResolverDispatcher;

/// Top-level orchestrator for the DDNS client lifecycle.
///
/// Owns the scheduler, thread pool, resolver dispatcher, and driver catalog.
/// Callers should invoke methods in order:
///   1. load_drivers()
///   2. validate_config()
///   3. run() — blocks until a stop is requested
class Manager {
public:
    /// Construct the manager with the runtime config and a stop source.
    /// @param config        Normalised runtime configuration.
    /// @param stop_source   Shared stop source (typically from SignalWatcher).
    explicit Manager(domain::RuntimeConfig config, std::stop_source stop_source);

    /// Construct with injected dependencies (for testing).
    /// @param config        Normalised runtime configuration.
    /// @param stop_source   Shared stop source.
    /// @param dispatcher    Pre-configured resolver dispatcher (mock or real).
    /// @param http_factory  Factory that creates HttpClient instances on demand.
    Manager(domain::RuntimeConfig config, std::stop_source stop_source,
                ResolverDispatcher dispatcher, HttpClientFactory http_factory);

    ~Manager();

    /// Load all driver shared libraries specified in the configuration.
    void load_drivers();

    /// Run pre-flight validation on the loaded configuration.
    /// @throws ConfigVerificationException  On the first violated constraint.
    void validate_config() const;

    /// Run the scheduler loop.  Blocks until a stop is requested.
    void run();

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

#endif //YADDNSC_CORE_MANAGER_H
