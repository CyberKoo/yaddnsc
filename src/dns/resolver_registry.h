//
// Created by Kotarou on 2026/7/8.
//

#ifndef YADDNSC_DNS_RESOLVER_REGISTRY_H
#define YADDNSC_DNS_RESOLVER_REGISTRY_H

#include <functional>
#include <memory>
#include <string_view>

#include "config/dns_config.h"

class ResolverBase;

/// ResolverRegistry — self-registering factory for DNS resolver types.
///
/// Each resolver implementation registers its factory function via a
/// Registrar static at file scope.  The registry maps URI schemas
/// ("https", "tls", "" for classic) to the corresponding factory.
///
/// Usage in a resolver .cpp:
/// @code
///   namespace {
///   [[maybe_unused]] DnsResolverRegistry::Registrar _reg(
///       "https",
///       [](const DnsServer& server) -> std::unique_ptr<ResolverBase> {
///           return std::make_unique<DohResolver>(...);
///       }
///   );
///   }
/// @endcode
///
/// Factory callers simply do:
/// @code
///   auto resolver = DnsResolverRegistry::create(server);
/// @endcode
namespace DnsResolverRegistry {

/// Factory function type: receives a DNS server config and returns a resolver.
using FactoryFn = std::function<std::shared_ptr<ResolverBase>(const Config::DnsServer&)>;

/// Register a factory for the given URI schema.
/// @param schema   URI schema (e.g. "https", "tls"). Empty string is the
///                 fallback for unrecognised schemas.
/// @param factory  Factory function that constructs the resolver.
void register_factory(std::string_view schema, FactoryFn factory);

/// Create a resolver for the given server address.
///
/// Parses the server address as a URI to determine the schema,
/// then dispatches to the registered factory.
///
/// @param server  DNS server address and port.
/// @return        A new resolver instance.
/// @throws DnsLookupException  If no factory is registered for the schema.
[[nodiscard]] std::shared_ptr<ResolverBase> create(const Config::DnsServer& server);

/// RAII helper for static registration.
///
/// Usage in anonymous namespace at file scope:
/// @code
///   [[maybe_unused]] Registrar _reg("https", factory_fn);
/// @endcode
struct Registrar {
    Registrar(std::string_view schema, FactoryFn factory) {
        register_factory(schema, std::move(factory));
    }
};

}  // namespace DnsResolverRegistry

#endif  // YADDNSC_DNS_RESOLVER_REGISTRY_H
