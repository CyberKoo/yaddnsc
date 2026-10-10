#include "diagnostics.h"

#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <chrono>
#include <expected>
#include <string>
#include <utility>

#include "application/ports/network_interfaces.h"
#include "domain/dns/record_kind.h"
#include "domain/fqdn.h"
#include "coro/scope.hpp"
#include "support/fmt.hpp"
#include "application/ports/gateway.h"
#include "application/ports/resolver.h"
#include "domain/config/runtime_config.h"
#include "domain/error/dns_error.h"
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

namespace app {

std::vector<DriverListItem> list_drivers(const DriverCatalogPort& catalog) {
    std::vector<DriverListItem> items;
    for (const auto& name : catalog.loaded_drivers()) {
        items.push_back(DriverListItem{.name = name, .detail = catalog.describe(name)});
    }
    return items;
}

std::vector<InterfaceListItem> list_interfaces(const NetworkInterfacesPort& interfaces) {
    std::vector<InterfaceListItem> items;
    for (const auto& name : interfaces.names()) {
        // names() only yields interfaces from the same cached snapshot that
        // addresses() reads, so a missing entry degrades to an empty row.
        items.push_back(InterfaceListItem{
            .name = name, .addresses = interfaces.addresses(name).value_or(std::vector<domain::InetAddress>{})});
    }
    return items;
}

coro::Task<DnsResolveOutcome> dns_resolve(ResolverPort& resolver, std::string host, std::string type_text) {
    DnsResolveOutcome outcome{.host = std::move(host), .type_text = std::move(type_text), .lookup = std::nullopt};

    const auto type = domain::record_kind_from_str(outcome.type_text);
    if (!type.has_value()) {
        co_return outcome;  // lookup stays nullopt — unknown record type
    }

    outcome.lookup = co_await resolver.resolve(outcome.host, *type);
    co_return outcome;
}

coro::Task<DnsResolveOutcome> dns_resolve_command(ResolverPort& resolver, std::string host, std::string type_text,
                                                  coro::Duration budget) {
    auto bounded = co_await coro::with_timeout(
        budget, [&resolver, &host, &type_text] { return dns_resolve(resolver, host, type_text); });
    if (bounded.timed_out) {
        co_return DnsResolveOutcome{
            .host = std::move(host),
            .type_text = std::move(type_text),
            .lookup = std::unexpected(
                domain::DnsErrorInfo{domain::DnsError::CONNECTION,
                                     fmt::format("DNS lookup timed out after {}s",
                                                 std::chrono::duration_cast<std::chrono::seconds>(budget).count())}),
        };
    }
    co_return std::move(*bounded);
}

coro::Task<std::expected<void, std::string>> validate_driver_configs(GatewayPort& gateway,
                                                                     const domain::RuntimeConfig& config) {
    for (const auto& domain_config : config.domains) {
        for (const auto& subdomain : domain_config.subdomains) {
            const auto result = co_await gateway.validate_config(domain_config.driver, subdomain.driver_params);
            if (!result) {
                co_return std::unexpected(
                    fmt::format("Driver '{}' rejected configuration for {}: {}", domain_config.driver,
                                domain::make_fqdn(domain_config.name, subdomain.name), result.error().message));
            }
        }
    }
    co_return std::expected<void, std::string>{};
}

}  // namespace app
