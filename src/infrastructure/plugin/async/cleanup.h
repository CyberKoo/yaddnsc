//
// plugin — fatal policy for unprovable async cleanup.
//

#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_CLEANUP_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_CLEANUP_H

#include <cstdlib>
#include <string_view>

#include <spdlog/spdlog.h>

namespace plugin {

/// The async owner cannot prove an instance was destroyed (destroy threw, its
/// step could not be submitted, or the selected entry group has no destroy).
/// Releasing the module and services would leave the plugin running against
/// freed borrows, so the only safe action is an immediate abort. This is
/// intentionally not a recoverable error path.
[[noreturn]] inline void fatal_async_cleanup_failure(std::string_view driver, std::string_view detail) noexcept {
    try {
        SPDLOG_CRITICAL("Fatal async cleanup failure for '{}': {}", driver, detail);
        SPDLOG_CRITICAL(
            "The async driver instance may still be alive; refusing to release its module, services, "
            "or cycle borrows");
    } catch (...) {
    }
    std::abort();
}

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_CLEANUP_H
