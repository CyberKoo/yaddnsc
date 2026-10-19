#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_CYCLE_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_CYCLE_H

#include <chrono>
#include <memory>
#include <string>

#include "coro/task.hpp"
#include "domain/update/driver_update_command.h"  // IWYU pragma: keep — compiled standalone as a header check, so the include must stay
#include "infrastructure/http/types.h"  // IWYU pragma: keep — compiled standalone as a header check, so the include must stay

class PluginModule;

namespace app {
class LoggerPort;
}

namespace plugin {
struct CallState;

/// Pre-build an async protocol owner before root admission. The owner retains
/// module, services and command through worker/HTTP drain and subscriber exit.
/// Logger must outlive drain. Allocation may throw; loop-thread factories.
[[nodiscard]] coro::Task<void> make_async_update_owner(std::shared_ptr<CallState> state,
                                                       std::shared_ptr<const PluginModule> module,
                                                       domain::DriverUpdateCommand command,
                                                       const app::LoggerPort& logger, http::Options options,
                                                       std::chrono::milliseconds budget);

/// Validation uses the same shielded ABI step boundary, without HTTP admission.
[[nodiscard]] coro::Task<void> make_async_validate_owner(std::shared_ptr<CallState> state,
                                                         std::shared_ptr<const PluginModule> module,
                                                         std::string driver_param_json, const app::LoggerPort& logger);

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_CYCLE_H
