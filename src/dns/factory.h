//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_DNS_FACTORY_H
#define YADDNSC_DNS_FACTORY_H

#include "dispatcher.h"

namespace Config {
    struct AppConfig;
}

/// DnsResolverFactory — constructs a ResolverDispatcher from application config.
///
/// Extracted from Manager::Impl to isolate URI-parsing and resolver-type
/// selection logic into a single, independently testable component.
namespace DnsResolverFactory {
    /// Build a fully-configured ResolverDispatcher from application config.
    /// @param config  Application configuration with resolver settings.
    /// @return        A ResolverDispatcher ready for use.
    [[nodiscard]] ResolverDispatcher create(const Config::AppConfig &config);
} // namespace DnsResolverFactory

#endif  // YADDNSC_DNS_FACTORY_H
