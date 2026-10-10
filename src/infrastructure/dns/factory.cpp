//
// dns — build a coroutine dispatcher from resolver settings (implementation).
//

#include "factory.h"

#include <spdlog/spdlog.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <expected>
#include <optional>

#include "domain/network/inet_address.h"
#include "infrastructure/dns/bootstrap/bootstrap.h"
#include "infrastructure/dns/resolver/classic.h"
#include "infrastructure/dns/resolver/doh.h"
#include "infrastructure/dns/resolver/dot.h"
#include "infrastructure/uri/uri.h"
#include "support/fmt.hpp"
#include "support/redact.hpp"
#include "version.h"
#include "domain/config/dns_config.h"
#include "domain/config/runtime_config.h"
#include "infrastructure/dns/resolver/resolver.h"
#include "infrastructure/http/types.h"
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

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

[[nodiscard]] std::unique_ptr<Resolver> make_backend(const domain::DnsServer& server,
                                                     const std::vector<domain::DnsServer>& bootstrap,
                                                     std::shared_ptr<const net::TlsContext> tls_context) {
    const auto uri = Uri::parse(server.address);
    if (!uri.has_value()) {
        throw std::invalid_argument(fmt::format(R"(Malformed resolver address "{}")", server.address));
    }

    const std::string schema{uri->get_schema()};
    if (schema.empty()) {
        // The bare authority host, never get_host_literal(): that accessor
        // brackets IPv6 for URL building, so "[2606:4700:4700::1111]" would
        // reach inet_pton with its brackets and be rejected as a non-literal.
        const auto address = domain::InetAddress::parse(uri->get_host());
        if (!address.has_value()) {
            throw std::invalid_argument(
                fmt::format(R"(Classic resolver address "{}" is not an IP literal)", server.address));
        }
        return std::make_unique<ClassicResolver>(*address, server.port);
    }
    if (schema == "tls") {
        const auto port = static_cast<std::uint16_t>(uri->get_port() != 0 ? uri->get_port() : 853);
        EndpointOptions options;
        options.bootstrap_dns = bootstrap;
        options.tls_context = std::move(tls_context);
        return std::make_unique<DotResolver>(std::string(uri->get_host()), port, std::move(options));
    }
    if (schema == "https") {
        http::Options options;
        // The legacy DoH resolver always identified itself on the wire, and
        // bounded the connect phase at 1s (the read phase is EXCHANGE_BUDGET's).
        options.user_agent = YADDNSC::get_full_version();
        options.connect_timeout = std::chrono::seconds(1);
        options.resolve = make_bootstrap_resolver(bootstrap);
        options.tls_context = std::move(tls_context);
        return std::make_unique<DohResolver>(server.address, std::move(options));
    }
    throw std::invalid_argument(
        fmt::format(R"(No coroutine resolver backend for schema "{}" (server: {}))", schema, server.address));
}

}  // namespace

std::unique_ptr<Dispatcher> make_dispatcher(const domain::ResolverSettings& settings,
                                            std::vector<domain::DnsServer> bootstrap,
                                            std::shared_ptr<const net::TlsContext> tls_context) {
    if (settings.servers.empty()) {
        throw std::invalid_argument("ResolverSettings.servers must not be empty");
    }

    const auto backend_type = [](std::string_view schema) -> std::string_view {
        if (schema == "tls") {
            return "DoT";
        }
        if (schema == "https") {
            return "DoH";
        }
        return "classic";
    };

    std::vector<std::unique_ptr<Resolver>> backends;
    backends.reserve(settings.servers.size());
    for (const auto& server : settings.servers) {
        backends.push_back(make_backend(server, bootstrap, tls_context));
        // make_backend() already validated the address, so this cannot fail.
        const auto uri = Uri::parse(server.address).value();
        SPDLOG_INFO("DNS resolver #{}: {} ({})", backends.size(),
                    Utils::redacted_uri_credentials(
                        uri.get_schema().empty() ? std::string(uri.get_host_literal()) : uri.get_origin()),
                    backend_type(uri.get_schema()));
    }

    // Log configured custom resolver count and strategy — once at startup.
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

    return std::make_unique<Dispatcher>(std::move(backends), to_strategy(settings.strategy));
}

}  // namespace dns
