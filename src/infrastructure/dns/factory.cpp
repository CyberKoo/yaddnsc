//
// Created by Kotarou on 2026/6/29.
//

#include "factory.h"

#include <memory>
#include <utility>
#include <vector>

#include "domain/config/runtime_config.h"
#include "domain/config/dns_config.h"
#include "infrastructure/dns/resolver/base.h"
#include "infrastructure/dns/resolver_catalog.h"

#include "resolver_config.h"
#include "infrastructure/network/uri.h"

#include <spdlog/spdlog.h>

// ===========================================================================
// DnsResolverFactory::create — build a ResolverDispatcher from resolver settings.
// ===========================================================================

ResolverDispatcher DnsResolverFactory::create(const domain::ResolverSettings &settings,
                                              const Utils::CancellationToken &token,
                                              const ResolverCatalog &catalog) {
    // The server list arrives already normalised (legacy single-server format
    // folded in by the config normaliser).
    std::vector<Config::DnsServer> dns_servers = settings.servers;

    // Ensure at least one DNS server is available.
    if (dns_servers.empty()) {
        dns_servers.push_back({YADDNSC_DEFAULT_DNS_SERVER, YADDNSC_DEFAULT_DNS_PORT});
    }

    // Build resolver objects from server configurations, dispatching on the
    // URI schema via the catalog (https → DohResolver, tls → DotResolver,
    // "" → ClassicResolver).
    std::vector<std::unique_ptr<ResolverBase> > resolvers;
    for (const auto &server: dns_servers) {
        resolvers.push_back(catalog.create(server, token));
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
