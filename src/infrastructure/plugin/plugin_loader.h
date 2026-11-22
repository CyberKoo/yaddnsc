#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_LOADER_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_LOADER_H

#include <expected>
#include <memory>
#include <string>

#include "infrastructure/plugin/plugin_error.h"

namespace plugin {
class PluginPackage;
}

/// Validates one driver image and produces an immutable protocol package.
/// Loading is single-threaded startup work; the returned package is safe to
/// share through its catalog and session leases.
class PluginLoader {
public:
    [[nodiscard]] static std::expected<std::shared_ptr<const plugin::PluginPackage>, plugin::PluginError> load(
        const std::string& path);
};

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_LOADER_H
