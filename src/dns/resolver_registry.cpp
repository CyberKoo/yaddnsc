//
// Created by Kotarou on 2026/7/8.
//

#include "resolver_registry.h"

#include <string>
#include <unordered_map>

#include "uri.h"
#include "fmt.hpp"
#include "dns_error.h"
#include "dns/resolver/base.h"
#include "exception/dns_lookup.h"

namespace DnsResolverRegistry {

namespace {

/// Meyer's singleton: the schema→factory map.
/// Function-local static guarantees initialisation before first use,
/// avoiding static initialisation order fiasco across translation units.
std::unordered_map<std::string, FactoryFn> &factories() {
    static std::unordered_map<std::string, FactoryFn> reg;
    return reg;
}

}  // anonymous namespace

void register_factory(std::string_view schema, FactoryFn factory) {
    factories()[std::string(schema)] = std::move(factory);
}

std::shared_ptr<ResolverBase> create(const DnsServer &server) {
    auto uri = Uri::parse(server.address);
    auto schema = std::string(uri.get_schema());

    auto &reg = factories();
    auto it = reg.find(schema);

    // Fallback to the default resolver (empty schema) only when the URI
    // had no explicit schema (e.g. bare IP "1.1.1.1").  If an unknown
    // schema was explicitly given (e.g. "tls1://..."), we error out
    // below rather than silently routing to the generic fallback.
    if (it == reg.end() && schema.empty()) {
        it = reg.find("");
    }

    if (it == reg.end()) {
        throw DnsLookupException(
            fmt::format(R"(No resolver factory registered for schema "{}" (server: {}))",
                        schema, server.address),
            DnsError::CONFIG
        );
    }

    return it->second(server);
}

}  // namespace DnsResolverRegistry
