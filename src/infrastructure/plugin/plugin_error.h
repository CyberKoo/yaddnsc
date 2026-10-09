#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_ERROR_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_ERROR_H

#include <string>

namespace plugin {

/// Plugin loading / ABI-contract failure.
///
/// Loading failures surface as PluginError values from the plugin loader and
/// are re-thrown as PluginLoadException at the DriverCatalog boundary (the
/// fail-fast manual-load path); a single update failure is a DriverError.
struct PluginError {
    enum class Code {
        LOAD_FAILED,         ///< dlopen failed (not a loadable module)
        MISSING_SYMBOL,      ///< A required entry point is absent
        ABI_MISMATCH,        ///< Magic number or ABI major/minor mismatch
        CONTRACT_VIOLATION,  ///< The plugin violated the ABI contract at runtime
    };
    Code code;
    std::string message;
};

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_ERROR_H
