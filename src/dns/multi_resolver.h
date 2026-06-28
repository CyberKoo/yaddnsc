//
// Created by Kotarou on 2026/6/28.
//

#ifndef YADDNSC_DNS_MULTI_RESOLVER_H
#define YADDNSC_DNS_MULTI_RESOLVER_H

#include <memory>
#include <string>
#include <vector>
#include <optional>

#include "type.h"
#include "config/config.h"

class ResolverBase;

// ---------------------------------------------------------------------------
// MultiResolver — dispatches DNS queries across one or more backend resolvers
//                 using a configurable strategy (fallback / concurrent), with
//                 automatic retry on transient errors.
//
// Eliminates the need to pass resolver vectors through every layer.
// Thread-safe: resolve() is const and does not mutate shared state.
// ---------------------------------------------------------------------------
class MultiResolver {
public:
    MultiResolver();

    explicit MultiResolver(std::vector<std::shared_ptr<ResolverBase>> resolvers,
                           Config::resolver_strategy strategy = Config::resolver_strategy::CONCURRENT);

    ~MultiResolver();

    MultiResolver(MultiResolver &&) noexcept;
    MultiResolver &operator=(MultiResolver &&) noexcept;

    [[nodiscard]] std::optional<std::string>
    resolve(const std::string &host, dns_type type, int max_retries = 5, int backoff_ms = 1000) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // YADDNSC_DNS_MULTI_RESOLVER_H
