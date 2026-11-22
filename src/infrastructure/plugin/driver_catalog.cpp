#include "driver_catalog.h"

#include <algorithm>
#include <expected>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>
#include <yaddnsc/util/format.hpp>

#include "domain/driver/capabilities.h"
#include "domain/error/error.h"
#include "infrastructure/plugin/driver_not_found_exception.h"
#include "infrastructure/plugin/plugin_package.h"
#include "support/fmt.hpp"

namespace {
[[nodiscard]] std::string_view driver_library_name(const std::string_view path) {
    const auto pos = path.rfind('/');
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}
}  // namespace

DriverCatalog::DriverCatalog(std::vector<std::shared_ptr<const plugin::PluginPackage>> packages) {
    for (auto& package : packages) {
        register_package(std::move(package));
    }
}

void DriverCatalog::register_package(std::shared_ptr<const plugin::PluginPackage> package) {
    const std::string driver_name = package->descriptor().name;
    const std::string path = package->path();
    auto [_, inserted] = modules_.emplace(driver_name, std::move(package));
    if (!inserted) {
        SPDLOG_WARN("Driver '{}' ({}) is already loaded, skipped", driver_name, driver_library_name(path));
        return;
    }

    SPDLOG_DEBUG("Loaded driver '{}' ({})", driver_name, driver_library_name(path));
    [[maybe_unused]] const auto& descriptor = modules_.at(driver_name)->descriptor();
    SPDLOG_TRACE("Driver {} ({}), developed by {}, version: {}", descriptor.name, descriptor.description,
                 descriptor.author, descriptor.version);
}

void DriverCatalog::unload_driver(const std::string& name) {
    if (modules_.erase(name) == 0) {
        throw DriverNotFoundException(fmt::format("Driver '{}' is not loaded, cannot unload", name));
    }
    SPDLOG_INFO("Unloaded driver '{}'", name);
}

std::shared_ptr<const plugin::PluginPackage> DriverCatalog::find(const std::string_view name) const {
    if (const auto it = modules_.find(name); it != modules_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::string> DriverCatalog::loaded_drivers() const {
    std::vector<std::string> loaded_drivers;
    loaded_drivers.reserve(modules_.size());
    std::ranges::transform(modules_, std::back_inserter(loaded_drivers),
                           [](const auto& entry) -> std::string { return entry.first; });
    return loaded_drivers;
}

std::expected<app::DriverDescription, domain::DriverError> DriverCatalog::describe(const std::string_view name) const {
    const auto package = find(name);
    if (!package) {
        return std::unexpected(domain::DriverError{domain::DriverError::Code::NOT_FOUND, {}});
    }
    const auto& descriptor = package->descriptor();
    return app::DriverDescription{
        .name = descriptor.name,
        .version = descriptor.version,
        .author = descriptor.author,
        .description = descriptor.description,
        .capabilities = package->capabilities(),
        .supports_validate = package->supports_validate(),
    };
}
