#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_LOAD_EXCEPTION_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_LOAD_EXCEPTION_H

#include "support/exception.h"

/// Thrown when a driver plugin shared library cannot be loaded or fails the
/// v1 alpha ABI checks (missing entry points, magic or ABI major/minor
/// mismatch). Replaces BadDriverException on the plugin boundary.
class PluginLoadException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;

    [[nodiscard]] std::string_view get_name() const noexcept override { return "PluginLoadException"; }
};

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_LOAD_EXCEPTION_H
