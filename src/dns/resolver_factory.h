//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_DNS_RESOLVER_FACTORY_H
#define YADDNSC_DNS_RESOLVER_FACTORY_H

#include "resolver_dispatcher.h"

namespace Config {
    struct config;
}

// ---------------------------------------------------------------------------
// DnsResolverFactory — constructs a ResolverDispatcher from application
//                      config.
//
// Extracted from Manager::Impl to isolate URI-parsing and resolver-type
// selection logic into a single, independently testable component.
// ---------------------------------------------------------------------------
struct DnsResolverFactory {
    static ResolverDispatcher create(const Config::config &config);
};

#endif // YADDNSC_DNS_RESOLVER_FACTORY_H
