//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_DNS_FACTORY_H
#define YADDNSC_DNS_FACTORY_H

#include "dispatcher.h"

namespace domain {
    struct ResolverSettings;
}

namespace Utils {
class CancellationToken;
}

/// DnsResolverFactory — constructs a ResolverDispatcher from resolver settings.
///
/// Extracted from Manager::Impl to isolate URI-parsing and resolver-type
/// selection logic into a single, independently testable component.
namespace DnsResolverFactory {
    /// Build a fully-configured ResolverDispatcher from resolver settings.
    /// @param settings  Normalised resolver settings (legacy fields folded in).
    /// @param token     Cancellation token bound into every created resolver.
    /// @return          A ResolverDispatcher ready for use.
    [[nodiscard]] ResolverDispatcher create(const domain::ResolverSettings &settings,
                                            const Utils::CancellationToken &token);
} // namespace DnsResolverFactory

#endif  // YADDNSC_DNS_FACTORY_H
