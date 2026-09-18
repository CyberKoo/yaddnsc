//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_DNS_FACTORY_H
#define YADDNSC_DNS_FACTORY_H

#include "dispatcher.h"

class ResolverCatalog;

namespace domain {
    struct ResolverSettings;
}

/// DnsResolverFactory — constructs a ResolverDispatcher from resolver settings.
///
/// Extracted from Manager::Impl to isolate URI-parsing and resolver-type
/// selection logic into a single, independently testable component.
namespace DnsResolverFactory {
    /// Build a fully-configured ResolverDispatcher from resolver settings.
    /// @param settings  Valid, normalised resolver settings with at least one
    ///                  server (legacy fields already folded in).
    /// @param catalog   Resolver catalog used to dispatch on the URI schema
    ///                  (production: ResolverCatalog::with_builtins()).
    /// @return          A ResolverDispatcher ready for use.
    /// @throws std::invalid_argument if the RuntimeConfig invariant is broken.
    [[nodiscard]] ResolverDispatcher create(const domain::ResolverSettings &settings,
                                            const ResolverCatalog &catalog);
} // namespace DnsResolverFactory

#endif  // YADDNSC_DNS_FACTORY_H
