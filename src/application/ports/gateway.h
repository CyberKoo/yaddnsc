#ifndef YADDNSC_APPLICATION_PORTS_GATEWAY_H
#define YADDNSC_APPLICATION_PORTS_GATEWAY_H

#include <string>

#include <expected>

#include "domain/error/error.h"
#include "domain/update/driver_update_command.h"
#include "infrastructure/coro/task.hpp"

namespace app {

/// Driver updates and one-shot configuration diagnostics.
///
/// Failure: DriverError values. Cancellation propagates as `coro::Cancelled`:
/// the await is an abandon point. A driver cycle that already started runs to
/// completion; a cycle not yet started never enters the driver.
/// The port must outlive the task. Thread safety: loop-thread only.
class GatewayPort {
public:
    virtual ~GatewayPort() = default;

    /// Diagnostic path for `config test`, outside the update-loop lifecycle.
    /// Validate driver parameters without updating. A missing validation entry
    /// returns DriverError; cancellation propagates.
    [[nodiscard]] virtual coro::Task<std::expected<void, domain::DriverError>> validate_config(
        std::string driver_name, std::string driver_param_json) = 0;

    [[nodiscard]] virtual coro::Task<std::expected<void, domain::DriverError>> update(
        std::string driver_name, domain::DriverUpdateCommand command) = 0;
};

}  // namespace app

#endif  // YADDNSC_APPLICATION_PORTS_GATEWAY_H
