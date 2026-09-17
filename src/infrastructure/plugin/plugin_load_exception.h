//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_EXCEPTION_PLUGIN_LOAD_H
#define YADDNSC_EXCEPTION_PLUGIN_LOAD_H

#include "support/exception.h"

/// Thrown when a driver plugin shared library cannot be loaded or fails the
/// v1 alpha ABI checks (missing entry points, magic or api_revision
/// mismatch). Replaces BadDriverException on the plugin boundary.
class PluginLoadException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;

    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "PluginLoadException";
    }
};

#endif // YADDNSC_EXCEPTION_PLUGIN_LOAD_H
