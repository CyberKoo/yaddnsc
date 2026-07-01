//
// Created by Kotarou on 2026/6/29.
//

#include "resolver_factory.h"

#include <memory>
#include <vector>

#include <spdlog/spdlog.h>

#include "uri.h"
#include "doh.h"
#include "dot.h"
#include "classic.h"
#include "network/http_client.h"
#include "config/config.h"

ResolverDispatcher DnsResolverFactory::create(const Config::config &config) {
    // Build the list of DNS servers from config, preserving backward
    // compatibility with the legacy single-server format.
    std::vector<dns_server_type> dns_servers;
    if (config.resolver.use_custom_server) {
        if (!config.resolver.servers.empty()) {
            dns_servers = config.resolver.servers;
        } else if (!config.resolver.address.empty()) {
            // Legacy single-server format.
            dns_servers.push_back({config.resolver.address, config.resolver.port});
        }
    }

    // Build resolver objects from server configurations.
    // Each address is parsed as a URI to determine the protocol:
    //   https://...  → DohResolver (DNS-over-HTTPS, with a default
    //   PersistentHttpClient) tls://...    → DotResolver (DNS-over-TLS)
    //   otherwise    → ClassicResolver (traditional UDP/TCP DNS)
    std::vector<std::shared_ptr<ResolverBase> > resolvers;
    for (const auto &server: dns_servers) {
        const auto uri = Uri::parse(server.address);
        if (uri.get_schema() == "https") {
            auto opts = HttpClientOptions{
                .connection_timeout = std::chrono::seconds(1), .read_timeout = std::chrono::seconds(5)
            };
            auto http_client = std::make_unique<PersistentHttpClient>(uri, opts);
            auto resolver = std::make_shared<DohResolver>(std::move(http_client), server.address);
            SPDLOG_INFO("DNS resolver #{}: {} ({})", resolver->get_id(), uri.get_origin(), resolver->get_type());
            resolvers.push_back(std::move(resolver));
        } else if (uri.get_schema() == "tls") {
            auto resolver = std::make_shared<DotResolver>(std::string(uri.get_host()), uri.get_port());
            SPDLOG_INFO("DNS resolver #{}: {} ({})", resolver->get_id(), uri.get_origin(), resolver->get_type());
            resolvers.push_back(std::move(resolver));
        } else {
            auto resolver = std::make_shared<ClassicResolver>(server);
            SPDLOG_INFO("DNS resolver #{}: {}:{} ({})", resolver->get_id(), uri.get_host_literal(), server.port,
                        resolver->get_type());
            resolvers.push_back(std::move(resolver));
        }
    }

    // Log configured custom resolver count and strategy — once at startup.
    if (dns_servers.size() > 1) {
        SPDLOG_INFO("Configured {} custom resolver(s) in {} mode", dns_servers.size(),
                    config.resolver.strategy == Config::resolver_strategy::FALLBACK ? "fallback" : "concurrent"
        );
    }

    return ResolverDispatcher(std::move(resolvers), config.resolver.strategy);
}
