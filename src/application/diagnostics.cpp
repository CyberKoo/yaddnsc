//
// Created by Kotarou on 2026/9/17.
//

#include "diagnostics.h"

#include <exception>
#include <utility>

#include <magic_enum/magic_enum.hpp>

#include "application/ports/dns_resolver.h"
#include "application/ports/network_interfaces.h"

enum class RecordKind;

namespace Diagnostics {

std::vector<DriverListItem> list_drivers(const DriverCatalogPort& catalog) {
    std::vector<DriverListItem> items;
    for (const auto& name : catalog.loaded_drivers()) {
        DriverListItem item{.name = name, .detail = std::nullopt, .error = {}};
        try {
            item.detail = catalog.describe(name);
        } catch (const std::exception& e) {
            item.error = e.what();
        }
        items.push_back(std::move(item));
    }
    return items;
}

std::vector<InterfaceListItem> list_interfaces(const NetworkInterfaces& interfaces) {
    std::vector<InterfaceListItem> items;
    for (const auto& name : interfaces.names()) {
        items.push_back(InterfaceListItem{.name = name, .addresses = interfaces.addresses(name)});
    }
    return items;
}

DnsResolveOutcome dns_resolve(const DnsResolverPort& resolver, std::string host, std::string type_text) {
    DnsResolveOutcome outcome{.host = std::move(host), .type_text = std::move(type_text), .lookup = std::nullopt};

    const auto type = magic_enum::enum_cast<RecordKind>(outcome.type_text, magic_enum::case_insensitive);
    if (!type.has_value()) {
        return outcome;  // lookup stays nullopt — unknown record type
    }

    outcome.lookup = resolver.resolve(outcome.host, *type);
    return outcome;
}

}  // namespace Diagnostics
