//
// Created by Kotarou on 2026/6/29.
//

#include "factory.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "domain/config/dns_config.h"
#include "domain/config/runtime_config.h"
#include "infrastructure/dns/dispatcher.h"
#include "infrastructure/dns/resolver/base.h"
#include "infrastructure/dns/resolver_catalog.h"
#include "infrastructure/network/uri.h"

// ===========================================================================
// DnsResolverFactory::create — build a ResolverDispatcher from resolver settings.
// ===========================================================================

ResolverDispatcher DnsResolverFactory::create(const domain::ResolverSettings& settings,
                                              const ResolverCatalog& catalog) {
    // The server list arrives fully normalized. An empty list is an internal
    // invariant violation: configuration intent must never be guessed here.
    if (settings.servers.empty()) {
        throw std::invalid_argument("ResolverSettings.servers must not be empty");
    }

    const auto& dns_servers = settings.servers;

    // Build resolver objects from server configurations, dispatching on the
    // URI schema via the catalog (https → DohResolver, tls → DotResolver,
    // "" → ClassicResolver).
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    for (const auto& server : dns_servers) {
        resolvers.push_back(catalog.create(server));
        const auto uri = Uri::parse(server.address);
        SPDLOG_INFO("DNS resolver #{}: {} ({})", resolvers.back()->get_id(),
                    uri.get_schema().empty() ? uri.get_host_literal() : uri.get_origin(), resolvers.back()->get_type());
    }

    // Log configured custom resolver count and strategy — once at startup.
    if (dns_servers.size() > 1) {
        const auto strategy_name = [](Config::ResolverStrategy strategy) {
            switch (strategy) {
                case Config::ResolverStrategy::FALLBACK:
                    return "fallback";
                case Config::ResolverStrategy::SHUFFLE:
                    return "shuffle";
                case Config::ResolverStrategy::CONCURRENT:
                    return "concurrent";
            }
            std::unreachable();
        };
        SPDLOG_INFO("Configured {} custom resolver(s) in {} mode", dns_servers.size(),
                    strategy_name(settings.strategy));
    }

    return ResolverDispatcher(std::move(resolvers), settings.strategy);
}
