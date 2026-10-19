#include "factory.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "domain/config/runtime_config.h"
#include "infrastructure/dns/bootstrap/bootstrap.h"
#include "infrastructure/dns/endpoint.h"
#include "infrastructure/dns/resolver/classic.h"
#include "infrastructure/dns/resolver/doh.h"
#include "infrastructure/dns/resolver/dot.h"
#include "infrastructure/dns/resolver/resolver.h"
#include "infrastructure/http/types.h"
#include "support/redact.hpp"

#include "version.h"

namespace dns {
namespace {

[[nodiscard]] Strategy to_strategy(domain::ResolverStrategy strategy) noexcept {
    switch (strategy) {
        case domain::ResolverStrategy::FALLBACK:
            return Strategy::FALLBACK;
        case domain::ResolverStrategy::SHUFFLE:
            return Strategy::SHUFFLE;
        case domain::ResolverStrategy::CONCURRENT:
            return Strategy::CONCURRENT;
    }
    return Strategy::CONCURRENT;
}

[[nodiscard]] std::unique_ptr<Resolver> make_backend(const ResolverEndpoint& endpoint,
                                                     const std::vector<domain::DnsServer>& bootstrap,
                                                     std::shared_ptr<const net::TlsContext> tls_context,
                                                     Policy policy) {
    switch (endpoint.kind) {
        case ResolverEndpoint::Kind::CLASSIC:
            return std::make_unique<ClassicResolver>(*endpoint.literal, endpoint.port, policy);
        case ResolverEndpoint::Kind::DOT: {
            EndpointOptions options;
            options.policy = policy;
            options.bootstrap_dns = bootstrap;
            options.tls_context = std::move(tls_context);
            return std::make_unique<DotResolver>(std::string(endpoint.uri.get_host()), endpoint.port,
                                                 std::move(options));
        }
        case ResolverEndpoint::Kind::DOH: {
            http::Options options;
            options.user_agent = YADDNSC::get_full_version();
            options.resolve = make_bootstrap_resolver(bootstrap, policy);
            options.tls_context = std::move(tls_context);
            return std::make_unique<DohResolver>(endpoint.uri, std::move(options), policy);
        }
    }
    std::unreachable();
}

}  // namespace

std::unique_ptr<Dispatcher> make_dispatcher(const domain::ResolverSettings& settings,
                                            const std::vector<domain::DnsServer>& bootstrap,
                                            const std::shared_ptr<const net::TlsContext>& tls_context, Policy policy) {
    if (settings.servers.empty()) {
        throw std::invalid_argument("ResolverSettings.servers must not be empty");
    }
    std::vector<std::unique_ptr<Resolver>> backends;
    backends.reserve(settings.servers.size());
    for (const auto& server : settings.servers) {
        auto endpoint = parse_resolver_endpoint(server);
        if (!endpoint) {
            throw std::invalid_argument(endpoint.error().front());
        }
        backends.push_back(make_backend(*endpoint, bootstrap, tls_context, policy));
        const auto& uri = endpoint->uri;
        const auto type = endpoint->kind == ResolverEndpoint::Kind::CLASSIC ? "classic"
                          : endpoint->kind == ResolverEndpoint::Kind::DOT   ? "DoT"
                                                                            : "DoH";
        SPDLOG_INFO("DNS resolver #{}: \"{}\" ({})", backends.size(),
                    Utils::redacted_uri_credentials(uri.get_schema().empty() ? std::string(uri.get_host_literal())
                                                                             : uri.get_origin()),
                    type);
    }
    if (settings.servers.size() > 1) {
        const auto strategy_name = [](domain::ResolverStrategy strategy) {
            switch (strategy) {
                case domain::ResolverStrategy::FALLBACK:
                    return "fallback";
                case domain::ResolverStrategy::SHUFFLE:
                    return "shuffle";
                case domain::ResolverStrategy::CONCURRENT:
                    return "concurrent";
            }
            std::unreachable();
        };
        SPDLOG_INFO("Configured {} custom resolver(s) in {} mode", settings.servers.size(),
                    strategy_name(settings.strategy));
    }
    return std::make_unique<Dispatcher>(std::move(backends), to_strategy(settings.strategy), policy);
}

}  // namespace dns
