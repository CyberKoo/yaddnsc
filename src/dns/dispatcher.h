//
// Created by Kotarou on 2026/6/28.
//

#ifndef YADDNSC_DNS_DISPATCHER_H
#define YADDNSC_DNS_DISPATCHER_H

#include <memory>
#include <string>
#include <vector>

#include "config/config.h"

class ResolverBase;

// ---------------------------------------------------------------------------
// ResolverDispatcher — dispatches DNS queries across one or more backend
//                      resolvers using a configurable strategy (fallback /
//                      concurrent), with automatic retry on transient errors.
//
// Eliminates the need to pass resolver vectors through every layer.
// Thread-safe: resolve() is const and does not mutate shared state.
// ---------------------------------------------------------------------------
class ResolverDispatcher {
public:
    ResolverDispatcher();

    explicit ResolverDispatcher(std::vector<std::shared_ptr<ResolverBase> > resolvers,
                                Config::ResolverStrategy strategy = Config::ResolverStrategy::CONCURRENT);

    ~ResolverDispatcher();

    ResolverDispatcher(ResolverDispatcher &&) noexcept;

    ResolverDispatcher &operator=(ResolverDispatcher &&) noexcept;

    [[nodiscard]] std::vector<std::string>
    resolve(const std::string &host, DNS::Type type, int max_retries = 5, int backoff_ms = 1000) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // YADDNSC_DNS_DISPATCHER_H
