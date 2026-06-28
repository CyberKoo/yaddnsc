//
// Created by Kotarou on 2026/6/28.
//

#ifndef YADDNSC_DNS_RESOLVER_BASE_H
#define YADDNSC_DNS_RESOLVER_BASE_H

#include <string>
#include <vector>
#include <cstdint>

#include "type.h"
#include "mixin.h"

// ---------------------------------------------------------------------------
// ResolverBase — common interface for all DNS resolvers.
//
// Provides the shared contract (query + non-copyable/non-movable semantics)
// so that callers can work with any resolver type polymorphically.
// ---------------------------------------------------------------------------
class ResolverBase {
public:
    virtual ~ResolverBase() = default;

    [[nodiscard]] virtual std::vector<uint8_t> query(const std::string &host, dns_type type) const = 0;

private:
    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

#endif // YADDNSC_DNS_RESOLVER_BASE_H
