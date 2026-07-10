//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_CORE_UPDATER_H
#define YADDNSC_CORE_UPDATER_H

#include <functional>
#include <memory>

#include "mixin.h"

class Driver;
class HttpClient;
class IpSourceBase;
struct UpdateTask;
class ResolverDispatcher;

namespace Config {
    struct SubdomainConfig;
}

/// Updater — executor that processes a single UpdateTask.
///
/// Holds a non-owning reference to the ResolverDispatcher (initialised before
/// any call to process()). The Driver is pre-resolved by the caller and passed
/// directly into process(), eliminating the need for runtime string lookups.
///
/// @note process() is thread-safe: it is marked const, owns no mutable state,
///       and may be called concurrently from multiple pool threads.
class Updater {
public:
    /// Factory type for creating IP source instances.
    using IpSourceFactory = std::function<std::unique_ptr<IpSourceBase>(const Config::SubdomainConfig &)>;

    /// Construct with a reference to the resolver dispatcher.
    /// @param resolver_pool  Resolver used to look up current DNS records.
    explicit Updater(const ResolverDispatcher &resolver_pool);

    /// Construct with injected IP source factory (for testing).
    /// @param resolver_pool  Resolver used to look up current DNS records.
    /// @param ip_factory     Factory that creates IpSourceBase instances on demand.
    Updater(const ResolverDispatcher &resolver_pool, IpSourceFactory ip_factory);

    ~Updater();

    /// Execute a single update task using the given driver and HTTP client.
    ///
    /// Steps:
    ///   1. Optionally resolve the current DNS record for comparison.
    ///   2. If the IP has changed (or force_update is set), invoke the driver.
    ///
    /// Exception handling architecture:
    ///   ┌─────────────────────────────────────────────────────────────┐
    ///   │ Updater::process() noexcept  ←  catch-all (log + swallow)  │
    ///   │   └── Impl::process()         ←  no try-catch              │
    ///   │         └── resolve_local_address()  ←  no try-catch       │
    ///   │               └── ip_source->resolve()  ←  throws on err   │
    ///   └─────────────────────────────────────────────────────────────┘
    ///
    /// IpSourceBase implementations throw std::runtime_error on failure.
    /// The exception aborts the current resolution operation and propagates
    /// uncaught through the intermediate layers (Impl::process and
    /// resolve_local_address have no try-catch).  It is caught only at this
    /// noexcept boundary, where it is logged via SPDLOG_ERROR and swallowed.
    /// There is no retry, fallback, or error-type branching in any catch
    /// block — the catch is a pure observation point per the project's error
    /// handling guideline.
    ///
    /// @param task         The update task describing what to update.
    /// @param driver       The driver plugin to use.
    /// @param http_client  HTTP client for the upstream API call.
    ///
    /// @note Never throws — all errors and outcomes are logged internally.
    void process(const UpdateTask &task, const Driver &driver, HttpClient &http_client) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

#endif // YADDNSC_CORE_UPDATER_H
