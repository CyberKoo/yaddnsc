//
// Created by Kotarou on 2026/6/29.
//

#include "factory.h"

#include <memory>
#include <utility>
#include <vector>

#include "config/config.h"
#include "config/dns_config.h"
#include "dns/resolver/base.h"
#include "dns/resolver_registry.h"

#include "resolver_config.h"
#include "uri.h"

#include <spdlog/spdlog.h>

// ===========================================================================
// DnsResolverFactory::create — build a ResolverDispatcher from app config.
// ===========================================================================

ResolverDispatcher DnsResolverFactory::create(const Config::AppConfig &config) {
    // Build the list of DNS servers from config, preserving backward
    // compatibility with the legacy single-server format.
    std::vector<Config::DnsServer> dns_servers;
    if (config.resolver.use_custom_server) {
        if (!config.resolver.servers.empty()) {
            dns_servers = config.resolver.servers;
        } else if (!config.resolver.address.empty()) {
            // Legacy single-server format.
            dns_servers.push_back({config.resolver.address, config.resolver.port});
        }
    }

    // Ensure at least one DNS server is available.
    if (dns_servers.empty()) {
        dns_servers.push_back({YADDNSC_DEFAULT_DNS_SERVER, YADDNSC_DEFAULT_DNS_PORT});
    }

    // Build resolver objects from server configurations.
    // Each resolver registers itself via DnsResolverRegistry, keyed by
    // URI schema (https → DohResolver, tls → DotResolver, "" → ClassicResolver).
    std::vector<std::unique_ptr<ResolverBase> > resolvers;
    for (const auto &server: dns_servers) {
        resolvers.push_back(DnsResolverRegistry::create(server));
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
                    strategy_name(config.resolver.strategy));
    }

    return ResolverDispatcher(std::move(resolvers), config.resolver.strategy);
}
