//
// Created by Kotarou on 2026/9/17.
//

#include "resolver_catalog.h"

#include <cstdint>
#include <utility>

#include <yaddnsc/util/format.hpp>

#include "domain/error/dns_error.h"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/resolver/base.h"
#include "infrastructure/dns/resolver/classic.h"
#include "infrastructure/dns/resolver/doh.h"
#include "infrastructure/dns/resolver/dot.h"
#include "infrastructure/network/uri.h"
#include "support/fmt.hpp"
#include "support/util/cancellation_token.hpp"  // IWYU pragma: keep — resolvers take the token by value

void ResolverCatalog::register_factory(std::string_view schema, FactoryFn factory) {
    factories_[std::string(schema)] = std::move(factory);
}

ResolverCatalog ResolverCatalog::with_builtins() {
    ResolverCatalog catalog;

    catalog.register_factory(
        "",
        [](const Config::DnsServer& server, const Utils::CancellationToken& token) -> std::unique_ptr<ResolverBase> {
            return std::make_unique<ClassicResolver>(server, token);
        });

    // DoH resolver: port is read from the URI only; server.port is intentionally
    // ignored because the URI already specifies the port (e.g. https://1.1.1.1:1443/dns-query).
    // If no port is present in the URI, the default is 443.
    catalog.register_factory(
        "https",
        [](const Config::DnsServer& server, const Utils::CancellationToken& token) -> std::unique_ptr<ResolverBase> {
            auto uri = Uri::parse(server.address);
            auto host = std::string(uri.get_host());
            auto port = static_cast<std::uint16_t>(uri.get_port() != 0 ? uri.get_port() : 443);
            auto path = std::string(uri.get_path());
            if (path.empty()) {
                path = "/";
            }
            return std::make_unique<DohResolver>(std::move(host), port, std::move(path), std::string(uri.get_origin()),
                                                 token);
        });

    // DoT resolver: port is read from the URI only; server.port is intentionally
    // ignored because the URI already specifies the port (e.g. tls://1.1.1.1:853).
    // If no port is present in the URI, the default is 853.
    catalog.register_factory(
        "tls",
        [](const Config::DnsServer& server, const Utils::CancellationToken& token) -> std::unique_ptr<ResolverBase> {
            auto uri = Uri::parse(server.address);
            auto host = std::string(uri.get_host());
            auto port = static_cast<std::uint16_t>(uri.get_port() != 0 ? uri.get_port() : 853);
            return std::make_unique<DotResolver>(std::move(host), port, std::string(uri.get_origin()), token);
        });

    return catalog;
}

std::unique_ptr<ResolverBase> ResolverCatalog::create(const Config::DnsServer& server,
                                                      const Utils::CancellationToken& token) const {
    auto uri = Uri::parse(server.address);
    auto schema = std::string(uri.get_schema());

    auto it = factories_.find(schema);

    // Fallback to the default resolver (empty schema) only when the URI
    // had no explicit schema (e.g. bare IP "1.1.1.1").  If an unknown
    // schema was explicitly given (e.g. "tls1://..."), we error out
    // below rather than silently routing to the generic fallback.
    if (it == factories_.end() && schema.empty()) {
        it = factories_.find("");
    }

    if (it == factories_.end()) {
        throw DnsLookupException(
            fmt::format(R"(No resolver factory registered for schema "{}" (server: {}))", schema, server.address),
            DnsError::CONFIG);
    }

    return it->second(server, token);
}
