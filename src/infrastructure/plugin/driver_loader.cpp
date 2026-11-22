#include "driver_loader.h"

#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <yaddnsc/util/format.hpp>

#include "domain/config/runtime_config.h"
#include "infrastructure/plugin/plugin_error.h"
#include "infrastructure/plugin/plugin_loader.h"
#include "support/fmt.hpp"
#include "support/util/algorithm.hpp"

#include "config_cmake.h"

namespace {
[[nodiscard]] std::filesystem::path default_driver_dir() {
    return {YADDNSC_DEFAULT_DRIVER_DIR};
}

[[nodiscard]] std::filesystem::path resolve_driver_base(const std::optional<std::filesystem::path>& driver_dir) {
    if (driver_dir.has_value()) {
        if (driver_dir->empty()) {
            return {};
        }
        return *driver_dir;
    }
    return default_driver_dir();
}

[[nodiscard]] std::filesystem::path resolve_driver_path(const std::filesystem::path& base_dir,
                                                        const std::string& driver) {
    std::filesystem::path path{driver};
    return path.is_relative() ? base_dir / path : path;
}

[[nodiscard]] std::string_view driver_library_name(const std::string_view path) {
    const auto pos = path.rfind('/');
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

/// Length-aware scan for an embedded NUL in a path's native representation.
///
/// std::filesystem::path keeps every byte it is given; the C APIs behind the
/// filesystem and dlopen calls stop at the first NUL, which would silently
/// change the path that is actually used.
template<typename NativeString>
[[nodiscard]] bool contains_embedded_nul(const NativeString& native) {
    return native.find(static_cast<typename NativeString::value_type>('\0')) != NativeString::npos;
}

[[nodiscard]] plugin::DriverLoadError make_error(const plugin::DriverLoadError::Code code, std::string driver,
                                                 std::string message) {
    return {code, std::move(driver), std::move(message)};
}

[[nodiscard]] plugin::DriverLoadError translate_error(const plugin::PluginError& error, const std::string& path) {
    using Code = plugin::DriverLoadError::Code;
    const auto code = [&error] {
        switch (error.code) {
            case plugin::PluginError::Code::LOAD_FAILED:
                return Code::LOAD_FAILED;
            case plugin::PluginError::Code::MISSING_SYMBOL:
                return Code::MISSING_SYMBOL;
            case plugin::PluginError::Code::ABI_MISMATCH:
                return Code::ABI_MISMATCH;
            case plugin::PluginError::Code::CONTRACT_VIOLATION:
                return Code::CONTRACT_VIOLATION;
        }
        return Code::LOAD_FAILED;
    }();
    return {code, path, std::string(error.diagnostic())};
}

[[nodiscard]] plugin::DriverLoadResult load_one(const std::string& path) {
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error)) {
        if (exists_error) {
            return std::unexpected(
                make_error(plugin::DriverLoadError::Code::LOAD_FAILED, path,
                           fmt::format("Failed to inspect driver '{}': {}", path, exists_error.message())));
        }
        return std::unexpected(
            make_error(plugin::DriverLoadError::Code::LOAD_FAILED, path,
                       fmt::format("Driver library '{}' not found at {}", driver_library_name(path), path)));
    }

    auto loaded = PluginLoader::load(path);
    if (!loaded) {
        return std::unexpected(translate_error(loaded.error(), path));
    }

    plugin::DriverLoadReport report;
    report.packages.push_back(std::move(*loaded));
    return report;
}

void append_one(plugin::DriverLoadReport& destination, plugin::DriverLoadReport&& source) {
    destination.packages.insert(destination.packages.end(), std::make_move_iterator(source.packages.begin()),
                                std::make_move_iterator(source.packages.end()));
    destination.skipped.insert(destination.skipped.end(), std::make_move_iterator(source.skipped.begin()),
                               std::make_move_iterator(source.skipped.end()));
    destination.warnings.insert(destination.warnings.end(), std::make_move_iterator(source.warnings.begin()),
                                std::make_move_iterator(source.warnings.end()));
}

[[nodiscard]] plugin::DriverLoadResult load_auto_discover(const domain::DriverSettings& settings,
                                                          const std::filesystem::path& base_dir) {
    plugin::DriverLoadReport report;
    if (!settings.load.empty()) {
        report.warnings.push_back(fmt::format("auto_discover is enabled, ignoring manual load list with {} entry(ies)",
                                              settings.load.size()));
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(base_dir, error);
    if (error) {
        return std::unexpected(
            make_error(plugin::DriverLoadError::Code::LOAD_FAILED, base_dir.string(),
                       fmt::format("Failed to inspect driver directory '{}': {}", base_dir.string(), error.message())));
    }
    if (!exists) {
        report.warnings.push_back(
            fmt::format("auto_discover enabled but driver_dir '{}' does not exist", base_dir.string()));
        return report;
    }
    if (!std::filesystem::is_directory(base_dir, error)) {
        if (error) {
            return std::unexpected(make_error(
                plugin::DriverLoadError::Code::LOAD_FAILED, base_dir.string(),
                fmt::format("Failed to inspect driver directory '{}': {}", base_dir.string(), error.message())));
        }
        report.warnings.push_back(fmt::format("auto_discover enabled but '{}' is not a directory", base_dir.string()));
        return report;
    }

    try {
        for (const auto& entry : std::filesystem::directory_iterator(base_dir)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".so") {
                continue;
            }
            auto loaded = load_one(entry.path().string());
            if (!loaded) {
                report.skipped.push_back(std::move(loaded.error()));
                continue;
            }
            append_one(report, std::move(*loaded));
        }
    } catch (const std::filesystem::filesystem_error& exception) {
        report.warnings.push_back(
            fmt::format("Failed to iterate driver directory '{}': {}", base_dir.string(), exception.what()));
    }
    return report;
}

}  // namespace

plugin::DriverLoadResult DriverLoader::load(const domain::DriverSettings& settings) {
    // Embedded NUL bytes are rejected before any filesystem or dlopen call:
    // those APIs take a C string and would silently truncate the path at the
    // first NUL, loading a different image than the settings asked for.
    if (settings.driver_dir.has_value() && contains_embedded_nul(settings.driver_dir->native())) {
        return std::unexpected(make_error(plugin::DriverLoadError::Code::INVALID_SETTINGS, {},
                                          "driver_dir contains an embedded NUL byte"));
    }
    for (const auto& driver : settings.load) {
        if (driver.find('\0') != std::string::npos) {
            return std::unexpected(make_error(plugin::DriverLoadError::Code::INVALID_SETTINGS, {},
                                              "driver path contains an embedded NUL byte"));
        }
    }

    const auto base_dir = resolve_driver_base(settings.driver_dir);
    if (base_dir.empty()) {
        return std::unexpected(make_error(plugin::DriverLoadError::Code::INVALID_SETTINGS, {},
                                          "driver_dir is set but empty in configuration"));
    }
    if (settings.auto_discover) {
        return load_auto_discover(settings, base_dir);
    }

    plugin::DriverLoadReport report;
    auto load = settings.load;
    Utils::dedupe(load);
    for (const auto& driver : load) {
        auto loaded = load_one(resolve_driver_path(base_dir, driver).string());
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        append_one(report, std::move(*loaded));
    }
    return report;
}
