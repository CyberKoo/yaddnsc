//
// Created by Kotarou on 2026/9/17.
//

#include "diagnostics.h"

#include <exception>
#include <utility>

#include "application/ports/network_interfaces.h"

namespace Diagnostics {

std::vector<DriverListItem> list_drivers(const DriverCatalogPort& catalog) {
    std::vector<DriverListItem> items;
    for (const auto& name : catalog.loaded_drivers()) {
        DriverListItem item{.name = name, .detail = std::nullopt, .error = {}};
        try {
            item.detail = catalog.describe(name);
        } catch (const std::exception& e) {
            item.error = e.what();
        } catch (...) {
            item.error = "unknown error while describing driver";
        }
        items.push_back(std::move(item));
    }
    return items;
}

std::vector<InterfaceListItem> list_interfaces(const NetworkInterfaces& interfaces) {
    std::vector<InterfaceListItem> items;
    for (const auto& name : interfaces.names()) {
        // names() only yields interfaces from the same cached snapshot that
        // addresses() reads, so a missing entry degrades to an empty row.
        items.push_back(InterfaceListItem{
            .name = name, .addresses = interfaces.addresses(name).value_or(std::vector<InetAddress>{})});
    }
    return items;
}

}  // namespace Diagnostics
