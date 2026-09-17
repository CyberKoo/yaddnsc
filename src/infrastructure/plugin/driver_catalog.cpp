//
// Created by Kotarou on 2026/9/17.
//

#include "driver_catalog.h"

#include <algorithm>
#include <filesystem>

#include "exception/driver_not_found.h"
#include "exception/plugin_load.h"

#include "fmt.hpp"

#include <spdlog/spdlog.h>

namespace {
    [[nodiscard]] std::string_view get_driver_lib_name(std::string_view path) {
        const auto pos = path.rfind('/');
        if (pos == std::string_view::npos) {
            return path;
        }
        return path.substr(pos + 1);
    }
} // anonymous namespace

void DriverCatalog::load_driver(const std::string &path) {
    if (!std::filesystem::exists(path)) {
        throw PluginLoadException(
                fmt::format("Driver library '{}' not found at {}", get_driver_lib_name(path), path));
    }

    auto module = PluginModule::load(path);
    if (!module) {
        throw PluginLoadException(module.error().message);
    }

    const auto driver_name = module->descriptor().name;
    auto [_, inserted] =
            modules_.emplace(driver_name, std::make_shared<const PluginModule>(std::move(*module)));
    if (!inserted) {
        SPDLOG_WARN("Driver '{}' ({}) is already loaded, skipped", driver_name, get_driver_lib_name(path));
        return;
    }

    SPDLOG_DEBUG("Loaded driver '{}' ({})", driver_name, get_driver_lib_name(path));
    [[maybe_unused]] const auto &descriptor = modules_.at(driver_name)->descriptor();
    SPDLOG_TRACE("Driver {} ({}), developed by {}, version: {}", descriptor.name, descriptor.description,
                 descriptor.author, descriptor.version);
}

void DriverCatalog::unload_driver(const std::string &name) {
    if (modules_.erase(name) == 0) {
        throw DriverNotFoundException(fmt::format("Driver '{}' is not loaded, cannot unload", name));
    }
    SPDLOG_INFO("Unloaded driver '{}'", name);
}

std::vector<std::string> DriverCatalog::get_loaded_drivers() const {
    std::vector<std::string> loaded_drivers;
    loaded_drivers.reserve(modules_.size());
    std::ranges::transform(modules_, std::back_inserter(loaded_drivers),
                           [](const auto &kv) -> std::string { return kv.first; });
    return loaded_drivers;
}

std::shared_ptr<const PluginModule> DriverCatalog::find(std::string_view name) const {
    if (const auto it = modules_.find(name); it != modules_.end()) {
        return it->second;
    }
    return nullptr;
}

const DriverDescriptor &DriverCatalog::get_descriptor(std::string_view name) const {
    if (const auto it = modules_.find(name); it != modules_.end()) {
        return it->second->descriptor();
    }
    throw DriverNotFoundException(fmt::format("Driver '{}' is not loaded", name));
}

std::vector<std::string> DriverCatalog::loaded_drivers() const {
    return get_loaded_drivers();
}

DriverDescription DriverCatalog::describe(std::string_view name) const {
    const auto &descriptor = get_descriptor(name);
    return DriverDescription{
        .name = descriptor.name,
        .version = descriptor.version,
        .author = descriptor.author,
        .description = descriptor.description,
    };
}
