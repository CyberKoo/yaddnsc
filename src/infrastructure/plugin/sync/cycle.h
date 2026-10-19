#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_SYNC_CYCLE_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_SYNC_CYCLE_H

#include <memory>
#include <string>

#include "coro/task.hpp"
#include "domain/update/driver_update_command.h"

class PluginModule;

namespace app {
class LoggerPort;
}

namespace plugin {
class Bridge;
struct CallState;

/// Pre-build a shielded sync owner before root admission. Captures module,
/// command and bridge leases; logger must outlive real worker/HTTP drain.
/// Each full create/operation/destroy sequence remains on one worker thread.
/// Allocation may throw; invocation and result publication are loop-thread only.
[[nodiscard]] coro::Task<void> make_sync_update_owner(std::shared_ptr<CallState> state,
                                                      std::shared_ptr<const PluginModule> module,
                                                      domain::DriverUpdateCommand command,
                                                      std::shared_ptr<Bridge> bridge, const app::LoggerPort& logger);
[[nodiscard]] coro::Task<void> make_sync_validate_owner(std::shared_ptr<CallState> state,
                                                        std::shared_ptr<const PluginModule> module,
                                                        std::string driver_param_json, std::shared_ptr<Bridge> bridge,
                                                        const app::LoggerPort& logger);

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_SYNC_CYCLE_H
