//
// Created by Kotarou on 2026/6/28.
//

#ifndef YADDNSC_DNS_DISPATCHER_H
#define YADDNSC_DNS_DISPATCHER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "config/dns_config.h"

#include "record_kind.h"

class ResolverBase;

/// ResolverDispatcher — dispatches DNS queries across one or more backend
///                      resolvers using a configurable strategy (fallback /
///                      concurrent), with automatic retry on transient errors.
///
/// Eliminates the need to pass resolver vectors through every layer.
/// @note Thread-safe: resolve() is const and does not mutate shared state.
class ResolverDispatcher {
public:
    /// Construct with a list of resolver backends and a dispatch strategy.
    /// @param resolvers  Vector of resolver backends to query.
    /// @param strategy   Dispatch strategy (fallback or concurrent).
    explicit ResolverDispatcher(std::vector<std::shared_ptr<ResolverBase> > resolvers,
                                Config::ResolverStrategy strategy = Config::ResolverStrategy::CONCURRENT);

    ~ResolverDispatcher();

    ResolverDispatcher(ResolverDispatcher &&) noexcept;

    ResolverDispatcher &operator=(ResolverDispatcher &&) noexcept;

    /// Resolve a hostname using the configured strategy and backends.
    ///
    /// On transient errors (timeout, NXDOMAIN retryable), automatically retries
    /// up to `max_retries` times with exponential-like backoff.
    ///
    /// @param host         Hostname to resolve.
    /// @param type         DNS record type (A or AAAA).
    /// @param max_retries  Maximum number of retries on transient errors.
    /// @param backoff_ms   Base backoff interval in milliseconds.
    /// @return             List of resolved IP address strings.
    [[nodiscard]] std::vector<std::string> resolve(const std::string &host, RecordKind type,
                                                   std::uint32_t max_retries = 5,
                                                   std::uint32_t backoff_ms = 1000) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // YADDNSC_DNS_DISPATCHER_H
