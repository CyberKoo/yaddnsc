//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_DNS_RESOLVER_CATALOG_H
#define YADDNSC_DNS_RESOLVER_CATALOG_H

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "domain/config/dns_config.h"

class ResolverBase;

/// ResolverCatalog — instance-level factory catalog for DNS resolver types.
///
/// Replaces the former process-wide DnsResolverRegistry: the schema→factory
/// map now lives in an explicit instance, so tests can inject stub resolvers
/// without touching shared global state (parallel-safe).
///
/// Production uses ResolverCatalog::with_builtins(), which maps URI schemas
/// ("https", "tls", "" for classic) to the corresponding resolver factories.
class ResolverCatalog {
public:
    /// Factory function type: receives a DNS server config and returns a
    /// resolver.  Cancellation is not bound at construction; it flows
    /// through ResolverBase::query().
    using FactoryFn = std::function<std::unique_ptr<ResolverBase>(const Config::DnsServer &)>;

    /// Register a factory for the given URI schema.
    /// @param schema   URI schema (e.g. "https", "tls"). Empty string is the
    ///                 fallback for schema-less server addresses.
    /// @param factory  Factory function that constructs the resolver.
    void register_factory(std::string_view schema, FactoryFn factory);

    /// A catalog carrying the built-in resolver factories:
    /// "" → ClassicResolver, "https" → DohResolver, "tls" → DotResolver.
    [[nodiscard]] static ResolverCatalog with_builtins();

    /// Create a resolver for the given server address.
    ///
    /// Parses the server address as a URI to determine the schema,
    /// then dispatches to the registered factory.
    ///
    /// @param server  DNS server address and port.
    /// @return        A new resolver instance.
    /// @throws DnsLookupException  If no factory is registered for the schema.
    [[nodiscard]] std::unique_ptr<ResolverBase> create(const Config::DnsServer &server) const;

private:
    std::unordered_map<std::string, FactoryFn> factories_;
};

#endif // YADDNSC_DNS_RESOLVER_CATALOG_H
