//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_DNS_RESOLVER_FACTORY_H
#define YADDNSC_DNS_RESOLVER_FACTORY_H

#include "multi_resolver.h"

namespace Config {
    struct config;
}

// ---------------------------------------------------------------------------
// DnsResolverFactory — constructs a MultiResolver from application config.
//
// Extracted from Manager::Impl to isolate URI-parsing and resolver-type
// selection logic into a single, independently testable component.
// ---------------------------------------------------------------------------
struct DnsResolverFactory {
    static MultiResolver create(const Config::config &config);
};

#endif // YADDNSC_DNS_RESOLVER_FACTORY_H
