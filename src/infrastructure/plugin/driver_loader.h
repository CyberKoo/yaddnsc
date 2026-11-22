#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_LOADER_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_LOADER_H

#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace domain {
struct DriverSettings;
}

namespace plugin {
class PluginPackage;

struct DriverLoadError {
    enum class Code {
        INVALID_SETTINGS,
        LOAD_FAILED,
        MISSING_SYMBOL,
        ABI_MISMATCH,
        CONTRACT_VIOLATION,
    };

    Code code;
    std::string driver;
    std::string message;
};

struct DriverLoadReport {
    std::vector<std::shared_ptr<const PluginPackage>> packages;
    std::vector<DriverLoadError> skipped;
    std::vector<std::string> warnings;
};

using DriverLoadResult = std::expected<DriverLoadReport, DriverLoadError>;
}  // namespace plugin

/// Discover and validate configured driver packages. The loader has no catalog
/// or configuration-module dependency; composition owns registration and
/// presentation of the returned report.
struct DriverLoader {
    [[nodiscard]] static plugin::DriverLoadResult load(const domain::DriverSettings& settings);
};

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_LOADER_H
