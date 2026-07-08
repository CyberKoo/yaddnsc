//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_CORE_MANAGER_H
#define YADDNSC_CORE_MANAGER_H

#include <functional>
#include <memory>
#include <stop_token>

#include "config/config.h"
#include "mixin.h"

class HttpClient;
class ResolverDispatcher;

/// Top-level orchestrator for the DDNS client lifecycle.
///
/// Owns the scheduler, thread pool, resolver dispatcher, and driver manager.
/// Callers should invoke methods in order:
///   1. load_drivers()
///   2. validate_config()
///   3. run() — blocks until a stop is requested
class Manager {
public:
    /// Construct the manager with the loaded config and a stop source.
    /// @param config        Parsed application configuration.
    /// @param stop_source   Shared stop source (typically from SignalWatcher).
    explicit Manager(Config::AppConfig config, std::stop_source stop_source);

    /// Construct with injected dependencies (for testing).
    /// @param config        Parsed application configuration.
    /// @param stop_source   Shared stop source.
    /// @param dispatcher    Pre-configured resolver dispatcher (mock or real).
    /// @param http_factory  Factory that creates HttpClient instances on demand.
    Manager(Config::AppConfig config, std::stop_source stop_source,
            ResolverDispatcher dispatcher, std::function<std::unique_ptr<HttpClient>()> http_factory);

    ~Manager();

    /// Load all driver shared libraries specified in the configuration.
    void load_drivers() const;

    /// Run pre-flight validation on the loaded configuration.
    /// @throws ConfigVerificationException  On the first violated constraint.
    void validate_config() const;

    /// Run the scheduler loop.  Blocks until a stop is requested.
    void run() const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;

    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

#endif //YADDNSC_CORE_MANAGER_H
