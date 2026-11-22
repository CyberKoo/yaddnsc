#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_ERROR_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_ERROR_H

#include <string>
#include <string_view>

namespace plugin {

/// Plugin loading / ABI-contract failure.
///
/// Loading failures surface as PluginError values from the plugin loader.
/// DriverLoader translates them into its module-owned load error, and
/// composition selects the command presentation; a single update failure is a
/// DriverError.
struct PluginError {
    enum class Code {
        LOAD_FAILED,         ///< dlopen failed (not a loadable module)
        MISSING_SYMBOL,      ///< A required entry point is absent
        ABI_MISMATCH,        ///< Magic number or ABI major/minor mismatch
        CONTRACT_VIOLATION,  ///< The plugin violated the ABI contract at runtime
    };
    Code code;
    std::string message;

    /// Static diagnostic for an error that carries no detailed message.
    ///
    /// It must point to a string literal with static storage duration: the
    /// fallback path exists for failures that cannot afford an allocation, so
    /// the text is borrowed instead of owned.
    std::string_view fallback_message{};

    /// The text to report for this error: the detailed message when there is
    /// one, otherwise the static fallback.
    [[nodiscard]] std::string_view diagnostic() const noexcept {
        return message.empty() ? fallback_message : std::string_view{message};
    }
};

/// Builds an error that carries only a static fallback diagnostic.
///
/// The factory performs no allocation, so it stays usable while an allocation
/// failure is being handled: the returned error owns no text and borrows the
/// fallback from static storage.
[[nodiscard]] inline PluginError make_fallback_error(PluginError::Code code, std::string_view fallback) noexcept {
    return PluginError{code, std::string{}, fallback};
}

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_ERROR_H
