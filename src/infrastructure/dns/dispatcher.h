//
// Created by Kotarou on 2026/6/28.
//

#ifndef YADDNSC_DNS_DISPATCHER_H
#define YADDNSC_DNS_DISPATCHER_H

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "domain/config/dns_config.h"

#include "application/ports/dns_resolver.h"
#include "domain/error/dns_error_info.h"
#include "domain/dns/record_kind.h"

class ResolverBase;

/// ResolverDispatcher — dispatches DNS queries across one or more backend
///                      resolvers using a configurable strategy (fallback /
///                      shuffle / concurrent), with automatic retry on
///                      transient errors.
///
/// Implements the application-facing DnsResolverPort: the two-argument
/// resolve() override applies the default retry policy (1 retry, 50 ms base
/// backoff, single-resolver mode only).
///
/// Eliminates the need to pass resolver vectors through every layer.
/// @note Thread-safe: resolve() is const and does not mutate shared state.
class ResolverDispatcher : public DnsResolverPort {
public:
    /// Construct with a list of resolver backends and a dispatch strategy.
    /// @param resolvers  Vector of resolver backends to query.
    /// @param strategy   Dispatch strategy (fallback, shuffle, or concurrent).
    explicit ResolverDispatcher(std::vector<std::unique_ptr<ResolverBase> > resolvers,
                                Config::ResolverStrategy strategy = Config::ResolverStrategy::CONCURRENT);

    ~ResolverDispatcher() override;

    ResolverDispatcher(ResolverDispatcher &&) noexcept;

    ResolverDispatcher &operator=(ResolverDispatcher &&) noexcept;

    /// Resolve a hostname using the configured strategy and backends,
    /// with the default retry policy (max_retries = 1, backoff_ms = 50).
    [[nodiscard]] std::expected<std::vector<std::string>, DnsErrorInfo>
    resolve(std::string_view host, RecordKind type) const override;

    /// Resolve a hostname using the configured strategy and backends.
    ///
    /// Retry behaviour depends on the dispatch strategy:
    ///   - Single resolver: retries up to `max_retries` times with
    ///     exponential-like backoff on transient errors.
    ///   - Multiple resolvers (fallback / shuffle / concurrent): no per-query retries;
    ///     fault tolerance is provided by resolver redundancy.
    ///
    /// @param host         Hostname to resolve.
    /// @param type         DNS record type (A or AAAA).
    /// @param max_retries  Maximum number of retries on transient errors
    ///                     (single-resolver mode only; ignored in multi-resolver mode).
    /// @param backoff_ms   Base backoff interval in milliseconds
    ///                     (single-resolver mode only).
    /// @return             Resolved IP strings on success, or a DnsErrorInfo
    ///                     describing the failure.  Callers should check the
    ///                     error code to distinguish transient (RETRY, CONNECTION)
    ///                     from permanent errors (NX_DOMAIN, NODATA, PARSE, CONFIG).
    [[nodiscard]] std::expected<std::vector<std::string>, DnsErrorInfo>
    resolve(std::string_view host, RecordKind type, std::uint32_t max_retries,
            std::uint32_t backoff_ms) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // YADDNSC_DNS_DISPATCHER_H
